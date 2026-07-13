#include "play_chooser.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/bai_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bag.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_timer.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../ent/words.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "move_gen.h"
#include "simmer.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  PLAY_CHOOSER_DEFAULT_SIM_PLIES = 2,
  PLAY_CHOOSER_DEFAULT_SIM_MAX_CANDIDATES = 15,
  // Assume roughly four tiles per play across both players when splitting
  // the remaining clock into per-move budgets.
  PLAY_CHOOSER_TILES_PER_PLAY_PAIR = 8,
};

static const double PLAY_CHOOSER_DEFAULT_UNTIMED_MOVE_SECONDS = 5.0;
static const double PLAY_CHOOSER_DEFAULT_CHALLENGE_SECONDS = 5.0;
static const double PLAY_CHOOSER_MIN_MOVE_BUDGET_SECONDS = 0.05;
// Minimum budget for the null-window solve that resolves a challenge
// decision after the sibling branch produced an exact value; the warm
// shared transposition table makes these solves cheap.
static const double PLAY_CHOOSER_MIN_RESOLVE_SECONDS = 0.25;
// Fraction of system memory for the chooser's single shared transposition
// table. All of the chooser's endgame solves — the concurrent keep and
// challenge branch evaluations of a challenge decision and the subsequent
// move-choosing solve — share this one table, so search done while
// deciding whether to challenge seeds the search for the move itself.
// Two choosers (one per player) stay within the 50% total cap.
static const double PLAY_CHOOSER_ENDGAME_TT_FRACTION = 0.2;

struct PlayChooser {
  PlayChooserStrategy strategy;
  MoveList *move_list;
  SimResults *sim_results;
  SimCtx *sim_ctx;
  EndgameResults *endgame_results;
  EndgameCtx *endgame_ctx;
  // Contexts and results for the keep/challenge branch evaluations of a
  // challenge decision, which run concurrently on separate threads. Each
  // branch needs its own, but all solves share endgame_tt.
  EndgameResults *keep_endgame_results;
  EndgameCtx *keep_endgame_ctx;
  EndgameResults *challenge_endgame_results;
  EndgameCtx *challenge_endgame_ctx;
  // Lazily created on the first endgame solve; owned.
  TranspositionTable *endgame_tt;
};

static int
play_chooser_get_sim_max_candidates(const PlayChooserStrategy *strategy) {
  if (strategy->sim_max_candidates > 0) {
    return strategy->sim_max_candidates;
  }
  return PLAY_CHOOSER_DEFAULT_SIM_MAX_CANDIDATES;
}

static int play_chooser_get_num_threads(const PlayChooserStrategy *strategy) {
  if (strategy->num_threads > 0) {
    return strategy->num_threads;
  }
  return 1;
}

PlayChooser *play_chooser_create(const PlayChooserStrategy *strategy) {
  if (strategy->pre_endgame_eval == PLAY_CHOOSER_EVAL_ENDGAME) {
    log_fatal("play chooser pre-endgame evaluation cannot be the endgame "
              "solver");
  }
  if ((strategy->pre_endgame_eval == PLAY_CHOOSER_EVAL_SIM ||
       strategy->endgame_eval == PLAY_CHOOSER_EVAL_SIM) &&
      strategy->win_pcts == NULL) {
    log_fatal("play chooser sim evaluation requires win percentages");
  }
  PlayChooser *play_chooser = (PlayChooser *)malloc_or_die(sizeof(PlayChooser));
  play_chooser->strategy = *strategy;
  play_chooser->move_list =
      move_list_create(play_chooser_get_sim_max_candidates(strategy));
  play_chooser->sim_results = sim_results_create(0.0);
  play_chooser->sim_ctx = NULL;
  play_chooser->endgame_results = endgame_results_create();
  play_chooser->endgame_ctx = endgame_ctx_create();
  play_chooser->keep_endgame_results = endgame_results_create();
  play_chooser->keep_endgame_ctx = endgame_ctx_create();
  play_chooser->challenge_endgame_results = endgame_results_create();
  play_chooser->challenge_endgame_ctx = endgame_ctx_create();
  play_chooser->endgame_tt = NULL;
  return play_chooser;
}

