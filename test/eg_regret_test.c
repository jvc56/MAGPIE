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
