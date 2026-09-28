#include "pat_threat_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat_features.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Research harness for a threat check: for each candidate a patsimdiff run
// simmed, the opponent's best reply to it, averaged over racks dealt from
// the unseen pool (the same racks for every candidate of a position).
// PAT is trained to predict exactly this, the opponent's next reply score,
// so a direct estimate shows how much of the sim's verdict a per-move
// reply check could recover.
//
// Environment:
//   PTH_IN      a patsimdiff CSV (its <PTH_IN>.cgp holds the positions)
//   PTH_OUT     output CSV: pos,move,reply_score,reply_equity_pick_score
//   PTH_RACKS   racks per position (default 64)
//   PTH_MIN_BAG positions with a smaller bag are skipped (default 15)
//   PTH_WORKER / PTH_NUM_WORKERS  position sharding
//
// reply_score is the mean of the opponent's highest-scoring reply;
// reply_equity_pick_score is the mean score of the reply they would pick
// by static equity (score plus leave), which is how the sim's rollout
// players choose.

enum {
  PTH_LINE_CAP = 65536,
  PTH_MAX_CANDS = 64,
  PTH_MOVE_CAP = 64,
  PTH_POOL_CAP = 128,
  PTH_MAX_RACKS = 1024,
};

static long pth_env_long(const char *name, long default_value) {
  const char *value = getenv(name);
  return value ? strtol(value, NULL, 10) : default_value;
}

static uint64_t pth_next(uint64_t *state) {
  *state += 0x9e3779b97f4a7c15ULL;
  uint64_t x = *state;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// The best move for the player on turn by the given sort, into best,
// restricted to lane_mask and lane_cover_masks when set (see MoveGenArgs).
// The tile-count mask and exchange skipping pth_best passes (see
// MoveGenArgs); set per variant by the PTH_VARIANTS timing mode.
static uint32_t pth_tiles_played_mask = 0;
static bool pth_skip_exchanges = false;

static bool pth_best(const Game *game, move_sort_t sort, MoveList *list,
                     Move *best, uint64_t lane_mask,
                     const uint32_t *lane_cover_masks) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = sort,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .lane_mask = lane_mask,
      .lane_cover_masks = lane_cover_masks,
      .tiles_played_mask = pth_tiles_played_mask,
      .skip_exchanges = pth_skip_exchanges,
  };
  generate_moves(&args);
  if (move_list_get_count(list) == 0) {
    return false;
  }
  move_copy(best, move_list_get_move(list, 0));
  return true;
}

