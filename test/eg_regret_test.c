// LAB (not for merge): greedy endgame choices against endgame solves.
//
// Plays static self-play games (stock leaves, no pool shift, no pre-endgame
// table, no PAT). At every turn with an empty bag, the mover's top <cands>
// plays by static equity (the greedy policy's ranking: candidate 0 is the
// greedy pick) are each valued by endgame solves of 2 and 4 plies, and of 6
// plies when the two racks hold at most <sixmax> tiles. Each value comes from a
// solve of the position with that candidate as the solve's actual move, so it
// is exact for that depth and shares its search with the best play's value.
//
// Spec: first=<game>,games=<n>,threads=<t>,cands=<k>,sixmax=<tiles>,tt=<0|1>
// tt=1 keeps one small transposition table per thread across its solves;
// tt=0 gives every solve a table of its own (the control).
// Lines printed (values in points of spread from the position, for the mover):
//   P <game> <turn> <mover tiles> <opp tiles> <moves> <b2> <b4> <b6> <cgp>
//   C <game> <turn> <cand> <score> <equity> <tiles played> <goes out> <leave>
//     <v2> <v4> <v6> <seconds>
// b6 and v6 are -99999 when not solved; a pass's equity prints as -99999.
//
// egstuck:<file> reads lines "<id> <cgp>" and, for the player on turn, prints
//   S <id> <stuck> <excess> <most tiles in one move> <rack tiles>
// and, for each of the mover's top 12 plays by static equity (the egregret
// candidates), what the opponent can do on the board after it:
//   O <id> <cand> <score> <tiles played> <game over> <opp can go out>
//     <opp's best out score> <opp's best score> <opp's out plays on the
//     board before the move> <of those, still playable for the same score>
//     <best score among those> <opp's out plays after the move> <equity>
//     <leave>
// egstuck:all:<file> prints O lines for every play, in static order.
//
// egsolve:<file> reads lines "<id> <rank> <cgp>" and prints, for the play of
// that rank in static order, its 4-ply value and the position's:
//   V <id> <rank> <v4> <b4> <score> <tiles played>
// stuck = rack tiles no legal move uses at all; excess = copies of a letter
// beyond the most that any one move uses (blanks count as ?); "-" for none.

#include "eg_regret_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/path_move_lists.h"
#include "../src/ent/letter_distribution.h"
#include "../src/str/letter_distribution_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  EGR_SPEC_FIELDS = 6,
  EGR_MAX_THREADS = 10,
  EGR_MAX_CANDS = 32,
  EGR_NO_VALUE = -99999,
  EGR_MOVE_LIST_CAPACITY = 2000,
  EGR_TT_SIZE_POWER = 20,
  EGR_RACK_STRING = 64,
  EGR_STUCK_LINE = 4096,
  EGR_STUCK_CANDS = 12,
  EGR_STUCK_OUT_PLAYS = 2000,
};

static const uint64_t EGR_GAME_SEED = 91000000;
static const double EGR_OWN_TT_FRACTION = 0.0001;

typedef struct EgrShared {
  int first;
  int games;
  int cands;
  int six_max;
  bool shared_tt;
  int next_game;
  long long positions;
  pthread_mutex_t mutex;
} EgrShared;

typedef struct EgrWorker {
  EgrShared *shared;
  Game *game;
  EndgameCtx *ctx;
  EndgameResults *results;
  TranspositionTable *tt;
} EgrWorker;

// Solves the position to plies with actual as the actual move. Returns the
// best value and sets *actual_value.
static int egr_solve(EgrWorker *worker, int plies, const Move *actual,
                     int *actual_value) {
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  EndgameArgs args;
  endgame_args_fill(
      thread_control, worker->game, EGR_OWN_TT_FRACTION, plies,
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, /*num_threads=*/1,
      /*use_heuristics=*/true, /*num_top_moves=*/1, NULL, NULL, NULL, NULL,
      NULL, NULL, DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/true,
      /*incremental_movegen=*/true, /*disable_best_reply_shortcuts=*/false,
      /*enable_pv_display=*/false, 0.0, 0.0, /*seed=*/1,
      /*skip_word_pruning=*/false, worker->tt, 0, false, 0, false, 0, 0, 0,
      actual, &args);
  ErrorStack *error_stack = error_stack_create();
  endgame_solve(&worker->ctx, &args, worker->results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    log_fatal("egregret: endgame solve failed");
  }
  error_stack_destroy(error_stack);
  thread_control_destroy(thread_control);
  if (!endgame_results_get_actual_move_found(worker->results)) {
    log_fatal("egregret: actual move not found");
  }
  *actual_value =
      endgame_results_get_value(worker->results, ENDGAME_RESULT_ACTUAL);
  return endgame_results_get_value(worker->results, ENDGAME_RESULT_BEST);
}