void play_chooser_destroy(PlayChooser *play_chooser) {
  if (play_chooser == NULL) {
    return;
  }
  move_list_destroy(play_chooser->move_list);
  sim_results_destroy(play_chooser->sim_results);
  sim_ctx_destroy(play_chooser->sim_ctx);
  endgame_results_destroy(play_chooser->endgame_results);
  endgame_ctx_destroy(play_chooser->endgame_ctx);
  endgame_results_destroy(play_chooser->keep_endgame_results);
  endgame_ctx_destroy(play_chooser->keep_endgame_ctx);
  endgame_results_destroy(play_chooser->challenge_endgame_results);
  endgame_ctx_destroy(play_chooser->challenge_endgame_ctx);
  transposition_table_destroy(play_chooser->endgame_tt);
  free(play_chooser);
}

// The chooser's shared transposition table, created on first use. Shared
// across the branch evaluations and the move-choosing solves so search
// results carry over between them.
static TranspositionTable *
play_chooser_get_endgame_tt(PlayChooser *play_chooser) {
  if (play_chooser->endgame_tt == NULL) {
    play_chooser->endgame_tt =
        transposition_table_create(PLAY_CHOOSER_ENDGAME_TT_FRACTION);
  }
  return play_chooser->endgame_tt;
}

static bool play_chooser_game_is_in_endgame(const Game *game) {
  return bag_get_letters(game_get_bag(game)) == 0;
}

static play_chooser_eval_t
play_chooser_get_eval_for_phase(const PlayChooserStrategy *strategy,
                                const Game *game) {
  if (play_chooser_game_is_in_endgame(game)) {
    return strategy->endgame_eval;
  }
  return strategy->pre_endgame_eval;
}

// Budget for the on-turn player's current move: a flat budget when
// configured, otherwise the player's remaining clock split evenly across
// an estimate of their remaining plays.
static double
play_chooser_get_seconds_for_move(const PlayChooserStrategy *strategy,
                                  const Game *game) {
  if (strategy->fixed_seconds_per_move > 0.0) {
    return strategy->fixed_seconds_per_move;
  }
  const GameTimer *game_timer = strategy->game_timer;
  if (game_timer == NULL || game_timer_is_untimed(game_timer)) {
    return PLAY_CHOOSER_DEFAULT_UNTIMED_MOVE_SECONDS;
  }
  const int player_on_turn_index = game_get_player_on_turn_index(game);
  const int total_unplayed_tiles =
      bag_get_letters(game_get_bag(game)) +
      rack_get_total_letters(player_get_rack(game_get_player(game, 0))) +
      rack_get_total_letters(player_get_rack(game_get_player(game, 1)));
  int plays_remaining_for_player =
      (total_unplayed_tiles + PLAY_CHOOSER_TILES_PER_PLAY_PAIR - 1) /
      PLAY_CHOOSER_TILES_PER_PLAY_PAIR;
  if (plays_remaining_for_player < 1) {
    plays_remaining_for_player = 1;
  }
  const double seconds_remaining =
      game_timer_get_seconds_remaining(game_timer, player_on_turn_index);
  double budget_seconds =
      seconds_remaining / (double)plays_remaining_for_player;
  if (budget_seconds < PLAY_CHOOSER_MIN_MOVE_BUDGET_SECONDS) {
    budget_seconds = PLAY_CHOOSER_MIN_MOVE_BUDGET_SECONDS;
  }
  return budget_seconds;
}

static void play_chooser_choose_static_move(PlayChooser *play_chooser,
                                            Game *game, Move *out_move) {
  const Move *best_move = get_top_equity_move(game, play_chooser->move_list);
  move_copy(out_move, best_move);
}

