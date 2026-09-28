#include "pat_threat_games_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat_features.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Research harness: game pairs of a static player that re-ranks its top
// moves by a sampled threat check against the same static player without
// it (both with PTG_PAT). For each of its top PTG_K moves within PTG_MARGIN
// points of the best, the treated player plays the move on a copy, deals
// the opponent PTG_R racks from the unseen pool (the same racks for every
// candidate) and takes the opponent's best reply score: over the whole
// board (PTG_MODE full) or only among plays covering an empty double or
// triple word square near any candidate's tiles (PTG_MODE partial). It
// then plays the candidate with the highest equity minus PTG_WEIGHT times
// its mean threat. Each pair plays one seed twice with the seats swapped.
//
// Environment:
//   PTG_OUT          CSV, appended: pair,spread_a,spread_b,changed_a,
//                    changed_b (spreads from the treated player's side;
//                    changed counts decisions the check altered)
//   PTG_WORKER / PTG_NUM_WORKERS   pair sharding
//   PTG_DEADLINE     Unix time; no pair starts at or after it
//   PTG_MODE         full, partial, fast or fastfull (default partial;
//                    see PTGSettings)
//   PTG_WEIGHT       default 0.3
//   PTG_K, PTG_R, PTG_MARGIN   defaults 5, 32, 20
//   PTG_PAT          default CSW24_hsp
//   PTG_SEED         default 20260928

enum {
  PTG_MAX_K = 16,
  PTG_MAX_R = 256,
  PTG_POOL_CAP = 128,
  PTG_NAME_CAP = 64,
  // Pre-move plays kept per rack for fastfull: those within
  // PTG_PRE_MARGIN points of the rack's best, up to PTG_PRE_CAP.
  PTG_PRE_CAP = 64,
  PTG_PRE_MARGIN = 30,
};

static long ptg_env_long(const char *name, long default_value) {
  const char *value = getenv(name);
  return value ? strtol(value, NULL, 10) : default_value;
}

static uint64_t ptg_next(uint64_t *state) {
  *state += 0x9e3779b97f4a7c15ULL;
  uint64_t x = *state;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static void ptg_generate(const Game *game, MoveList *list, move_record_t record,
                         move_sort_t sort, uint64_t lane_mask,
                         const uint32_t *cover, bool use_floor, Equity floor) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = record,
      .move_sort_type = sort,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .lane_mask = lane_mask,
      .lane_cover_masks = cover,
      .use_best_floor = use_floor,
      .best_floor = floor,
  };
  generate_moves(&args);
}

// Marks the empty double and triple word squares within RACK_SIZE of a
// fresh tile of move, in the tile's lane or one lane over, as lanes to
// search and squares a play must cover.
static void ptg_add_units(const Board *board, const Move *move,
                          uint64_t *lane_mask, uint32_t *cover) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int tile_row = move_get_row_start(move) + (vertical ? tile_idx : 0);
    const int tile_col = move_get_col_start(move) + (vertical ? 0 : tile_idx);
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        if (!board_is_empty(board, row, col) ||
            bonus_square_get_word_multiplier(
                board_get_bonus_square(board, row, col)) < 2) {
          continue;
        }
        if (abs(tile_row - row) <= 1 && abs(tile_col - col) <= RACK_SIZE) {
          *lane_mask |= (uint64_t)1 << row;
          cover[row] |= (uint32_t)1 << col;
        }
        if (abs(tile_col - col) <= 1 && abs(tile_row - row) <= RACK_SIZE) {
          *lane_mask |= (uint64_t)1 << (BOARD_DIM + col);
          cover[BOARD_DIM + col] |= (uint32_t)1 << row;
        }
      }
    }
  }
}

typedef struct PTGSettings {
  bool partial;
  // Fast premium check: each candidate searches only its own premium lanes,
  // and each rack's search starts from that rack's best play on the
  // unchanged board (B), so it only looks for plays that beat it; the
  // threat is the mean of max(B, best premium play).
  bool fast;
  // Fast full check: the opponent's best reply over the whole board, found
  // as the better of the best pre-move play the candidate does not disturb
  // and the best play touching the candidate (see ptg_fast_full_threats).
  bool fast_full;
  double weight;
  int k;
  int r;
  double margin;
} PTGSettings;

// A pre-move play of the opponent: its score and the squares it fills.
typedef struct PTGPlay {
  Equity score;
  int num_squares;
  int squares[RACK_SIZE];
} PTGPlay;