static void egr_measure(EgrWorker *worker, MoveList *all_moves, int game_idx,
                        int turn) {
  const EgrShared *shared = worker->shared;
  Game *game = worker->game;
  const LetterDistribution *ld = game_get_ld(game);
  const int mover_index = game_get_player_on_turn_index(game);
  const Rack *rack = player_get_rack(game_get_player(game, mover_index));
  const int mover_tiles = rack_get_total_letters(rack);
  const int opp_tiles = rack_get_total_letters(
      player_get_rack(game_get_player(game, 1 - mover_index)));
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = all_moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&gen_args);
  const int num_moves = move_list_get_count(all_moves);
  SortedMoveList *sorted = sorted_move_list_create(all_moves);
  const int num_cands = sorted->count < shared->cands ? sorted->count
                                                      : shared->cands;
  Move cands[EGR_MAX_CANDS];
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    move_copy(&cands[cand_idx], sorted->moves[cand_idx]);
  }
  sorted_move_list_destroy(sorted);
  const bool solve_six = mover_tiles + opp_tiles <= shared->six_max;

  StringBuilder *out = string_builder_create();
  int b2 = 0;
  int b4 = 0;
  int b6 = EGR_NO_VALUE;
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    if (worker->tt == NULL) {
      endgame_ctx_destroy(worker->ctx);
      worker->ctx = NULL;
    }
    const double start = (double)ctimer_monotonic_ns() / 1e9;
    int v2 = 0;
    int v4 = 0;
    int v6 = EGR_NO_VALUE;
    b2 = egr_solve(worker, 2, &cands[cand_idx], &v2);
    b4 = egr_solve(worker, 4, &cands[cand_idx], &v4);
    if (solve_six) {
      b6 = egr_solve(worker, 6, &cands[cand_idx], &v6);
    }
    const double seconds = (double)ctimer_monotonic_ns() / 1e9 - start;
    Rack leave;
    rack_copy(&leave, rack);
    const int tiles_played = move_get_tiles_played(&cands[cand_idx]);
    char leave_string[EGR_RACK_STRING] = "-";
    if (move_get_type(&cands[cand_idx]) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      for (int idx = 0; idx < move_get_tiles_length(&cands[cand_idx]); idx++) {
        const MachineLetter ml = move_get_tile(&cands[cand_idx], idx);
        if (ml != PLAYED_THROUGH_MARKER) {
          rack_take_letter(&leave, get_is_blanked(ml) ? BLANK_MACHINE_LETTER
                                                      : ml);
        }
      }
    }
    if (rack_get_total_letters(&leave) > 0) {
      StringBuilder *leave_sb = string_builder_create();
      string_builder_add_rack(leave_sb, &leave, ld, false);
      (void)snprintf(leave_string, sizeof(leave_string), "%s",
                     string_builder_peek(leave_sb));
      string_builder_destroy(leave_sb);
    }
    string_builder_add_formatted_string(
        out, "C %d %d %d %d %.3f %d %d %s %d %d %d %.3f\n", game_idx, turn,
        cand_idx, equity_to_int(move_get_score(&cands[cand_idx])),
        move_get_type(&cands[cand_idx]) == GAME_EVENT_PASS
            ? (double)EGR_NO_VALUE
            : equity_to_double(move_get_equity(&cands[cand_idx])),
        tiles_played,
        tiles_played == mover_tiles ? 1 : 0, leave_string, v2, v4, v6,
        seconds);
  }
  char *cgp = game_get_cgp(game, true);
  pthread_mutex_lock(&worker->shared->mutex);
  printf("P %d %d %d %d %d %d %d %d %s\n%s", game_idx, turn, mover_tiles,
         opp_tiles, num_moves, b2, b4, b6, cgp, string_builder_peek(out));
  (void)fflush(stdout);
  worker->shared->positions++;
  pthread_mutex_unlock(&worker->shared->mutex);
  free(cgp);
  string_builder_destroy(out);
}

static void *egr_worker_run(void *arg) {
  EgrWorker *worker = (EgrWorker *)arg;
  EgrShared *shared = worker->shared;
  MoveList *all_moves = move_list_create(EGR_MOVE_LIST_CAPACITY);
  MoveList *top_move = move_list_create(1);
  while (true) {
    pthread_mutex_lock(&shared->mutex);
    const int game_idx = shared->first + shared->next_game++;
    pthread_mutex_unlock(&shared->mutex);
    if (game_idx >= shared->first + shared->games) {
      break;
    }
    Game *game = worker->game;
    game_reset(game);
    game_seed(game, EGR_GAME_SEED + (uint64_t)game_idx);
    draw_starting_racks(game);
    int turn = 0;
    while (!game_over(game)) {
      if (bag_get_letters(game_get_bag(game)) == 0) {
        egr_measure(worker, all_moves, game_idx, turn);
      }
      play_move(get_top_equity_move(game, top_move), game, NULL);
      turn++;
    }
  }
  move_list_destroy(top_move);
  move_list_destroy(all_moves);
  return NULL;
}