// Generate the top static candidates and simulate them within the time
// budget. Returns false if no move could be produced.
static bool play_chooser_run_sim(PlayChooser *play_chooser, Game *game,
                                 double budget_seconds, Move *out_move,
                                 ErrorStack *error_stack) {
  const PlayChooserStrategy *strategy = &play_chooser->strategy;
  MoveList *move_list = play_chooser->move_list;
  move_list_reset(move_list);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .move_list = move_list,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&gen_args);
  const int num_candidates = move_list_get_count(move_list);
  if (num_candidates == 0) {
    return false;
  }
  if (num_candidates == 1) {
    move_copy(out_move, move_list_get_move(move_list, 0));
    return true;
  }

  const int num_threads = play_chooser_get_num_threads(strategy);
  const int sim_plies = strategy->sim_plies > 0
                            ? strategy->sim_plies
                            : PLAY_CHOOSER_DEFAULT_SIM_PLIES;
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);

  const int player_on_turn_index = game_get_player_on_turn_index(game);
  const Player *opponent = game_get_player(game, 1 - player_on_turn_index);

  SimArgs sim_args = {0};
  sim_args.num_plies = sim_plies;
  sim_args.game = game;
  sim_args.move_list = move_list;
  sim_args.num_plays = num_candidates;
  sim_args.known_opp_rack = player_get_known_rack_from_phonies(opponent);
  sim_args.win_pcts = strategy->win_pcts;
  sim_args.use_inference = false;
  sim_args.use_heat_map = false;
  sim_args.inference_results = NULL;
  sim_args.num_threads = num_threads;
  sim_args.print_interval = 0;
  sim_args.max_num_display_plays = num_candidates;
  sim_args.max_num_display_plies = sim_plies;
  sim_args.seed = strategy->seed;
  sim_args.thread_control = thread_control;
  sim_args.bai_options.sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS;
  sim_args.bai_options.threshold = BAI_THRESHOLD_NONE;
  sim_args.bai_options.delta = 1.0;
  sim_args.bai_options.sample_limit = (uint64_t)1e15;
  sim_args.bai_options.sample_minimum = 1;
  sim_args.bai_options.time_limit_seconds = budget_seconds;
  sim_args.bai_options.num_threads = num_threads;
  sim_args.bai_options.cutoff = 0.0;
  sim_args.bai_options.parent_worker_thread_index = 0;
  sim_args.bai_options.arm_avoid_prune = NULL;
  sim_args.bai_options.num_arm_avoid_prune = 0;
  sim_args.utility_w_winpct = 1.0;
  sim_args.utility_w_spread = 0.0;
  sim_args.utility_spread_scale = 100.0;

  // The persistent SimCtx recycles the simmer's allocations across calls
  // (samples themselves are reset per simulation by the engine).
  simulate(&sim_args, &play_chooser->sim_ctx, play_chooser->sim_results,
           error_stack);
  thread_control_destroy(thread_control);
  if (!error_stack_is_empty(error_stack)) {
    return false;
  }

  const Move *best_move = sim_results_get_best_move(play_chooser->sim_results);
  if (best_move == NULL) {
    // The sim could not pick a winner; fall back to the top equity move
    // from the candidates already generated.
    best_move = move_list_get_move(move_list, 0);
  }
  move_copy(out_move, best_move);
  return true;
}

// Requested endgame solve depth in plies.
static int play_chooser_get_endgame_plies(const PlayChooserStrategy *strategy) {
  if (strategy->endgame_plies > 0) {
    return strategy->endgame_plies;
  }
  return MAX_VARIANT_LENGTH;
}