static int ptg_move_squares(const Move *move, int *squares) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return 0;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  int count = 0;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = move_get_row_start(move) + (vertical ? tile_idx : 0);
    const int col = move_get_col_start(move) + (vertical ? 0 : tile_idx);
    squares[count++] = row * BOARD_DIM + col;
  }
  return count;
}

static Equity ptg_best_placement_score(const MoveList *list) {
  if (move_list_get_count(list) > 0) {
    const Move *reply = move_list_get_move(list, 0);
    if (move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      return move_get_score(reply);
    }
  }
  return 0;
}

// The fast full check's per-candidate threats: the mean over racks of the
// opponent's best reply score after the candidate. A post-move play either
// puts a tile on A, the empty squares bounding a run through one of the
// candidate's tiles in either direction (every hook, extension and
// playthrough of it), or it is a pre-move play that fills no square of the
// candidate or of A, unchanged in word and score. So the best reply is the
// better of B, the best pre-move play clear of those squares (from each
// rack's plays within PTG_PRE_MARGIN of its best), and the best play on A,
// searched only in A's lanes and only for plays beating B. A rack whose
// listed plays are all disturbed falls back to a full search.
static void ptg_fast_full_threats(const Game *game, const Move *const *cands,
                                  int num_cands, const Rack *racks,
                                  int num_racks, MoveList *reply_list,
                                  MoveList *pre_list, bool verify,
                                  double *threats) {
  static PTGPlay pre[PTG_MAX_R][PTG_PRE_CAP];
  // PTG_PRE_BEST keeps only each rack's best pre-move play; a candidate that
  // disturbs it sends that rack to the full search.
  const bool pre_best_only = getenv("PTG_PRE_BEST") != NULL;
  int pre_count[PTG_MAX_R];
  Game *passed = game_duplicate(game);
  Move *pass = move_create();
  move_set_as_pass(pass);
  play_move(pass, passed, NULL);
  move_destroy(pass);
  Player *passed_opponent =
      game_get_player(passed, game_get_player_on_turn_index(passed));
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    rack_copy(player_get_rack(passed_opponent), &racks[rack_idx]);
    move_list_reset(pre_list);
    const MoveGenArgs args = {
        .game = passed,
        .move_list = pre_list,
        .move_record_type = pre_best_only ? MOVE_RECORD_BEST
                                          : MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = int_to_equity(PTG_PRE_MARGIN),
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);
    move_list_sort_moves(pre_list);
    const int count = move_list_get_count(pre_list);
    pre_count[rack_idx] = count;
    for (int play_idx = 0; play_idx < count; play_idx++) {
      const Move *move = move_list_get_move(pre_list, play_idx);
      PTGPlay *play = &pre[rack_idx][play_idx];
      play->score = move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE
                        ? move_get_score(move)
                        : 0;
      play->num_squares = ptg_move_squares(move, play->squares);
    }
  }
  game_destroy(passed);

  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    Game *after = game_duplicate(game);
    play_move(cands[cand_idx], after, NULL);
    const Board *board = game_get_board(after);
    Player *opponent =
        game_get_player(after, game_get_player_on_turn_index(after));
    bool disturbed[BOARD_DIM * BOARD_DIM] = {false};
    uint64_t lane_mask = 0;
    uint32_t cover[2 * BOARD_DIM] = {0};
    int fresh[RACK_SIZE];
    const int num_fresh = ptg_move_squares(cands[cand_idx], fresh);
    for (int fresh_idx = 0; fresh_idx < num_fresh; fresh_idx++) {
      const int fresh_row = fresh[fresh_idx] / BOARD_DIM;
      const int fresh_col = fresh[fresh_idx] % BOARD_DIM;
      disturbed[fresh[fresh_idx]] = true;
      for (int dir = 0; dir < 2; dir++) {
        for (int side = -1; side <= 1; side += 2) {
          int row = fresh_row;
          int col = fresh_col;
          while (row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM &&
                 !board_is_empty(board, row, col)) {
            row += (dir == 1) ? side : 0;
            col += (dir == 0) ? side : 0;
          }
          if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM ||
              board_get_is_brick(board, row, col)) {
            continue;
          }
          disturbed[row * BOARD_DIM + col] = true;
          lane_mask |= (uint64_t)1 << row;
          cover[row] |= (uint32_t)1 << col;
          lane_mask |= (uint64_t)1 << (BOARD_DIM + col);
          cover[BOARD_DIM + col] |= (uint32_t)1 << row;
        }
      }
    }
    double total = 0.0;
    for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
      rack_copy(player_get_rack(opponent), &racks[rack_idx]);
      Equity value = -1;
      for (int play_idx = 0; play_idx < pre_count[rack_idx]; play_idx++) {
        const PTGPlay *play = &pre[rack_idx][play_idx];
        bool clear = true;
        for (int sq_idx = 0; sq_idx < play->num_squares && clear; sq_idx++) {
          clear = !disturbed[play->squares[sq_idx]];
        }
        if (clear) {
          value = play->score;
          break;
        }
      }
      if (value < 0) {
        // Every listed play is disturbed: search the whole board.
        ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                     NULL, false, 0);
        value = ptg_best_placement_score(reply_list);
      } else if (lane_mask != 0) {
        ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                     lane_mask, cover, true, value);
        const Equity touching = ptg_best_placement_score(reply_list);
        if (touching > value) {
          value = touching;
        }
      }
      if (verify) {
        ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                     NULL, false, 0);
        const Equity check = ptg_best_placement_score(reply_list);
        if (check != value) {
          log_fatal("fastfull mismatch: full %d vs fast %d",
                    equity_to_int(check), equity_to_int(value));
        }
      }
      total += equity_to_double(value);
    }
    threats[cand_idx] = total / num_racks;
    game_destroy(after);
  }
}