void eg_regret_run_spec(const char *spec) {
  int first = 0;
  int games = 0;
  int threads = 1;
  int cands = 0;
  int six_max = 0;
  int shared_tt = 1;
  if (sscanf(spec, "first=%d,games=%d,threads=%d,cands=%d,sixmax=%d,tt=%d",
             &first, &games, &threads, &cands, &six_max,
             &shared_tt) != EGR_SPEC_FIELDS ||
      threads < 1 || threads > EGR_MAX_THREADS || cands < 1 ||
      cands > EGR_MAX_CANDS) {
    log_fatal("egregret: bad spec: %s", spec);
  }
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -savesettings false -poolshift 0 "
      "-pegadjust none -pat none");
  EgrShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.first = first;
  shared.games = games;
  shared.cands = cands;
  shared.six_max = six_max;
  shared.shared_tt = shared_tt != 0;
  pthread_mutex_init(&shared.mutex, NULL);
  EgrWorker workers[EGR_MAX_THREADS];
  pthread_t thread_ids[EGR_MAX_THREADS];
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    workers[thread_idx].shared = &shared;
    workers[thread_idx].game = config_game_create(config);
    workers[thread_idx].ctx = NULL;
    workers[thread_idx].results = endgame_results_create();
    workers[thread_idx].tt =
        shared.shared_tt ? transposition_table_create_with_threads(
                               0.0, EGR_TT_SIZE_POWER, 1)
                         : NULL;
  }
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    pthread_create(&thread_ids[thread_idx], NULL, egr_worker_run,
                   &workers[thread_idx]);
  }
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    pthread_join(thread_ids[thread_idx], NULL);
  }
  printf("EGR done games=%d positions=%lld\n", games, shared.positions);
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    if (workers[thread_idx].tt != NULL) {
      transposition_table_destroy(workers[thread_idx].tt);
    }
    endgame_results_destroy(workers[thread_idx].results);
    endgame_ctx_destroy(workers[thread_idx].ctx);
    game_destroy(workers[thread_idx].game);
  }
  config_destroy(config);
}

static bool egs_same_play(const Move *move_1, const Move *move_2) {
  if (move_get_type(move_1) != move_get_type(move_2) ||
      move_get_row_start(move_1) != move_get_row_start(move_2) ||
      move_get_col_start(move_1) != move_get_col_start(move_2) ||
      move_get_dir(move_1) != move_get_dir(move_2) ||
      move_get_tiles_length(move_1) != move_get_tiles_length(move_2) ||
      move_get_score(move_1) != move_get_score(move_2)) {
    return false;
  }
  for (int idx = 0; idx < move_get_tiles_length(move_1); idx++) {
    if (move_get_tile(move_1, idx) != move_get_tile(move_2, idx)) {
      return false;
    }
  }
  return true;
}

// The plays of `player_idx` in `position` that use its whole rack, whoever is
// on turn there: their number, the best one's score and the number of lanes
// (rows and columns) that hold one. Leaves `position` as it was.
static int egs_own_outs(Game *position, int player_idx, MoveList *own_moves,
                        int *best_score, int *num_lanes) {
  const int on_turn = game_get_player_on_turn_index(position);
  game_set_player_on_turn_index(position, player_idx);
  const int rack_tiles = rack_get_total_letters(
      player_get_rack(game_get_player(position, player_idx)));
  const MoveGenArgs own_args = {
      .game = position,
      .move_list = own_moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&own_args);
  game_set_player_on_turn_index(position, on_turn);
  int num_outs = 0;
  uint64_t lanes = 0;
  *best_score = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(own_moves);
       move_idx++) {
    const Move *move = move_list_get_move(own_moves, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_tiles_played(move) != rack_tiles) {
      continue;
    }
    const int score = equity_to_int(move_get_score(move));
    if (num_outs == 0 || score > *best_score) {
      *best_score = score;
    }
    num_outs++;
    const bool vertical = board_is_dir_vertical(move_get_dir(move));
    lanes |= (uint64_t)1 << (vertical ? BOARD_DIM + move_get_col_start(move)
                                      : move_get_row_start(move));
  }
  *num_lanes = __builtin_popcountll(lanes);
  return num_outs;
}