// Solve the endgame within the time budget using the given context and
// the chooser's shared transposition table. external_thread_control may
// be NULL, in which case one is created for the solve; a caller-provided
// one allows another thread to interrupt the solve. When use_window is
// true, the solve searches the fixed [window_alpha, window_beta] window
// (in final-spread units) and the reported value is a bound relative to
// that window rather than an exact value. Returns false if no move could
// be produced.
static bool play_chooser_run_endgame(
    const PlayChooserStrategy *strategy, EndgameCtx **endgame_ctx,
    EndgameResults *endgame_results, TranspositionTable *shared_tt, Game *game,
    int num_threads, double budget_seconds,
    ThreadControl *external_thread_control, bool use_window,
    int32_t window_alpha, int32_t window_beta, Move *out_move,
    int32_t *out_value, ErrorStack *error_stack) {
  ThreadControl *thread_control = external_thread_control;
  if (thread_control == NULL) {
    thread_control = thread_control_create();
    thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  }
  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = thread_control;
  endgame_args.game = game;
  endgame_args.shared_tt = shared_tt;
  endgame_args.plies = play_chooser_get_endgame_plies(strategy);
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = num_threads;
  endgame_args.use_heuristics = true;
  endgame_args.num_top_moves = 1;
  endgame_args.soft_time_limit = budget_seconds * 0.9;
  endgame_args.hard_time_limit = budget_seconds;
  endgame_args.seed = strategy->seed;
  endgame_args.use_initial_window = use_window;
  endgame_args.initial_alpha = window_alpha;
  endgame_args.initial_beta = window_beta;

  endgame_solve(endgame_ctx, &endgame_args, endgame_results, error_stack);
  if (external_thread_control == NULL) {
    thread_control_destroy(thread_control);
  }
  if (!error_stack_is_empty(error_stack)) {
    return false;
  }
  const PVLine *pv_line =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
  if (pv_line->num_moves == 0) {
    return false;
  }
  if (out_move != NULL) {
    small_move_to_move(out_move, &pv_line->moves[0], game_get_board(game));
  }
  if (out_value != NULL) {
    *out_value =
        endgame_results_get_value(endgame_results, ENDGAME_RESULT_BEST);
  }
  return true;
}

void play_chooser_choose_move(PlayChooser *play_chooser, Game *game,
                              Move *out_move, ErrorStack *error_stack) {
  const PlayChooserStrategy *strategy = &play_chooser->strategy;
  const play_chooser_eval_t eval =
      play_chooser_get_eval_for_phase(strategy, game);
  const double budget_seconds =
      play_chooser_get_seconds_for_move(strategy, game);
  bool chose_move = false;
  switch (eval) {
  case PLAY_CHOOSER_EVAL_STATIC:
    break;
  case PLAY_CHOOSER_EVAL_SIM:
    chose_move = play_chooser_run_sim(play_chooser, game, budget_seconds,
                                      out_move, error_stack);
    break;
  case PLAY_CHOOSER_EVAL_ENDGAME:
    chose_move = play_chooser_run_endgame(
        strategy, &play_chooser->endgame_ctx, play_chooser->endgame_results,
        play_chooser_get_endgame_tt(play_chooser), game,
        play_chooser_get_num_threads(strategy), budget_seconds,
        /*external_thread_control=*/NULL, /*use_window=*/false, 0, 0, out_move,
        NULL, error_stack);
    break;
  }
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  if (!chose_move) {
    play_chooser_choose_static_move(play_chooser, game, out_move);
  }
}

// Adds the letters a tile placement move takes from the player's rack
// (blanked tiles count as the blank).
static void play_chooser_add_played_letters_to_rack(const Move *move,
                                                    Rack *rack) {
  const int tiles_length = move_get_tiles_length(move);
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (get_is_blanked(ml)) {
      ml = BLANK_MACHINE_LETTER;
    }
    rack_add_letter(rack, ml);
  }
}

// Spread for the player on turn, in points.
static double play_chooser_get_spread(const Game *game) {
  const int player_on_turn_index = game_get_player_on_turn_index(game);
  const Equity player_score =
      player_get_score(game_get_player(game, player_on_turn_index));
  const Equity opponent_score =
      player_get_score(game_get_player(game, 1 - player_on_turn_index));
  return equity_to_double(player_score) - equity_to_double(opponent_score);
}