// The treated player's move: its top moves re-ranked by the threat check.
// Returns whether the check changed the choice.
static bool ptg_choose(const Game *game, const PTGSettings *settings,
                       MoveList *top_list, MoveList *reply_list,
                       MoveList *pre_list, Move *chosen, uint64_t rng_seed) {
  ptg_generate(game, top_list, MOVE_RECORD_ALL, MOVE_SORT_EQUITY, 0, NULL,
               false, 0);
  move_list_sort_moves(top_list);
  const Move *best = move_list_get_move(top_list, 0);
  move_copy(chosen, best);
  if (bag_get_letters(game_get_bag(game)) == 0 ||
      move_get_type(best) == GAME_EVENT_PASS) {
    return false;
  }
  const double best_equity = equity_to_double(move_get_equity(best));
  int num_cands = 0;
  const Move *cands[PTG_MAX_K];
  for (int move_idx = 0;
       move_idx < move_list_get_count(top_list) && num_cands < settings->k;
       move_idx++) {
    const Move *move = move_list_get_move(top_list, move_idx);
    // A pass's equity is a sentinel, and it is never a threat candidate.
    if (move_get_type(move) == GAME_EVENT_PASS) {
      break;
    }
    if (equity_to_double(move_get_equity(move)) <
        best_equity - settings->margin) {
      break;
    }
    cands[num_cands++] = move;
  }
  if (num_cands < 2) {
    return false;
  }
  const int on_turn = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  pat_compute_unseen_counts(board_get_readonly_lanes(game_get_board(game), 0),
                            ld, player_get_rack(game_get_player(game, on_turn)),
                            unseen);
  MachineLetter pool[PTG_POOL_CAP];
  int pool_size = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    for (int count = 0; count < unseen[ml]; count++) {
      pool[pool_size++] = (MachineLetter)ml;
    }
  }
  const int rack_size = pool_size < RACK_SIZE ? pool_size : RACK_SIZE;
  Rack racks[PTG_MAX_R];
  uint64_t rng = rng_seed;
  for (int rack_idx = 0; rack_idx < settings->r; rack_idx++) {
    MachineLetter shuffled[PTG_POOL_CAP];
    memcpy(shuffled, pool, sizeof(MachineLetter) * (size_t)pool_size);
    rack_set_dist_size_and_reset(&racks[rack_idx], ld_get_size(ld));
    for (int draw_idx = 0; draw_idx < rack_size; draw_idx++) {
      const int pick =
          draw_idx + (int)(ptg_next(&rng) % (uint64_t)(pool_size - draw_idx));
      const MachineLetter tile = shuffled[pick];
      shuffled[pick] = shuffled[draw_idx];
      shuffled[draw_idx] = tile;
      rack_add_letter(&racks[rack_idx], tile);
    }
  }
  uint64_t lane_mask = 0;
  uint32_t cover[2 * BOARD_DIM] = {0};
  if (settings->partial && !settings->fast) {
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      ptg_add_units(game_get_board(game), cands[cand_idx], &lane_mask, cover);
    }
  }
  // B per rack: its best score on the unchanged board, opponent to move.
  Equity baseline[PTG_MAX_R];
  if (settings->fast) {
    Game *passed = game_duplicate(game);
    Move *pass = move_create();
    move_set_as_pass(pass);
    play_move(pass, passed, NULL);
    move_destroy(pass);
    Player *opponent =
        game_get_player(passed, game_get_player_on_turn_index(passed));
    for (int rack_idx = 0; rack_idx < settings->r; rack_idx++) {
      rack_copy(player_get_rack(opponent), &racks[rack_idx]);
      ptg_generate(passed, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                   NULL, false, 0);
      baseline[rack_idx] =
          move_list_get_count(reply_list) > 0
              ? move_get_score(move_list_get_move(reply_list, 0))
              : 0;
    }
    game_destroy(passed);
  }
  double fast_full_threats[PTG_MAX_K];
  if (settings->fast_full) {
    ptg_fast_full_threats(game, cands, num_cands, racks, settings->r,
                          reply_list, pre_list, getenv("PTG_VERIFY") != NULL,
                          fast_full_threats);
  }
  int best_idx = 0;
  double best_value = -1e18;
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    double threat = 0.0;
    if (settings->fast_full) {
      threat = fast_full_threats[cand_idx];
    } else if (settings->fast) {
      uint64_t own_mask = 0;
      uint32_t own_cover[2 * BOARD_DIM] = {0};
      ptg_add_units(game_get_board(game), cands[cand_idx], &own_mask,
                    own_cover);
      Game *after = NULL;
      Player *opponent = NULL;
      if (own_mask != 0) {
        after = game_duplicate(game);
        play_move(cands[cand_idx], after, NULL);
        opponent = game_get_player(after, game_get_player_on_turn_index(after));
      }
      for (int rack_idx = 0; rack_idx < settings->r; rack_idx++) {
        Equity value = baseline[rack_idx];
        if (after != NULL) {
          rack_copy(player_get_rack(opponent), &racks[rack_idx]);
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                       own_mask, own_cover, true, baseline[rack_idx]);
          if (move_list_get_count(reply_list) > 0) {
            const Move *reply = move_list_get_move(reply_list, 0);
            if (move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                move_get_score(reply) > value) {
              value = move_get_score(reply);
            }
          }
        }
        if (after != NULL && getenv("PTG_VERIFY") != NULL) {
          // The floor must only prune: max(B, unrestricted best) agrees.
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                       own_mask, own_cover, false, 0);
          Equity check = baseline[rack_idx];
          if (move_list_get_count(reply_list) > 0) {
            const Move *reply = move_list_get_move(reply_list, 0);
            if (move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                move_get_score(reply) > check) {
              check = move_get_score(reply);
            }
          }
          if (check != value) {
            log_fatal("floor mismatch: %d vs %d", equity_to_int(check),
                      equity_to_int(value));
          }
        }
        threat += equity_to_double(value);
      }
      if (after != NULL) {
        game_destroy(after);
      }
      threat /= settings->r;
    } else if (move_get_type(cands[cand_idx]) ==
                   GAME_EVENT_TILE_PLACEMENT_MOVE &&
               (!settings->partial || lane_mask != 0)) {
      Game *after = game_duplicate(game);
      play_move(cands[cand_idx], after, NULL);
      Player *opponent =
          game_get_player(after, game_get_player_on_turn_index(after));
      for (int rack_idx = 0; rack_idx < settings->r; rack_idx++) {
        rack_copy(player_get_rack(opponent), &racks[rack_idx]);
        if (settings->partial) {
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                       lane_mask, cover, false, 0);
        } else {
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                       NULL, false, 0);
        }
        if (move_list_get_count(reply_list) > 0) {
          const Move *reply = move_list_get_move(reply_list, 0);
          if (move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
            threat += equity_to_double(move_get_score(reply));
          }
        }
      }
      game_destroy(after);
      threat /= settings->r;
    } else if (!settings->partial) {
      // A non-placement leaves the board as it is: the full reply there.
      Game *after = game_duplicate(game);
      play_move(cands[cand_idx], after, NULL);
      Player *opponent =
          game_get_player(after, game_get_player_on_turn_index(after));
      for (int rack_idx = 0; rack_idx < settings->r; rack_idx++) {
        rack_copy(player_get_rack(opponent), &racks[rack_idx]);
        ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                     NULL, false, 0);
        if (move_list_get_count(reply_list) > 0) {
          threat += equity_to_double(
              move_get_score(move_list_get_move(reply_list, 0)));
        }
      }
      game_destroy(after);
      threat /= settings->r;
    }
    const double value = equity_to_double(move_get_equity(cands[cand_idx])) -
                         settings->weight * threat;
    if (value > best_value) {
      best_value = value;
      best_idx = cand_idx;
    }
  }
  move_copy(chosen, cands[best_idx]);
  return best_idx != 0;
}