void test_pat_threat(void) {
  const char *in_path = getenv("PTH_IN");
  const char *out_path = getenv("PTH_OUT");
  if (!in_path || !out_path) {
    log_fatal("set PTH_IN and PTH_OUT");
  }
  const int num_racks = (int)pth_env_long("PTH_RACKS", 64) < PTH_MAX_RACKS
                            ? (int)pth_env_long("PTH_RACKS", 64)
                            : PTH_MAX_RACKS;
  const int min_bag = (int)pth_env_long("PTH_MIN_BAG", 15);
  const long worker = pth_env_long("PTH_WORKER", 0);
  const long num_workers = pth_env_long("PTH_NUM_WORKERS", 1);

  // Candidates per position from the patsimdiff CSV: pos,bag,move,...
  char *cgp_path = get_formatted_string("%s.cgp", in_path);
  FILE *cgp_in = fopen(cgp_path, "r");
  free(cgp_path);
  FILE *in = fopen(in_path, "r");
  assert(cgp_in && in);
  FILE *out = fopen(out_path, "w");
  assert(out);
  fprintf(out, "pos,move,reply_score,reply_equity_pick_score,partial_score,"
               "partial_hinge_score\n");
  const bool full = pth_env_long("PTH_FULL", 1) != 0;
  // PTH_VARIANTS: comma-separated search variants timed against each other
  // on every candidate and rack (full board, best by score): "all" (every
  // play and exchanges), "noexch" (every play), or tile counts such as
  // "1267" (only those counts, no exchanges). The first is the reference
  // the others' best scores are checked against.
  enum { PTH_MAX_VARIANTS = 16 };
  char variant_names[PTH_MAX_VARIANTS][16];
  uint32_t variant_masks[PTH_MAX_VARIANTS];
  bool variant_skip[PTH_MAX_VARIANTS];
  double variant_seconds[PTH_MAX_VARIANTS] = {0};
  long variant_matches[PTH_MAX_VARIANTS] = {0};
  double variant_deficit[PTH_MAX_VARIANTS] = {0};
  long variant_calls = 0;
  int num_variants = 0;
  if (getenv("PTH_VARIANTS")) {
    char list_text[256];
    (void)snprintf(list_text, sizeof(list_text), "%s", getenv("PTH_VARIANTS"));
    for (char *token = strtok(list_text, ",");
         token != NULL && num_variants < PTH_MAX_VARIANTS;
         token = strtok(NULL, ",")) {
      (void)snprintf(variant_names[num_variants], 16, "%s", token);
      variant_masks[num_variants] = 0;
      variant_skip[num_variants] = strcmp(token, "all") != 0;
      if (strcmp(token, "all") != 0 && strcmp(token, "noexch") != 0) {
        for (const char *digit = token; *digit; digit++) {
          variant_masks[num_variants] |= (uint32_t)1 << (*digit - '0');
        }
      }
      num_variants++;
    }
  }
  double full_seconds = 0.0;
  double partial_seconds = 0.0;
  double baseline_seconds = 0.0;
  long partial_calls = 0;
  long full_calls = 0;

  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1 -threads 1");
  MoveList *list = move_list_create(1);
  Move *reply = move_create();
  char line[PTH_LINE_CAP];
  char *header = fgets(line, sizeof(line), in) ? string_duplicate(line) : NULL;
  assert(header);
  free(header);
  // Read all candidate rows once; they are grouped by position.
  long cand_pos[PTH_MAX_CANDS];
  char cand_move[PTH_MAX_CANDS][PTH_MOVE_CAP];
  int num_cands = 0;
  long current_pos = -1;
  int current_bag = 0;
  bool more = true;
  char cgp_line[PTH_LINE_CAP];
  long position_index = 0;
  while (more) {
    char *row = fgets(line, sizeof(line), in);
    long pos = -1;
    int bag = 0;
    char move_text[PTH_MOVE_CAP] = {0};
    if (row) {
      char *end = NULL;
      pos = strtol(row, &end, 10);
      if (end == row || *end != ',' || strncmp(end + 1, "skip", 4) == 0) {
        continue;
      }
      bag = (int)strtol(end + 1, &end, 10);
      const char *move_start = end + 1;
      const char *move_end = strchr(move_start, ',');
      const size_t length = (size_t)(move_end - move_start);
      memcpy(move_text, move_start,
             length < PTH_MOVE_CAP - 1 ? length : PTH_MOVE_CAP - 1);
    } else {
      more = false;
    }
    if (pos != current_pos && current_pos >= 0) {
      // Process the finished position.
      if (current_bag >= min_bag && position_index % num_workers == worker) {
        // Find its CGP.
        rewind(cgp_in);
        bool found = false;
        while (fgets(cgp_line, sizeof(cgp_line), cgp_in)) {
          char *end = NULL;
          if (strtol(cgp_line, &end, 10) == current_pos && *end == ',') {
            end[strlen(end) - 1] = '\0';
            char *command = get_formatted_string("cgp %s", end + 1);
            load_and_exec_config_or_die(config, command);
            free(command);
            found = true;
            break;
          }
        }
        if (found) {
          Game *game = config_get_game(config);
          const LetterDistribution *ld = game_get_ld(game);
          uint8_t unseen[MAX_ALPHABET_SIZE];
          pat_compute_unseen_counts(
              board_get_readonly_lanes(game_get_board(game), 0), ld,
              player_get_rack(game_get_player(game, 0)), unseen);
          MachineLetter pool[PTH_POOL_CAP];
          int pool_size = 0;
          for (int ml = 0; ml < ld_get_size(ld); ml++) {
            for (int count = 0; count < unseen[ml]; count++) {
              pool[pool_size++] = (MachineLetter)ml;
            }
          }
          const int rack_size = pool_size < RACK_SIZE ? pool_size : RACK_SIZE;
          // The same racks for every candidate of the position.
          Rack *racks = malloc_or_die(sizeof(Rack) * (size_t)num_racks);
          uint64_t rng = (uint64_t)current_pos * 7919 + 17;
          for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
            MachineLetter shuffled[PTH_POOL_CAP];
            memcpy(shuffled, pool, sizeof(MachineLetter) * (size_t)pool_size);
            rack_set_dist_size_and_reset(&racks[rack_idx], ld_get_size(ld));
            for (int draw_idx = 0; draw_idx < rack_size; draw_idx++) {
              const int pick =
                  draw_idx +
                  (int)(pth_next(&rng) % (uint64_t)(pool_size - draw_idx));
              const MachineLetter tile = shuffled[pick];
              shuffled[pick] = shuffled[draw_idx];
              shuffled[draw_idx] = tile;
              rack_add_letter(&racks[rack_idx], tile);
            }
          }
          // Pass 1: the candidates, and the premium units any of them
          // reaches: an empty double or triple word square with a fresh
          // tile in its lane, or one lane over, within RACK_SIZE squares.
          Move *moves[PTH_MAX_CANDS];
          int num_valid = 0;
          int valid_idx[PTH_MAX_CANDS];
          uint64_t lane_mask = 0;
          uint32_t cover[2 * BOARD_DIM] = {0};
          const Board *board = game_get_board(game);
          for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
            ErrorStack *error_stack = error_stack_create();
            ValidatedMoves *vms = validated_moves_create(
                game, 0, cand_move[cand_idx], false, true, error_stack);
            if (error_stack_is_empty(error_stack) &&
                validated_moves_get_number_of_moves(vms) == 1) {
              moves[num_valid] = move_create();
              move_copy(moves[num_valid], validated_moves_get_move(vms, 0));
              valid_idx[num_valid++] = cand_idx;
            }
            validated_moves_destroy(vms);
            error_stack_destroy(error_stack);
          }
          for (int valid = 0; valid < num_valid; valid++) {
            const Move *move = moves[valid];
            if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
              continue;
            }
            const bool vertical = board_is_dir_vertical(move_get_dir(move));
            for (int tile_idx = 0; tile_idx < move_get_tiles_length(move);
                 tile_idx++) {
              if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
                continue;
              }
              const int tile_row =
                  move_get_row_start(move) + (vertical ? tile_idx : 0);
              const int tile_col =
                  move_get_col_start(move) + (vertical ? 0 : tile_idx);
              for (int row = 0; row < BOARD_DIM; row++) {
                for (int col = 0; col < BOARD_DIM; col++) {
                  if (!board_is_empty(board, row, col) ||
                      bonus_square_get_word_multiplier(
                          board_get_bonus_square(board, row, col)) < 2) {
                    continue;
                  }
                  // Horizontal unit: lane row, position col.
                  if (abs(tile_row - row) <= 1 &&
                      abs(tile_col - col) <= RACK_SIZE) {
                    lane_mask |= (uint64_t)1 << row;
                    cover[row] |= (uint32_t)1 << col;
                  }
                  // Vertical unit: lane col, position row.
                  if (abs(tile_col - col) <= 1 &&
                      abs(tile_row - row) <= RACK_SIZE) {
                    lane_mask |= (uint64_t)1 << (BOARD_DIM + col);
                    cover[BOARD_DIM + col] |= (uint32_t)1 << row;
                  }
                }
              }
            }
          }
          // B: each rack's best score on the unchanged board (after a pass,
          // so the opponent is on turn).
          double baseline[PTH_MAX_RACKS];
          {
            Game *passed = game_duplicate(game);
            Move *pass = move_create();
            move_set_as_pass(pass);
            play_move(pass, passed, NULL);
            move_destroy(pass);
            Player *opponent =
                game_get_player(passed, game_get_player_on_turn_index(passed));
            const clock_t start = clock();
            for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
              rack_copy(player_get_rack(opponent), &racks[rack_idx]);
              baseline[rack_idx] =
                  pth_best(passed, MOVE_SORT_SCORE, list, reply, 0, NULL)
                      ? equity_to_double(move_get_score(reply))
                      : 0.0;
            }
            baseline_seconds += (double)(clock() - start) / CLOCKS_PER_SEC;
            game_destroy(passed);
          }
          // Pass 2: per candidate, the full reply and the premium-connected
          // one.
          for (int valid = 0; valid < num_valid; valid++) {
            Game *after = game_duplicate(game);
            play_move(moves[valid], after, NULL);
            Player *opponent =
                game_get_player(after, game_get_player_on_turn_index(after));
            double score_sum = 0.0;
            double pick_sum = 0.0;
            double partial_sum = 0.0;
            double hinge_sum = 0.0;
            for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
              rack_copy(player_get_rack(opponent), &racks[rack_idx]);
              if (num_variants > 0) {
                Equity reference = 0;
                for (int variant = 0; variant < num_variants; variant++) {
                  pth_tiles_played_mask = variant_masks[variant];
                  pth_skip_exchanges = variant_skip[variant];
                  const clock_t start = clock();
                  Equity score = 0;
                  if (pth_best(after, MOVE_SORT_SCORE, list, reply, 0, NULL) &&
                      move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
                    score = move_get_score(reply);
                  }
                  variant_seconds[variant] +=
                      (double)(clock() - start) / CLOCKS_PER_SEC;
                  if (variant == 0) {
                    reference = score;
                  }
                  variant_matches[variant] += (score == reference) ? 1 : 0;
                  variant_deficit[variant] +=
                      equity_to_double(reference - score);
                }
                pth_tiles_played_mask = 0;
                pth_skip_exchanges = false;
                variant_calls++;
                continue;
              }
              if (full) {
                const clock_t start = clock();
                if (pth_best(after, MOVE_SORT_SCORE, list, reply, 0, NULL)) {
                  score_sum += equity_to_double(move_get_score(reply));
                }
                full_seconds += (double)(clock() - start) / CLOCKS_PER_SEC;
                full_calls++;
                if (pth_best(after, MOVE_SORT_EQUITY, list, reply, 0, NULL)) {
                  pick_sum += equity_to_double(move_get_score(reply));
                }
              }
              double partial = 0.0;
              if (lane_mask != 0) {
                const clock_t start = clock();
                if (pth_best(after, MOVE_SORT_SCORE, list, reply, lane_mask,
                             cover) &&
                    move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
                  partial = equity_to_double(move_get_score(reply));
                }
                partial_seconds += (double)(clock() - start) / CLOCKS_PER_SEC;
                partial_calls++;
              }
              partial_sum += partial;
              hinge_sum +=
                  partial > baseline[rack_idx] ? partial : baseline[rack_idx];
            }
            fprintf(out, "%ld,%s,%.3f,%.3f,%.3f,%.3f\n", current_pos,
                    cand_move[valid_idx[valid]], score_sum / num_racks,
                    pick_sum / num_racks, partial_sum / num_racks,
                    hinge_sum / num_racks);
            game_destroy(after);
          }
          for (int valid = 0; valid < num_valid; valid++) {
            move_destroy(moves[valid]);
          }
          (void)fflush(out);
          free(racks);
        }
      }
      position_index++;
      num_cands = 0;
    }
    if (row && num_cands < PTH_MAX_CANDS) {
      current_pos = pos;
      current_bag = bag;
      cand_pos[num_cands] = pos;
      (void)snprintf(cand_move[num_cands], PTH_MOVE_CAP, "%s", move_text);
      num_cands++;
    }
  }
  (void)cand_pos;
  for (int variant = 0; variant < num_variants; variant++) {
    fprintf(stderr,
            "variant %-8s %8.2f us/call  matches reference %6.2f%%  mean "
            "shortfall %.3f pts\n",
            variant_names[variant],
            1e6 * variant_seconds[variant] / (double)variant_calls,
            100.0 * (double)variant_matches[variant] / (double)variant_calls,
            variant_deficit[variant] / (double)variant_calls);
  }
  fprintf(stderr,
          "timing: full %.3f s over %ld calls (%.1f us each); partial %.3f s "
          "over %ld calls (%.1f us each); baseline %.3f s\n",
          full_seconds, full_calls,
          full_calls ? 1e6 * full_seconds / (double)full_calls : 0.0,
          partial_seconds, partial_calls,
          partial_calls ? 1e6 * partial_seconds / (double)partial_calls : 0.0,
          baseline_seconds);
  move_destroy(reply);
  move_list_destroy(list);
  config_destroy(config);
  (void)fclose(out);
  (void)fclose(in);
  (void)fclose(cgp_in);
}