// Final spread for the player on turn of a game that has already ended,
// applying the standard end-rack adjustments to whichever side still
// holds tiles.
static double play_chooser_get_final_spread(const Game *game) {
  const LetterDistribution *ld = game_get_ld(game);
  const int player_on_turn_index = game_get_player_on_turn_index(game);
  const Rack *player_rack =
      player_get_rack(game_get_player(game, player_on_turn_index));
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, 1 - player_on_turn_index));
  double spread = play_chooser_get_spread(game);
  if (rack_is_empty(player_rack)) {
    spread += equity_to_double(calculate_end_rack_points(opponent_rack, ld));
  } else if (rack_is_empty(opponent_rack)) {
    spread -= equity_to_double(calculate_end_rack_points(player_rack, ld));
  } else {
    spread += equity_to_double(calculate_end_rack_penalty(player_rack, ld)) -
              equity_to_double(calculate_end_rack_penalty(opponent_rack, ld));
  }
  return spread;
}

// Value of the position from the perspective of the player on turn:
// final spread points for STATIC, win fraction for SIM. Values from the
// same eval mode are comparable across positions. ENDGAME branch
// evaluations are handled by play_chooser_decide_challenge_endgame.
static double play_chooser_evaluate_position(PlayChooser *play_chooser,
                                             Game *game,
                                             play_chooser_eval_t eval,
                                             double budget_seconds,
                                             ErrorStack *error_stack) {
  if (game_over(game)) {
    return play_chooser_get_final_spread(game);
  }
  double value = 0.0;
  switch (eval) {
  case PLAY_CHOOSER_EVAL_STATIC: {
    const Move *best_move = get_top_equity_move(game, play_chooser->move_list);
    value = play_chooser_get_spread(game) +
            equity_to_double(move_get_equity(best_move));
    break;
  }
  case PLAY_CHOOSER_EVAL_SIM: {
    Move best_move;
    if (!play_chooser_run_sim(play_chooser, game, budget_seconds, &best_move,
                              error_stack)) {
      break;
    }
    const int best_index =
        sim_results_get_best_move_index(play_chooser->sim_results);
    const SimmedPlay *best_play =
        sim_results_get_simmed_play(play_chooser->sim_results, best_index);
    value = stat_get_mean(simmed_play_get_win_pct_stat(best_play));
    break;
  }
  case PLAY_CHOOSER_EVAL_ENDGAME:
    log_fatal("endgame branch evaluations must go through the endgame "
              "challenge decider");
    break;
  }
  return value;
}

// One side of an endgame challenge decision, solved on its own thread.
typedef struct PlayChooserEndgameBranch {
  PlayChooser *play_chooser;
  Game *game;
  double budget_seconds;
  EndgameCtx **endgame_ctx;
  EndgameResults *endgame_results;
  TranspositionTable *endgame_tt;
  int num_threads;
  ThreadControl *thread_control;
  // Interrupted when this branch solves exactly, so the decision can
  // switch the sibling to a cheaper does-it-beat-this search.
  ThreadControl *sibling_thread_control;
  ErrorStack *error_stack;
  double value;
  bool exact;
} PlayChooserEndgameBranch;