// Plays one game from seed with treated seat `treated`; returns the treated
// player's final spread and counts changed decisions.
static int ptg_play_game(Game *game, uint64_t seed, int treated,
                         const PTGSettings *settings, MoveList *top_list,
                         MoveList *reply_list, MoveList *plain_list,
                         MoveList *pre_list, Move *chosen, int *changed) {
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, 0);
  draw_starting_racks(game);
  int turn = 0;
  while (!game_over(game)) {
    const int on_turn = game_get_player_on_turn_index(game);
    if (on_turn == treated) {
      if (ptg_choose(game, settings, top_list, reply_list, pre_list, chosen,
                     seed * 1000003 + (uint64_t)turn)) {
        (*changed)++;
      }
      play_move(chosen, game, NULL);
    } else {
      play_move(get_top_equity_move(game, plain_list), game, NULL);
    }
    turn++;
  }
  return equity_to_int(player_get_score(game_get_player(game, treated)) -
                       player_get_score(game_get_player(game, 1 - treated)));
}

void test_pat_threat_games(void) {
  const char *out_path = getenv("PTG_OUT");
  if (!out_path) {
    log_fatal("set PTG_OUT");
  }
  const long worker = ptg_env_long("PTG_WORKER", 0);
  const long num_workers = ptg_env_long("PTG_NUM_WORKERS", 1);
  const long deadline = ptg_env_long("PTG_DEADLINE", 0);
  const char *mode = getenv("PTG_MODE") ? getenv("PTG_MODE") : "partial";
  const char *weight_text = getenv("PTG_WEIGHT");
  PTGSettings settings = {
      .partial = strcmp(mode, "full") != 0,
      .fast = strcmp(mode, "fast") == 0,
      .fast_full = strcmp(mode, "fastfull") == 0,
      .weight = weight_text ? strtod(weight_text, NULL) : 0.3,
      .k = (int)ptg_env_long("PTG_K", 5),
      .r = (int)ptg_env_long("PTG_R", 32),
      .margin = (double)ptg_env_long("PTG_MARGIN", 20),
  };
  if (settings.k > PTG_MAX_K) {
    settings.k = PTG_MAX_K;
  }
  if (settings.r > PTG_MAX_R) {
    settings.r = PTG_MAX_R;
  }
  const char *pat = getenv("PTG_PAT") ? getenv("PTG_PAT") : "CSW24_hsp";
  const uint64_t base_seed = (uint64_t)ptg_env_long("PTG_SEED", 20260928);
  char command[256];
  (void)snprintf(command, sizeof(command),
                 "set -lex CSW24 -leaves CSW24 -wmp true -pat %s -s1 equity "
                 "-s2 equity -r1 all -r2 all -numplays 1 -threads 1",
                 pat);
  Config *config = config_create_or_die(command);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *top_list = move_list_create(PTG_MAX_K * 4);
  MoveList *reply_list = move_list_create(1);
  MoveList *plain_list = move_list_create(1);
  MoveList *pre_list = move_list_create(PTG_PRE_CAP);
  Move *chosen = move_create();
  // Pairs already in the file are skipped, so a restart resumes.
  long done_max = -1;
  FILE *in = fopen(out_path, "r");
  if (in) {
    char line[256];
    while (fgets(line, sizeof(line), in)) {
      const long pair = strtol(line, NULL, 10);
      if (pair > done_max) {
        done_max = pair;
      }
    }
    (void)fclose(in);
  }
  FILE *out = fopen(out_path, "a");
  assert(out);
  for (long pair = worker;; pair += num_workers) {
    if (pair <= done_max) {
      continue;
    }
    if (deadline > 0 && (long)time(NULL) >= deadline) {
      break;
    }
    const uint64_t seed = ptg_next(&(uint64_t){base_seed + (uint64_t)pair});
    int changed_a = 0;
    int changed_b = 0;
    const int spread_a =
        ptg_play_game(game, seed, 0, &settings, top_list, reply_list,
                      plain_list, pre_list, chosen, &changed_a);
    const int spread_b =
        ptg_play_game(game, seed, 1, &settings, top_list, reply_list,
                      plain_list, pre_list, chosen, &changed_b);
    fprintf(out, "%ld,%d,%d,%d,%d\n", pair, spread_a, spread_b, changed_a,
            changed_b);
    (void)fflush(out);
  }
  (void)fclose(out);
  move_destroy(chosen);
  move_list_destroy(plain_list);
  move_list_destroy(pre_list);
  move_list_destroy(reply_list);
  move_list_destroy(top_list);
  config_destroy(config);
}
