// Decision-level comparison of search arms at a real per-move budget.
//
// infdecide:GAMES:MOVE_MS:THREADS:SEED:ARMS:PLAYOUTS
//
// Self-play games in which every move is chosen by the control search (the
// engine's default sim, no inference, MOVE_MS per move on THREADS threads).
// On every turn where inference is possible the same position is also
// searched by each extra arm in ARMS:
//   s  the same search with softmax inference (temperature 8)
//   p  the same search told the opponent's actual rack (perfect information)
//   c  the control search again with another seed (negative control)
//   t  the top static-equity move (positive control, free)
// Where an arm picks a different move from the control, both moves are scored
// against the opponent's actual rack two ways:
//   playout oracle: PLAYOUTS full games to the end with static play for both
//     sides, the unseen bag reshuffled each time, same shuffles for each move;
//   sim oracle: an equal-sample (round robin) sim of just those moves with
//     the opponent's rack known, through the engine's own rollout evaluator.
// The control's move is always the one played. The game stops when the bag
// empties: the opponent's rack is then known to everyone.

#include "infer_decide_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  DECIDE_MAX_ARMS = 5,
  DECIDE_MAX_TURNS = 200,
  DECIDE_PLAYOUT_MAX_TURNS = 120,
  DECIDE_MOVE_STR = 96,
  DECIDE_ORACLE_SIM_ROLLOUTS_PER_MOVE = 30000,
};

typedef struct DecideArm {
  char code;
  bool ran;
  bool simulated;
  Move move;
  char move_str[DECIDE_MOVE_STR];
  uint64_t iterations;
  double seconds;
  SimCtx *sim_ctx;
  // Index into the distinct-move table of the position.
  int distinct_index;
} DecideArm;

typedef struct PlayoutJob {
  const Game *base;
  const Move *moves;
  int num_moves;
  int mover;
  uint64_t seed_base;
  int first;
  int last;
  // [move][playout]
  float *win;
  float *spread;
  int playouts;
} PlayoutJob;

static double decide_win(double spread) {
  if (spread > 0) {
    return 1;
  }
  if (spread < 0) {
    return 0;
  }
  return 0.5;
}

static void *decide_playout_worker(void *context) {
  PlayoutJob *job = context;
  Game *work = game_duplicate(job->base);
  MoveList *best = move_list_create(1);
  for (int playout = job->first; playout < job->last; playout++) {
    for (int move_idx = 0; move_idx < job->num_moves; move_idx++) {
      game_copy(work, job->base);
      // Reshuffles the unseen bag; the opponent keeps its actual rack.
      game_seed(work, job->seed_base + (uint64_t)playout);
      play_move(&job->moves[move_idx], work, NULL);
      for (int turn = 0; !game_over(work) && turn < DECIDE_PLAYOUT_MAX_TURNS;
           turn++) {
        const Move *reply = get_top_equity_move(work, best);
        play_move(reply, work, NULL);
      }
      const double spread = equity_to_double(
          player_get_score(game_get_player(work, job->mover)) -
          player_get_score(game_get_player(work, 1 - job->mover)));
      const size_t slot =
          ((size_t)move_idx * (size_t)job->playouts) + (size_t)playout;
      job->spread[slot] = (float)spread;
      job->win[slot] = (float)decide_win(spread);
    }
  }
  move_list_destroy(best);
  game_destroy(work);
  return NULL;
}

static void decide_move_string(const Game *game, const Move *move, char *dest) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          true);
  const char *text = string_builder_peek(sb);
  size_t out = 0;
  for (size_t in = 0; text[in] != '\0' && out + 1 < DECIDE_MOVE_STR; in++) {
    dest[out++] = text[in] == ' ' ? '_' : text[in];
  }
  dest[out] = '\0';
  string_builder_destroy(sb);
}

static void decide_rack_string(const Game *game, const Rack *rack, char *dest,
                               size_t dest_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, rack, game_get_ld(game), false);
  const char *text = string_builder_peek(sb);
  snprintf(dest, dest_size, "%s", text[0] == '\0' ? "-" : text);
  string_builder_destroy(sb);
}