static void
play_chooser_solve_endgame_branch(PlayChooserEndgameBranch *branch) {
  branch->exact = false;
  int32_t endgame_value = 0;
  if (play_chooser_run_endgame(
          &branch->play_chooser->strategy, branch->endgame_ctx,
          branch->endgame_results, branch->endgame_tt, branch->game,
          branch->num_threads, branch->budget_seconds, branch->thread_control,
          /*use_window=*/false, 0, 0, NULL, &endgame_value,
          branch->error_stack)) {
    branch->value =
        play_chooser_get_spread(branch->game) + (double)endgame_value;
    // The value is exact once the solve has completed every requested
    // ply; a solve stopped by its time limit or an interrupt reports a
    // shallower completed depth.
    branch->exact =
        endgame_results_get_depth(branch->endgame_results,
                                  ENDGAME_RESULT_BEST) >=
        play_chooser_get_endgame_plies(&branch->play_chooser->strategy);
  }
  if (branch->exact) {
    thread_control_set_status(branch->sibling_thread_control,
                              THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
}

static void *play_chooser_endgame_branch_thread(void *arg) {
  play_chooser_solve_endgame_branch((PlayChooserEndgameBranch *)arg);
  return NULL;
}

// Decide an endgame challenge by solving the keep and challenge branches
// concurrently against the shared transposition table. A branch whose
// game is already over (for example a kept phony that goes out) has an
// exact value immediately. When one branch reaches an exact value while
// the other is unresolved, the unresolved branch is interrupted and
// re-solved with a null window around the exact value: alpha-beta then
// only proves which side of the verdict the branch falls on, which is
// much cheaper than computing its exact value — and the re-solve starts
// from the transposition table the interrupted search already filled.
static void play_chooser_decide_challenge_endgame(PlayChooser *play_chooser,
                                                  Game *keep_game,
                                                  Game *challenge_game,
                                                  double decision_seconds,
                                                  ChallengeDecision *decision,
                                                  ErrorStack *error_stack) {
  Timer decision_timer;
  ctimer_start(&decision_timer);
  TranspositionTable *endgame_tt = play_chooser_get_endgame_tt(play_chooser);
  const PlayChooserStrategy *strategy = &play_chooser->strategy;
  const int total_threads = play_chooser_get_num_threads(strategy);

  bool keep_exact = game_over(keep_game);
  bool challenge_exact = game_over(challenge_game);
  double keep_value =
      keep_exact ? play_chooser_get_final_spread(keep_game) : 0.0;
  double challenge_value =
      challenge_exact ? play_chooser_get_final_spread(challenge_game) : 0.0;

  if (!keep_exact && !challenge_exact) {
    // Solve both branches concurrently: each gets the whole decision
    // window and neither benefits from going first or second.
    const int keep_threads = (total_threads + 1) / 2;
    int challenge_threads = total_threads - keep_threads;
    if (challenge_threads < 1) {
      challenge_threads = 1;
    }
    ThreadControl *keep_thread_control = thread_control_create();
    thread_control_set_status(keep_thread_control,
                              THREAD_CONTROL_STATUS_STARTED);
    ThreadControl *challenge_thread_control = thread_control_create();
    thread_control_set_status(challenge_thread_control,
                              THREAD_CONTROL_STATUS_STARTED);
    ErrorStack *keep_error_stack = error_stack_create();
    PlayChooserEndgameBranch keep_branch = {
        .play_chooser = play_chooser,
        .game = keep_game,
        .budget_seconds = decision_seconds,
        .endgame_ctx = &play_chooser->keep_endgame_ctx,
        .endgame_results = play_chooser->keep_endgame_results,
        .endgame_tt = endgame_tt,
        .num_threads = keep_threads,
        .thread_control = keep_thread_control,
        .sibling_thread_control = challenge_thread_control,
        .error_stack = keep_error_stack,
        .value = 0.0,
        .exact = false,
    };
    PlayChooserEndgameBranch challenge_branch = {
        .play_chooser = play_chooser,
        .game = challenge_game,
        .budget_seconds = decision_seconds,
        .endgame_ctx = &play_chooser->challenge_endgame_ctx,
        .endgame_results = play_chooser->challenge_endgame_results,
        .endgame_tt = endgame_tt,
        .num_threads = challenge_threads,
        .thread_control = challenge_thread_control,
        .sibling_thread_control = keep_thread_control,
        .error_stack = error_stack,
        .value = 0.0,
        .exact = false,
    };
    cpthread_t keep_thread;
    cpthread_create(&keep_thread, play_chooser_endgame_branch_thread,
                    &keep_branch);
    play_chooser_solve_endgame_branch(&challenge_branch);
    cpthread_join(keep_thread);
    thread_control_destroy(keep_thread_control);
    thread_control_destroy(challenge_thread_control);
    if (!error_stack_is_empty(keep_error_stack)) {
      error_stack_push(
          error_stack, error_stack_top(keep_error_stack),
          string_duplicate("keep branch challenge evaluation failed"));
    }
    error_stack_destroy(keep_error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    keep_exact = keep_branch.exact;
    keep_value = keep_branch.value;
    challenge_exact = challenge_branch.exact;
    challenge_value = challenge_branch.value;
  }

  if (keep_exact == challenge_exact) {
    // Either both values are exact (the verdict is exact) or both solves
    // ran out of time (compare the best estimates).
    decision->keep_value = keep_value;
    decision->challenge_value = challenge_value;
    decision->should_challenge = challenge_value > keep_value;
    return;
  }

  // Exactly one branch is exact. The other only has to prove which side
  // of the verdict it falls on: a null-window solve around the exact
  // value, on the already-warm shared transposition table. The chooser
  // challenges only when the challenge branch is strictly better, so the
  // window goes just above the exact value when resolving the challenge
  // branch and just below it when resolving the keep branch.
  double remaining_seconds =
      decision_seconds - ctimer_elapsed_seconds(&decision_timer);
  if (remaining_seconds < PLAY_CHOOSER_MIN_RESOLVE_SECONDS) {
    remaining_seconds = PLAY_CHOOSER_MIN_RESOLVE_SECONDS;
  }
  const bool resolving_challenge_branch = !challenge_exact;
  Game *resolve_game = resolving_challenge_branch ? challenge_game : keep_game;
  EndgameCtx **resolve_ctx = resolving_challenge_branch
                                 ? &play_chooser->challenge_endgame_ctx
                                 : &play_chooser->keep_endgame_ctx;
  EndgameResults *resolve_results =
      resolving_challenge_branch ? play_chooser->challenge_endgame_results
                                 : play_chooser->keep_endgame_results;
  const int32_t exact_value = (int32_t)llround(
      resolving_challenge_branch ? keep_value : challenge_value);
  const int32_t window_alpha =
      resolving_challenge_branch ? exact_value : exact_value - 1;
  const int32_t window_beta = window_alpha + 1;
  int32_t resolve_endgame_value = 0;
  const bool resolved = play_chooser_run_endgame(
      strategy, resolve_ctx, resolve_results, endgame_tt, resolve_game,
      total_threads, remaining_seconds, /*external_thread_control=*/NULL,
      /*use_window=*/true, window_alpha, window_beta, NULL,
      &resolve_endgame_value, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  const double resolve_value = resolved
                                   ? play_chooser_get_spread(resolve_game) +
                                         (double)resolve_endgame_value
                                   : (double)window_alpha;
  const bool resolve_branch_beats_window =
      resolved && (int32_t)llround(resolve_value) >= window_beta;
  if (resolving_challenge_branch) {
    challenge_value = resolve_value;
    decision->should_challenge = resolve_branch_beats_window;
  } else {
    keep_value = resolve_value;
    decision->should_challenge = !resolve_branch_beats_window;
  }
  decision->keep_value = keep_value;
  decision->challenge_value = challenge_value;
}

void play_chooser_decide_challenge(PlayChooser *play_chooser,
                                   const Game *game_before_move,
                                   const Move *opp_move,
                                   ChallengeDecision *decision,
                                   ErrorStack *error_stack) {
  decision->move_is_phony = false;
  decision->should_challenge = false;
  decision->keep_value = 0.0;
  decision->challenge_value = 0.0;
  const PlayChooserStrategy *strategy = &play_chooser->strategy;
  if (!strategy->enable_challenges ||
      move_get_type(opp_move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const int opponent_index = game_get_player_on_turn_index(game_before_move);
  const int chooser_index = 1 - opponent_index;

  // Word validity is judged against the chooser's own lexicon; the
  // chooser never challenges a play it believes is valid.
  Game *keep_game = game_duplicate(game_before_move);
  FormedWords *formed_words =
      formed_words_create(game_get_board(keep_game), opp_move);
  formed_words_populate_validities(
      player_get_kwg(game_get_player(keep_game, chooser_index)), formed_words,
      game_get_variant(keep_game) == GAME_VARIANT_WORDSMOG);
  const int num_words = formed_words_get_num_words(formed_words);
  bool any_word_invalid = false;
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    if (!formed_words_get_word_valid(formed_words, word_idx)) {
      any_word_invalid = true;
      break;
    }
  }
  formed_words_destroy(formed_words);
  if (!any_word_invalid) {
    game_destroy(keep_game);
    return;
  }
  decision->move_is_phony = true;

  Rack played_letters;
  rack_set_dist_size_and_reset(&played_letters,
                               ld_get_size(game_get_ld(game_before_move)));
  play_chooser_add_played_letters_to_rack(opp_move, &played_letters);

  // Use the same evaluation mode for both branches so their values are
  // comparable, chosen from the pre-move phase of the game.
  const play_chooser_eval_t eval =
      play_chooser_get_eval_for_phase(strategy, game_before_move);

  // Keep branch: the phony stands. The opponent's played letters are no
  // longer on their rack, and they replenish from the bag.
  play_move_without_drawing_tiles(opp_move, keep_game);
  Rack *keep_known_rack = player_get_known_rack_from_phonies(
      game_get_player(keep_game, opponent_index));
  rack_subtract_using_floor_zero(keep_known_rack, &played_letters);
  if (!game_over(keep_game) && bag_get_letters(game_get_bag(keep_game)) > 0) {
    draw_to_full_rack(keep_game, opponent_index);
  }

  // Challenge branch: the phony comes off, the opponent loses the turn,
  // and the returned letters become known to the chooser (the full rack
  // becomes known when the bag is empty).
  Game *challenge_game = game_duplicate(game_before_move);
  game_set_player_on_turn_index(challenge_game, chooser_index);
  game_increment_consecutive_scoreless_turns(challenge_game);
  Rack *challenge_known_rack = player_get_known_rack_from_phonies(
      game_get_player(challenge_game, opponent_index));
  if (bag_get_letters(game_get_bag(challenge_game)) == 0) {
    rack_copy(challenge_known_rack,
              player_get_rack(game_get_player(challenge_game, opponent_index)));
  } else {
    rack_union(challenge_known_rack, &played_letters);
  }

  double decision_seconds = strategy->challenge_decision_seconds;
  if (decision_seconds <= 0.0) {
    decision_seconds = PLAY_CHOOSER_DEFAULT_CHALLENGE_SECONDS;
  }

  if (eval == PLAY_CHOOSER_EVAL_ENDGAME) {
    play_chooser_decide_challenge_endgame(play_chooser, keep_game,
                                          challenge_game, decision_seconds,
                                          decision, error_stack);
  } else {
    const double per_branch_seconds = decision_seconds / 2.0;
    decision->keep_value = play_chooser_evaluate_position(
        play_chooser, keep_game, eval, per_branch_seconds, error_stack);
    if (error_stack_is_empty(error_stack)) {
      decision->challenge_value = play_chooser_evaluate_position(
          play_chooser, challenge_game, eval, per_branch_seconds, error_stack);
      decision->should_challenge =
          decision->challenge_value > decision->keep_value;
    }
  }
  game_destroy(keep_game);
  game_destroy(challenge_game);
}

void play_chooser_challenge_off(Game *game, const Move *challenged_move) {
  // After the challenged move was played, the turn passed to the
  // challenger; the offender is the player not on turn.
  const int offender_index = 1 - game_get_player_on_turn_index(game);
  return_phony_letters(game);
  Rack *known_rack =
      player_get_known_rack_from_phonies(game_get_player(game, offender_index));
  if (bag_get_letters(game_get_bag(game)) == 0) {
    rack_copy(known_rack,
              player_get_rack(game_get_player(game, offender_index)));
  } else {
    Rack played_letters;
    rack_set_dist_size_and_reset(&played_letters,
                                 ld_get_size(game_get_ld(game)));
    play_chooser_add_played_letters_to_rack(challenged_move, &played_letters);
    rack_union(known_rack, &played_letters);
  }
}