// The mover's own going out next turn after candidate `cand_idx`, whose
// position (the opponent on turn, not over) is `after` and whose replies are
// in `reply_moves`. Prints
//   W <id> <cand> <out plays of the leave> <best such score> <lanes with one>
//     <opp replies> <replies after which the leave still goes out> <the least
//     best out score over the replies, 0 when one stops it> <whether it still
//     goes out after the opp's highest-scoring reply> <best out score then>
//     <the least, over the replies, of what going out gains (out score plus
//     twice the opp's leave, 0 when stopped) less the reply's score>
// The replies exclude the pass; the counts are 0 when the leave has no out
// play. `own_moves` and `after_reply` are scratch.
static void egs_print_own_outs(const char *position_id, int cand_idx,
                               Game *after, const MoveList *reply_moves,
                               MoveList *own_moves, Game *after_reply) {
  const int opp_idx = game_get_player_on_turn_index(after);
  const int mover_idx = 1 - opp_idx;
  const LetterDistribution *ld = game_get_ld(after);
  int best_out = 0;
  int num_lanes = 0;
  const int num_outs =
      egs_own_outs(after, mover_idx, own_moves, &best_out, &num_lanes);
  int num_replies = 0;
  int num_kept = 0;
  int least_kept_out = 0;
  int top_reply_score = -1;
  int top_reply_keeps = 0;
  int top_reply_out = 0;
  int least_net = 0;
  if (num_outs > 0) {
    const int reply_count = move_list_get_count(reply_moves);
    Move *replies = malloc_or_die(sizeof(Move) * (reply_count + 1));
    for (int reply_idx = 0; reply_idx < reply_count; reply_idx++) {
      move_copy(&replies[reply_idx], move_list_get_move(reply_moves, reply_idx));
    }
    for (int reply_idx = 0; reply_idx < reply_count; reply_idx++) {
      const Move *reply = &replies[reply_idx];
      if (move_get_type(reply) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      game_copy(after_reply, after);
      play_move(reply, after_reply, NULL);
      if (game_over(after_reply)) {
        continue;
      }
      int reply_out = 0;
      int reply_lanes = 0;
      const bool kept = egs_own_outs(after_reply, mover_idx, own_moves,
                                     &reply_out, &reply_lanes) > 0;
      const int reply_score = equity_to_int(move_get_score(reply));
      const int opp_leave = equity_to_int(calculate_end_rack_points(
          player_get_rack(game_get_player(after_reply, opp_idx)), ld));
      const int net = (kept ? reply_out + opp_leave : 0) - reply_score;
      if (num_replies == 0 || net < least_net) {
        least_net = net;
      }
      if (num_replies == 0 || (kept ? reply_out : 0) < least_kept_out) {
        least_kept_out = kept ? reply_out : 0;
      }
      if (reply_score > top_reply_score) {
        top_reply_score = reply_score;
        top_reply_keeps = kept ? 1 : 0;
        top_reply_out = kept ? reply_out : 0;
      }
      num_replies++;
      if (kept) {
        num_kept++;
      }
    }
    free(replies);
  }
  printf("W %s %d %d %d %d %d %d %d %d %d %d\n", position_id, cand_idx, num_outs,
         best_out, num_lanes, num_replies, num_kept, least_kept_out,
         top_reply_keeps, top_reply_out, least_net);
}

// A play as the cheap own-out estimate needs it: where it lies, what it
// scores and which rack tiles it places.
typedef struct EgsPlay {
  int lane;
  uint32_t span;
  int score;
  int tiles;
  // The tiles placed, sorted, five bits each (a designated blank counts as
  // the blank).
  uint64_t tiles_key;
  // Twice the face value of the tiles placed.
  int double_face;
  bool is_single;
  int row;
  int col;
} EgsPlay;

static uint64_t egs_tiles_key(const int *counts, int dist_size) {
  uint64_t key = 0;
  for (int ml = 0; ml < dist_size; ml++) {
    for (int copy_idx = 0; copy_idx < counts[ml]; copy_idx++) {
      key = (key << 5) | (uint64_t)(ml + 1);
    }
  }
  return key;
}

static void egs_play_set(EgsPlay *play, const Move *move,
                         const LetterDistribution *ld) {
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int length = move_get_tiles_length(move);
  play->row = move_get_row_start(move);
  play->col = move_get_col_start(move);
  play->lane = vertical ? BOARD_DIM + play->col : play->row;
  play->span = ((UINT32_C(1) << length) - 1) << (vertical ? play->row : play->col);
  play->score = equity_to_int(move_get_score(move));
  play->tiles = move_get_tiles_played(move);
  play->is_single = play->tiles == 1;
  int counts[MAX_ALPHABET_SIZE] = {0};
  play->double_face = 0;
  for (int idx = 0; idx < length; idx++) {
    const MachineLetter ml = move_get_tile(move, idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (play->is_single) {
      // The one square a single tile fills.
      if (vertical) {
        play->row += idx;
      } else {
        play->col += idx;
      }
    }
    const MachineLetter rack_ml = get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml;
    counts[rack_ml]++;
    play->double_face += 2 * equity_to_int(ld_get_score(ld, rack_ml));
  }
  play->tiles_key = egs_tiles_key(counts, ld_get_size(ld));
}

// Whether `play` is still a play, for the same score, after the move whose
// path_move_lists_move_hits are `hits`. A single tile also forms a word across
// its lane, so both of its lanes must stand.
static bool egs_play_stands(const EgsPlay *play, const uint32_t *hits) {
  if ((hits[play->lane] & play->span) != 0) {
    return false;
  }
  if (play->is_single) {
    if (((hits[play->row] >> play->col) & 1) != 0 ||
        ((hits[BOARD_DIM + play->col] >> play->row) & 1) != 0) {
      return false;
    }
  }
  return true;
}

// The cheap estimate of egs_print_own_outs, from the plays of both sides on
// the board before the candidate (`own_plays`, `opp_plays` with each one's
// move hits in `opp_hits`): the leave's out plays are the mover's plays of
// exactly the leave's tiles that the candidate leaves standing, the replies
// are the opponent's plays it leaves standing, and a reply stops an out play
// it does not leave standing. Plays the candidate or a reply creates are
// missed. Prints X with W's fields.
static void egs_print_own_outs_estimate(const char *position_id, int cand_idx,
                                        const Game *game, const Move *cand,
                                        const EgsPlay *own_plays,
                                        int num_own_plays,
                                        const EgsPlay *opp_plays,
                                        const uint32_t *opp_hits,
                                        int num_opp_plays, int opp_tiles,
                                        int opp_double_face, int *standing) {
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  int counts[MAX_ALPHABET_SIZE] = {0};
  const int dist_size = ld_get_size(ld);
  for (int ml = 0; ml < dist_size; ml++) {
    counts[ml] = rack_get_letter(rack, (MachineLetter)ml);
  }
  for (int idx = 0; idx < move_get_tiles_length(cand); idx++) {
    const MachineLetter ml = move_get_tile(cand, idx);
    if (ml != PLAYED_THROUGH_MARKER) {
      counts[get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml]--;
    }
  }
  const uint64_t leave_key = egs_tiles_key(counts, dist_size);
  uint32_t cand_hits[PATH_MOVE_LISTS_NUM_LANES];
  path_move_lists_move_hits(game_get_board(game), cand, cand_hits);
  int num_outs = 0;
  int best_out = 0;
  uint64_t lanes = 0;
  for (int play_idx = 0; play_idx < num_own_plays; play_idx++) {
    const EgsPlay *play = &own_plays[play_idx];
    if (play->tiles_key != leave_key || !egs_play_stands(play, cand_hits)) {
      continue;
    }
    standing[num_outs++] = play_idx;
    if (num_outs == 1 || play->score > best_out) {
      best_out = play->score;
    }
    lanes |= (uint64_t)1 << play->lane;
  }
  int num_replies = 0;
  int num_kept = 0;
  int least_kept_out = 0;
  int top_reply_score = -1;
  int top_reply_keeps = 0;
  int top_reply_out = 0;
  int least_net = 0;
  if (num_outs > 0) {
    for (int reply_idx = 0; reply_idx < num_opp_plays; reply_idx++) {
      const EgsPlay *reply = &opp_plays[reply_idx];
      if (reply->tiles == opp_tiles || !egs_play_stands(reply, cand_hits)) {
        continue;
      }
      const uint32_t *reply_hits =
          &opp_hits[(size_t)reply_idx * PATH_MOVE_LISTS_NUM_LANES];
      bool kept = false;
      int reply_out = 0;
      for (int out_idx = 0; out_idx < num_outs; out_idx++) {
        const EgsPlay *out_play = &own_plays[standing[out_idx]];
        if (egs_play_stands(out_play, reply_hits) &&
            (!kept || out_play->score > reply_out)) {
          kept = true;
          reply_out = out_play->score;
        }
      }
      const int net =
          (kept ? reply_out + opp_double_face - reply->double_face : 0) -
          reply->score;
      if (num_replies == 0 || net < least_net) {
        least_net = net;
      }
      if (num_replies == 0 || reply_out < least_kept_out) {
        least_kept_out = reply_out;
      }
      if (reply->score > top_reply_score) {
        top_reply_score = reply->score;
        top_reply_keeps = kept ? 1 : 0;
        top_reply_out = reply_out;
      }
      num_replies++;
      if (kept) {
        num_kept++;
      }
    }
  }
  printf("X %s %d %d %d %d %d %d %d %d %d %d\n", position_id, cand_idx, num_outs,
         best_out, __builtin_popcountll(lanes), num_replies, num_kept,
         least_kept_out, top_reply_keeps, top_reply_out, least_net);
}

// What the opponent can do after each of the mover's top plays.
static void egs_print_replies(const Game *game, const char *position_id,
                              MoveList *all_moves, MoveList *reply_moves,
                              Game *after, int max_cands) {
  SortedMoveList *sorted = sorted_move_list_create(all_moves);
  const int num_cands = sorted->count < max_cands ? sorted->count : max_cands;
  Move *cands = malloc_or_die(sizeof(Move) * (num_cands + 1));
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *mover_rack =
      player_get_rack(game_get_player(game, game_get_player_on_turn_index(game)));
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    move_copy(&cands[cand_idx], sorted->moves[cand_idx]);
  }
  sorted_move_list_destroy(sorted);
  // The opponent's out plays on the board as it is (after a pass).
  Move *old_outs = malloc_or_die(sizeof(Move) * EGR_STUCK_OUT_PLAYS);
  int num_old_outs = 0;
  // EGS_OWN_OUTS=1 adds the W lines (see egs_print_own_outs).
  const bool own_outs = getenv("EGS_OWN_OUTS") != NULL;
  MoveList *own_moves =
      own_outs ? move_list_create(EGR_MOVE_LIST_CAPACITY) : NULL;
  Game *after_reply = own_outs ? game_duplicate(game) : NULL;
  Move pass;
  move_set_as_pass(&pass);
  game_copy(after, game);
  play_move(&pass, after, NULL);
  if (!game_over(after)) {
    const int opp_tiles = rack_get_total_letters(player_get_rack(
        game_get_player(after, game_get_player_on_turn_index(after))));
    const MoveGenArgs old_args = {
        .game = after,
        .move_list = reply_moves,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&old_args);
    for (int move_idx = 0; move_idx < move_list_get_count(reply_moves);
         move_idx++) {
      const Move *reply = move_list_get_move(reply_moves, move_idx);
      if (move_get_tiles_played(reply) == opp_tiles &&
          num_old_outs < EGR_STUCK_OUT_PLAYS) {
        move_copy(&old_outs[num_old_outs++], reply);
      }
    }
  }
  EgsPlay *own_plays = NULL;
  EgsPlay *opp_plays = NULL;
  uint32_t *opp_hits = NULL;
  int *standing = NULL;
  int num_own_plays = 0;
  int num_opp_plays = 0;
  int opp_tiles_before = 0;
  int opp_double_face = 0;
  if (own_outs) {
    const Board *board = game_get_board(game);
    // reply_moves still holds the opponent's plays after a pass, if any.
    game_copy(after, game);
    play_move(&pass, after, NULL);
    const Rack *opp_rack = player_get_rack(
        game_get_player(game, 1 - game_get_player_on_turn_index(game)));
    opp_tiles_before = rack_get_total_letters(opp_rack);
    opp_double_face = equity_to_int(calculate_end_rack_points(opp_rack, ld));
    if (!game_over(after)) {
      const int reply_count = move_list_get_count(reply_moves);
      opp_plays = malloc_or_die(sizeof(EgsPlay) * (reply_count + 1));
      opp_hits = malloc_or_die(sizeof(uint32_t) * PATH_MOVE_LISTS_NUM_LANES *
                               (reply_count + 1));
      for (int move_idx = 0; move_idx < reply_count; move_idx++) {
        const Move *reply = move_list_get_move(reply_moves, move_idx);
        if (move_get_type(reply) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        egs_play_set(&opp_plays[num_opp_plays], reply, ld);
        path_move_lists_move_hits(
            board, reply,
            &opp_hits[(size_t)num_opp_plays * PATH_MOVE_LISTS_NUM_LANES]);
        num_opp_plays++;
      }
    }
    game_copy(after, game);
    const MoveGenArgs own_args = {
        .game = after,
        .move_list = own_moves,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&own_args);
    const int own_count = move_list_get_count(own_moves);
    own_plays = malloc_or_die(sizeof(EgsPlay) * (own_count + 1));
    standing = malloc_or_die(sizeof(int) * (own_count + 1));
    for (int move_idx = 0; move_idx < own_count; move_idx++) {
      const Move *own_move = move_list_get_move(own_moves, move_idx);
      if (move_get_type(own_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        egs_play_set(&own_plays[num_own_plays++], own_move, ld);
      }
    }
  }
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    game_copy(after, game);
    play_move(&cands[cand_idx], after, NULL);
    int num_survivors = 0;
    int best_survivor_score = 0;
    int num_new_outs = 0;
    int opp_out = 0;
    int best_out_score = 0;
    int best_score = 0;
    const bool over = game_over(after);
    if (!over) {
      const int opp_tiles = rack_get_total_letters(player_get_rack(
          game_get_player(after, game_get_player_on_turn_index(after))));
      const MoveGenArgs reply_args = {
          .game = after,
          .move_list = reply_moves,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
      };
      generate_moves(&reply_args);
      for (int move_idx = 0; move_idx < move_list_get_count(reply_moves);
           move_idx++) {
        const Move *reply = move_list_get_move(reply_moves, move_idx);
        const int score = equity_to_int(move_get_score(reply));
        if (score > best_score) {
          best_score = score;
        }
        if (move_get_tiles_played(reply) == opp_tiles) {
          if (opp_out == 0 || score > best_out_score) {
            best_out_score = score;
          }
          opp_out = 1;
          num_new_outs++;
          for (int old_idx = 0; old_idx < num_old_outs; old_idx++) {
            if (egs_same_play(reply, &old_outs[old_idx])) {
              if (num_survivors == 0 || score > best_survivor_score) {
                best_survivor_score = score;
              }
              num_survivors++;
              break;
            }
          }
        }
      }
    }
    Rack leave;
    rack_copy(&leave, mover_rack);
    if (move_get_type(&cands[cand_idx]) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      for (int idx = 0; idx < move_get_tiles_length(&cands[cand_idx]); idx++) {
        const MachineLetter ml = move_get_tile(&cands[cand_idx], idx);
        if (ml != PLAYED_THROUGH_MARKER) {
          rack_take_letter(&leave, get_is_blanked(ml) ? BLANK_MACHINE_LETTER
                                                      : ml);
        }
      }
    }
    StringBuilder *leave_sb = string_builder_create();
    if (rack_get_total_letters(&leave) > 0) {
      string_builder_add_rack(leave_sb, &leave, ld, false);
    } else {
      string_builder_add_string(leave_sb, "-");
    }
    printf("O %s %d %d %d %d %d %d %d %d %d %d %d %.3f %s\n", position_id,
           cand_idx, equity_to_int(move_get_score(&cands[cand_idx])),
           move_get_tiles_played(&cands[cand_idx]), over ? 1 : 0, opp_out,
           best_out_score, best_score, num_old_outs, num_survivors,
           best_survivor_score, num_new_outs,
           move_get_type(&cands[cand_idx]) == GAME_EVENT_PASS
               ? (double)EGR_NO_VALUE
               : equity_to_double(move_get_equity(&cands[cand_idx])),
           string_builder_peek(leave_sb));
    string_builder_destroy(leave_sb);
    if (!over && own_moves != NULL &&
        move_get_type(&cands[cand_idx]) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      egs_print_own_outs(position_id, cand_idx, after, reply_moves, own_moves,
                         after_reply);
      egs_print_own_outs_estimate(position_id, cand_idx, game, &cands[cand_idx],
                                  own_plays, num_own_plays, opp_plays, opp_hits,
                                  num_opp_plays, opp_tiles_before,
                                  opp_double_face, standing);
    }
  }
  free(own_plays);
  free(opp_plays);
  free(opp_hits);
  free(standing);
  if (own_moves != NULL) {
    move_list_destroy(own_moves);
    game_destroy(after_reply);
  }
  free(cands);
  free(old_outs);
}

static void egs_add_letters(StringBuilder *sb, const LetterDistribution *ld,
                            MachineLetter ml, int count) {
  for (int copy_idx = 0; copy_idx < count; copy_idx++) {
    string_builder_add_user_visible_letter(sb, ld, ml);
  }
}

void eg_stuck_run_file(const char *spec) {
  const bool all_plays = has_prefix("all:", spec);
  const char *path = all_plays ? spec + strlen("all:") : spec;
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -savesettings false -poolshift 0 "
      "-pegadjust none -pat none");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *all_moves = move_list_create(EGR_MOVE_LIST_CAPACITY);
  MoveList *reply_moves = move_list_create(EGR_MOVE_LIST_CAPACITY);
  Game *after = config_game_create(config);
  FILE *in = fopen(path, "r");
  if (in == NULL) {
    log_fatal("egstuck: cannot open %s", path);
  }
  char line[EGR_STUCK_LINE];
  while (fgets(line, sizeof(line), in) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    char *space = strchr(line, ' ');
    if (space == NULL) {
      continue;
    }
    *space = '\0';
    load_cgp_or_die(game, space + 1);
    const Rack *rack =
        player_get_rack(game_get_player(game, game_get_player_on_turn_index(game)));
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = all_moves,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&gen_args);
    int max_used[MAX_ALPHABET_SIZE] = {0};
    int max_tiles = 0;
    for (int move_idx = 0; move_idx < move_list_get_count(all_moves);
         move_idx++) {
      const Move *move = move_list_get_move(all_moves, move_idx);
      if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      if (move_get_tiles_played(move) > max_tiles) {
        max_tiles = move_get_tiles_played(move);
      }
      int used[MAX_ALPHABET_SIZE] = {0};
      for (int idx = 0; idx < move_get_tiles_length(move); idx++) {
        const MachineLetter ml = move_get_tile(move, idx);
        if (ml != PLAYED_THROUGH_MARKER) {
          used[get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml]++;
        }
      }
      for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
        if (used[ml] > max_used[ml]) {
          max_used[ml] = used[ml];
        }
      }
    }
    StringBuilder *stuck = string_builder_create();
    StringBuilder *excess = string_builder_create();
    for (int ml = 0; ml < ld_get_size(ld); ml++) {
      const int count = rack_get_letter(rack, (MachineLetter)ml);
      if (count == 0) {
        continue;
      }
      if (max_used[ml] == 0) {
        egs_add_letters(stuck, ld, (MachineLetter)ml, count);
      } else if (count > max_used[ml]) {
        egs_add_letters(excess, ld, (MachineLetter)ml, count - max_used[ml]);
      }
    }
    printf("S %s %s %s %d %d\n", line,
           string_builder_length(stuck) > 0 ? string_builder_peek(stuck) : "-",
           string_builder_length(excess) > 0 ? string_builder_peek(excess)
                                             : "-",
           max_tiles, rack_get_total_letters(rack));
    string_builder_destroy(stuck);
    string_builder_destroy(excess);
    if (line[0] == 'm') {
      egs_print_replies(game, line, all_moves, reply_moves, after,
                        all_plays ? EGR_MOVE_LIST_CAPACITY : EGR_STUCK_CANDS);
    }
  }
  (void)fclose(in);
  (void)fflush(stdout);
  move_list_destroy(reply_moves);
  move_list_destroy(all_moves);
  game_destroy(after);
  game_destroy(game);
  config_destroy(config);
}