void infer_decide_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  assert(string_splitter_get_number_of_items(fields) == 6);
  ErrorStack *errors = error_stack_create();
  const int games = string_to_int(string_splitter_get_item(fields, 0), errors);
  const int move_ms =
      string_to_int(string_splitter_get_item(fields, 1), errors);
  const int threads =
      string_to_int(string_splitter_get_item(fields, 2), errors);
  const uint64_t seed =
      string_to_uint64(string_splitter_get_item(fields, 3), errors);
  char arm_codes[DECIDE_MAX_ARMS + 1];
  snprintf(arm_codes, sizeof(arm_codes), "%s",
           string_splitter_get_item(fields, 4));
  const int playouts =
      string_to_int(string_splitter_get_item(fields, 5), errors);
  assert(error_stack_is_empty(errors) && games > 0 && move_ms > 0 &&
         threads > 0 && playouts > 1);
  string_splitter_destroy(fields);
  error_stack_destroy(errors);
  const int num_extra = (int)strlen(arm_codes);
  for (int idx = 0; idx < num_extra; idx++) {
    assert(strchr("spct", arm_codes[idx]) != NULL);
  }
  const double move_seconds = move_ms / 1000.0;

  // Engine defaults for the search (candidates, plies, stopping rule, PAT),
  // with the tables on. INFDEC_SET appends settings, e.g. "-plies 2".
  const char *extra_settings = getenv("INFDEC_SET");
  char *settings = get_formatted_string(
      "set -lex CSW24 -leaves CSW24 -wmp true -rit true -ritmmap true -wit "
      "true -s1 equity -s2 equity -r1 all -r2 all -pat auto -threads %d "
      "-numplays 100 -minp 500 -savesettings false -autosavegcg false %s",
      threads, extra_settings ? extra_settings : "");
  Config *config = config_create_or_die(settings);
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex CSW24;");
  errors = error_stack_create();
  config_load_win_pcts(config, errors);
  assert(error_stack_is_empty(errors) && config_get_win_pcts(config) != NULL);
  Game *game = config_get_game(config);
  for (int seat = 0; seat < 2; seat++) {
    const Player *player = game_get_player(game, seat);
    if (player_get_pat(player) == NULL || player_get_pat_disabled(player)) {
      log_fatal("infdecide: PAT not active for player %d", seat);
    }
  }
  const int num_plays = config_get_num_plays(config);
  printf("INFDEC_SETTINGS games=%d move_ms=%d threads=%d seed=%llu arms=c0%s "
         "playouts=%d plies=%d candidates=%d oracle_sim_rollouts=%d "
         "temperature=8 margin=10000 lex=CSW24 settings=\"%s\"\n",
         games, move_ms, threads, (unsigned long long)seed, arm_codes,
         playouts, config_get_plies(config), num_plays,
         DECIDE_ORACLE_SIM_ROLLOUTS_PER_MOVE, settings);
  free(settings);
  fflush(stdout);

  const LetterDistribution *ld = game_get_ld(game);
  Rack played;
  Rack known;
  Rack observer;
  Rack opp_actual;
  Rack no_rack;
  rack_set_dist_size_and_reset(&played, ld_get_size(ld));
  rack_set_dist_size_and_reset(&known, ld_get_size(ld));
  rack_set_dist_size_and_reset(&observer, ld_get_size(ld));
  rack_set_dist_size_and_reset(&opp_actual, ld_get_size(ld));
  rack_set_dist_size_and_reset(&no_rack, ld_get_size(ld));
  MoveList *moves = move_list_create(num_plays);
  MoveList *static_best = move_list_create(1);
  MoveList *oracle_moves = move_list_create(DECIDE_MAX_ARMS);
  SimResults *results = sim_results_create(0);
  SimResults *oracle_results = sim_results_create(0);
  InferenceResults *inferences = inference_results_create(NULL);
  ThreadControl *control = config_get_thread_control(config);
  // Arm 0 is the control; the rest follow arm_codes.
  DecideArm arms[DECIDE_MAX_ARMS + 1] = {0};
  const int num_arms = num_extra + 1;
  arms[0].code = '0';
  for (int idx = 0; idx < num_extra; idx++) {
    arms[idx + 1].code = arm_codes[idx];
  }
  float *playout_win =
      malloc_or_die(sizeof(float) * (size_t)num_arms * (size_t)playouts);
  float *playout_spread =
      malloc_or_die(sizeof(float) * (size_t)num_arms * (size_t)playouts);
  PlayoutJob *jobs = malloc_or_die(sizeof(PlayoutJob) * (size_t)threads);
  cpthread_t *workers = malloc_or_die(sizeof(cpthread_t) * (size_t)threads);

  for (int game_idx = 0; game_idx < games; game_idx++) {
    const uint64_t game_seed_value = seed + (uint64_t)game_idx;
    game_reset(game);
    game_seed(game, game_seed_value);
    game_set_starting_player_index(game, game_idx % 2);
    draw_starting_racks(game);
    Game *previous = game_duplicate(game);
    Move previous_move = {0};
    for (int turn = 0; !game_over(game) && turn < DECIDE_MAX_TURNS; turn++) {
      const int bag_tiles = bag_get_letters(game_get_bag(game));
      if (bag_tiles == 0) {
        break;
      }
      const int seat = game_get_player_on_turn_index(game);
      const bool eligible =
          turn > 0 && move_get_type(&previous_move) != GAME_EVENT_PASS &&
          (move_get_type(&previous_move) != GAME_EVENT_EXCHANGE ||
           bag_get_letters(game_get_bag(previous)) <= 30);
      rack_copy(&opp_actual, player_get_rack(game_get_player(game, 1 - seat)));
      rack_copy(&observer, player_get_rack(game_get_player(game, seat)));
      rack_reset(&played);
      if (move_get_type(&previous_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        for (int tile_idx = 0; tile_idx < move_get_tiles_length(&previous_move);
             tile_idx++) {
          const MachineLetter ml = move_get_tile(&previous_move, tile_idx);
          if (ml != PLAYED_THROUGH_MARKER) {
            rack_add_letter(&played,
                            get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
          }
        }
      }
      const uint64_t sim_seed = (game_seed_value * UINT64_C(1000003)) +
                                ((uint64_t)turn * UINT64_C(7919));
      int candidates = 0;
      for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
        DecideArm *arm = &arms[arm_idx];
        arm->ran = arm_idx == 0 || eligible;
        arm->simulated = false;
        arm->iterations = 0;
        arm->seconds = 0;
        arm->distinct_index = -1;
        if (!arm->ran) {
          continue;
        }
        Timer timer;
        ctimer_start(&timer);
        if (arm->code == 't') {
          move_copy(&arm->move, get_top_equity_move(game, static_best));
          arm->seconds = ctimer_elapsed_seconds(&timer);
          decide_move_string(game, &arm->move, arm->move_str);
          continue;
        }
        const bool use_inference = arm->code == 's';
        SimArgs args;
        config_fill_sim_args(config,
                             arm->code == 'p' ? &opp_actual : &no_rack,
                             &played, &observer, &known, &args);
        args.move_list = moves;
        args.num_plays = num_plays;
        args.game = game;
        args.inference_results = inferences;
        args.seed = arm->code == 'c' ? sim_seed + UINT64_C(7777777) : sim_seed;
        args.bai_options.time_limit_seconds = move_seconds;
        args.use_inference = use_inference;
        if (use_inference) {
          infer_args_fill(&args.inference_args, 0, int_to_equity(10000), NULL,
                          previous, threads, 0, 0, control, false, true,
                          1 - seat, move_get_score(&previous_move),
                          move_get_type(&previous_move) == GAME_EVENT_EXCHANGE
                              ? move_get_tiles_played(&previous_move)
                              : 0,
                          &played, &known, &observer);
          args.inference_args.temperature = int_to_equity(8);
          args.inference_args.target_move = &previous_move;
        }
        thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
        const Move *move =
            get_top_simming_move(game, moves, &args, &arm->sim_ctx, results,
                                 &arm->simulated, errors);
        if (!error_stack_is_empty(errors) || move == NULL) {
          error_stack_print_and_reset(errors);
          log_fatal("infdecide failed: seed=%llu game=%d turn=%d arm=%c",
                    (unsigned long long)seed, game_idx, turn, arm->code);
        }
        move_copy(&arm->move, move);
        arm->seconds = ctimer_elapsed_seconds(&timer);
        if (arm->simulated) {
          arm->iterations = sim_results_get_iteration_count(results);
        }
        if (arm_idx == 0) {
          candidates = move_list_get_count(moves);
        }
        decide_move_string(game, &arm->move, arm->move_str);
      }

      // Distinct moves, the control's first.
      Move distinct[DECIDE_MAX_ARMS + 1];
      char distinct_str[DECIDE_MAX_ARMS + 1][DECIDE_MOVE_STR];
      int num_distinct = 0;
      for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
        DecideArm *arm = &arms[arm_idx];
        if (!arm->ran) {
          continue;
        }
        for (int idx = 0; idx < num_distinct; idx++) {
          if (strcmp(distinct_str[idx], arm->move_str) == 0) {
            arm->distinct_index = idx;
            break;
          }
        }
        if (arm->distinct_index < 0) {
          arm->distinct_index = num_distinct;
          move_copy(&distinct[num_distinct], &arm->move);
          snprintf(distinct_str[num_distinct], DECIDE_MOVE_STR, "%s",
                   arm->move_str);
          num_distinct++;
        }
      }

      char rack_str[32];
      char opp_str[32];
      decide_rack_string(game, &observer, rack_str, sizeof(rack_str));
      decide_rack_string(game, &opp_actual, opp_str, sizeof(opp_str));
      printf("INFDEC_POS seed=%llu game=%d turn=%d seat=%d bag=%d eligible=%d "
             "candidates=%d rack=%s opp_rack=%s distinct=%d",
             (unsigned long long)seed, game_idx, turn, seat, bag_tiles,
             eligible, candidates, rack_str, opp_str, num_distinct);
      for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
        const DecideArm *arm = &arms[arm_idx];
        if (arm->ran) {
          printf(" %c_move=%s %c_iterations=%llu %c_seconds=%.3f", arm->code,
                 arm->move_str, arm->code, (unsigned long long)arm->iterations,
                 arm->code, arm->seconds);
        }
      }
      printf("\n");
      fflush(stdout);

      double sim_win[DECIDE_MAX_ARMS + 1] = {0};
      double sim_utility[DECIDE_MAX_ARMS + 1] = {0};
      double sim_equity[DECIDE_MAX_ARMS + 1] = {0};
      uint64_t sim_rollouts[DECIDE_MAX_ARMS + 1] = {0};
      double oracle_seconds = 0;
      if (num_distinct > 1) {
        Timer oracle_timer;
        ctimer_start(&oracle_timer);
        // Playout oracle.
        const uint64_t playout_seed = (sim_seed * UINT64_C(31)) + 17;
        for (int worker = 0; worker < threads; worker++) {
          jobs[worker] = (PlayoutJob){
              .base = game,
              .moves = distinct,
              .num_moves = num_distinct,
              .mover = seat,
              .seed_base = playout_seed,
              .first = (int)(((int64_t)playouts * worker) / threads),
              .last = (int)(((int64_t)playouts * (worker + 1)) / threads),
              .win = playout_win,
              .spread = playout_spread,
              .playouts = playouts,
          };
          cpthread_create(&workers[worker], decide_playout_worker,
                          &jobs[worker]);
        }
        for (int worker = 0; worker < threads; worker++) {
          cpthread_join(workers[worker]);
        }
        // Sim oracle: equal samples of just these moves, opponent rack known.
        move_list_reset(oracle_moves);
        for (int idx = 0; idx < num_distinct; idx++) {
          move_list_add_move(oracle_moves, &distinct[idx]);
        }
        SimArgs args;
        config_fill_sim_args(config, &opp_actual, &played, &observer, &known,
                             &args);
        args.move_list = oracle_moves;
        args.num_plays = num_distinct;
        args.game = game;
        args.inference_results = inferences;
        args.use_inference = false;
        args.seed = sim_seed + UINT64_C(424243);
        args.bai_options.threshold = BAI_THRESHOLD_NONE;
        args.bai_options.sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN;
        args.bai_options.sample_limit =
            (uint64_t)DECIDE_ORACLE_SIM_ROLLOUTS_PER_MOVE *
            (uint64_t)num_distinct;
        args.bai_options.time_limit_seconds = 120;
        thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
        SimCtx *oracle_ctx = NULL;
        simulate(&args, &oracle_ctx, oracle_results, errors);
        if (!error_stack_is_empty(errors)) {
          error_stack_print_and_reset(errors);
          log_fatal("infdecide oracle sim failed: seed=%llu game=%d turn=%d",
                    (unsigned long long)seed, game_idx, turn);
        }
        const int oracle_plays = sim_results_get_number_of_plays(oracle_results);
        for (int play_idx = 0; play_idx < oracle_plays; play_idx++) {
          const SimmedPlay *simmed =
              sim_results_get_simmed_play(oracle_results, play_idx);
          char simmed_str[DECIDE_MOVE_STR];
          decide_move_string(game, simmed_play_get_move(simmed), simmed_str);
          for (int idx = 0; idx < num_distinct; idx++) {
            if (strcmp(distinct_str[idx], simmed_str) == 0) {
              sim_win[idx] =
                  stat_get_mean(simmed_play_get_win_pct_stat(simmed));
              sim_utility[idx] =
                  stat_get_mean(simmed_play_get_utility_stat(simmed));
              sim_equity[idx] =
                  stat_get_mean(simmed_play_get_equity_stat(simmed));
              sim_rollouts[idx] = simmed_play_get_num_rollouts(simmed);
            }
          }
        }
        sim_ctx_destroy(oracle_ctx);
        oracle_seconds = ctimer_elapsed_seconds(&oracle_timer);
      }

      for (int arm_idx = 1; arm_idx < num_arms; arm_idx++) {
        const DecideArm *arm = &arms[arm_idx];
        if (!arm->ran) {
          continue;
        }
        const int idx = arm->distinct_index;
        if (idx == 0) {
          printf("INFDEC_ARM seed=%llu game=%d turn=%d seat=%d bag=%d arm=%c "
                 "same=1\n",
                 (unsigned long long)seed, game_idx, turn, seat, bag_tiles,
                 arm->code);
          continue;
        }
        double win_sum = 0;
        double win_sq = 0;
        double spread_sum = 0;
        double spread_sq = 0;
        double ctl_win = 0;
        double ctl_spread = 0;
        for (int playout = 0; playout < playouts; playout++) {
          const size_t arm_slot =
              ((size_t)idx * (size_t)playouts) + (size_t)playout;
          const double dwin =
              (double)playout_win[arm_slot] - (double)playout_win[playout];
          const double dspread = (double)playout_spread[arm_slot] -
                                 (double)playout_spread[playout];
          win_sum += dwin;
          win_sq += dwin * dwin;
          spread_sum += dspread;
          spread_sq += dspread * dspread;
          ctl_win += playout_win[playout];
          ctl_spread += playout_spread[playout];
        }
        const double count = playouts;
        const double dwin_mean = win_sum / count;
        const double dspread_mean = spread_sum / count;
        const double dwin_se =
            sqrt(fmax(0, (win_sq / count) - (dwin_mean * dwin_mean)) /
                 (count - 1));
        const double dspread_se =
            sqrt(fmax(0, (spread_sq / count) - (dspread_mean * dspread_mean)) /
                 (count - 1));
        printf("INFDEC_ARM seed=%llu game=%d turn=%d seat=%d bag=%d arm=%c "
               "same=0 move=%s ctl_move=%s playouts=%d ctl_win=%.6f "
               "ctl_spread=%.4f dwin=%.6f dwin_se=%.6f dspread=%.4f "
               "dspread_se=%.4f sim_dwin=%.6f sim_dutility=%.6f "
               "sim_dequity=%.4f sim_rollouts=%llu sim_ctl_rollouts=%llu "
               "oracle_seconds=%.3f\n",
               (unsigned long long)seed, game_idx, turn, seat, bag_tiles,
               arm->code, arm->move_str, arms[0].move_str, playouts,
               ctl_win / count, ctl_spread / count, dwin_mean, dwin_se,
               dspread_mean, dspread_se, sim_win[idx] - sim_win[0],
               sim_utility[idx] - sim_utility[0],
               sim_equity[idx] - sim_equity[0],
               (unsigned long long)sim_rollouts[idx],
               (unsigned long long)sim_rollouts[0], oracle_seconds);
      }
      if (fflush(stdout) != 0) {
        log_fatal("infdecide output flush failed");
      }

      game_copy(previous, game);
      move_copy(&previous_move, &arms[0].move);
      play_move(&arms[0].move, game, NULL);
    }
    printf("INFDEC_GAME seed=%llu game=%d\n", (unsigned long long)seed,
           game_idx);
    fflush(stdout);
    game_destroy(previous);
  }

  for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
    sim_ctx_destroy(arms[arm_idx].sim_ctx);
  }
  free(workers);
  free(jobs);
  free(playout_spread);
  free(playout_win);
  inference_results_destroy(inferences);
  sim_results_destroy(oracle_results);
  sim_results_destroy(results);
  move_list_destroy(oracle_moves);
  move_list_destroy(static_best);
  move_list_destroy(moves);
  error_stack_destroy(errors);
  config_destroy(config);
}
