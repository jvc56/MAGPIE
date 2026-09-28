#include "pat_threat_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/board.h"
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

// The best move for the player on turn by the given sort, into best.
static bool pth_best(const Game *game, move_sort_t sort, MoveList *list,
                     Move *best) {
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
  const int num_racks = (int)pth_env_long("PTH_RACKS", 64);
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
  fprintf(out, "pos,move,reply_score,reply_equity_pick_score\n");

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
          for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
            ErrorStack *error_stack = error_stack_create();
            ValidatedMoves *vms = validated_moves_create(
                game, 0, cand_move[cand_idx], false, true, error_stack);
            if (!error_stack_is_empty(error_stack) ||
                validated_moves_get_number_of_moves(vms) != 1) {
              validated_moves_destroy(vms);
              error_stack_destroy(error_stack);
              continue;
            }
            error_stack_destroy(error_stack);
            Game *after = game_duplicate(game);
            play_move(validated_moves_get_move(vms, 0), after, NULL);
            validated_moves_destroy(vms);
            Player *opponent =
                game_get_player(after, game_get_player_on_turn_index(after));
            double score_sum = 0.0;
            double pick_sum = 0.0;
            for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
              rack_copy(player_get_rack(opponent), &racks[rack_idx]);
              if (pth_best(after, MOVE_SORT_SCORE, list, reply)) {
                score_sum += equity_to_double(move_get_score(reply));
              }
              if (pth_best(after, MOVE_SORT_EQUITY, list, reply)) {
                pick_sum += equity_to_double(move_get_score(reply));
              }
            }
            fprintf(out, "%ld,%s,%.3f,%.3f\n", current_pos, cand_move[cand_idx],
                    score_sum / num_racks, pick_sum / num_racks);
            game_destroy(after);
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
  move_destroy(reply);
  move_list_destroy(list);
  config_destroy(config);
  (void)fclose(out);
  (void)fclose(in);
  (void)fclose(cgp_in);
}