void eg_solve_run_file(const char *path) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -savesettings false -poolshift 0 "
      "-pegadjust none -pat none");
  EgrWorker worker;
  worker.shared = NULL;
  worker.game = config_game_create(config);
  worker.ctx = NULL;
  worker.results = endgame_results_create();
  worker.tt = transposition_table_create_with_threads(0.0, EGR_TT_SIZE_POWER, 1);
  MoveList *all_moves = move_list_create(EGR_MOVE_LIST_CAPACITY);
  FILE *in = fopen(path, "r");
  if (in == NULL) {
    log_fatal("egsolve: cannot open %s", path);
  }
  char line[EGR_STUCK_LINE];
  while (fgets(line, sizeof(line), in) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    char *rank_string = strchr(line, ' ');
    if (rank_string == NULL) {
      continue;
    }
    *rank_string = '\0';
    rank_string++;
    char *cgp = strchr(rank_string, ' ');
    if (cgp == NULL) {
      continue;
    }
    *cgp = '\0';
    cgp++;
    const int rank = atoi(rank_string);
    load_cgp_or_die(worker.game, cgp);
    const MoveGenArgs gen_args = {
        .game = worker.game,
        .move_list = all_moves,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&gen_args);
    SortedMoveList *sorted = sorted_move_list_create(all_moves);
    if (rank < 0 || rank >= sorted->count) {
      log_fatal("egsolve: no play of rank %d in %s", rank, line);
    }
    Move move;
    move_copy(&move, sorted->moves[rank]);
    sorted_move_list_destroy(sorted);
    int v4 = 0;
    const int b4 = egr_solve(&worker, 4, &move, &v4);
    printf("V %s %d %d %d %d %d\n", line, rank, v4, b4,
           equity_to_int(move_get_score(&move)), move_get_tiles_played(&move));
    (void)fflush(stdout);
  }
  (void)fclose(in);
  move_list_destroy(all_moves);
  transposition_table_destroy(worker.tt);
  endgame_results_destroy(worker.results);
  endgame_ctx_destroy(worker.ctx);
  game_destroy(worker.game);
  config_destroy(config);
}
