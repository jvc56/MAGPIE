#include "peg_judge.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/ent/win_pct.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "peg1_net.h"
#include "peg2_net.h"
#include "peg_net_util.h"
#include "peg_world.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

PegExactSolver peg_exact_create(int plies, double tt_fraction) {
  const char *cap = getenv("PEG_EXACT_CAP_S");
  PegExactSolver exact = {.ctx = NULL,
                          .results = endgame_results_create(),
                          .tt = transposition_table_create(tt_fraction),
                          .plies = plies,
                          .cap_seconds = cap != NULL ? strtod(cap, NULL) : 0};
  return exact;
}

void peg_exact_destroy(PegExactSolver *exact) {
  endgame_ctx_destroy(exact->ctx);
  endgame_results_destroy(exact->results);
  transposition_table_destroy(exact->tt);
}

int peg_exact_value(PegExactSolver *exact, const Game *game, bool *proven) {
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  EndgameArgs args;
  endgame_args_fill(
      control, game, /*tt_fraction_of_mem=*/0.0, exact->plies,
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, /*num_threads=*/1,
      /*use_heuristics=*/true, /*num_top_moves=*/1,
      /*per_ply_callback=*/NULL, /*per_ply_callback_data=*/NULL,
      /*before_search_callback=*/NULL, /*before_search_callback_data=*/NULL,
      /*per_root_move_callback=*/NULL, /*per_root_move_callback_data=*/NULL,
      DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/false,
      /*incremental_movegen=*/true, best_reply_shortcuts_disabled(),
      /*enable_pv_display=*/false,
      /*soft_time_limit=*/0.0, /*hard_time_limit=*/0.0, /*seed=*/1,
      /*skip_word_pruning=*/false, exact->tt, /*max_workers=*/0,
      /*first_win=*/false, /*first_win_fallback_moves=*/0,
      /*use_initial_window=*/false, /*initial_alpha=*/0, /*initial_beta=*/0,
      /*external_deadline_ns=*/0, /*actual_move=*/NULL, &args);
  const int64_t start = ctimer_monotonic_ns();
  if (exact->cap_seconds > 0) {
    args.external_deadline_ns = start + (int64_t)(exact->cap_seconds * 1e9);
  }
  ErrorStack *errors = error_stack_create();
  endgame_solve(&exact->ctx, &args, exact->results, errors);
  const double elapsed = (double)(ctimer_monotonic_ns() - start) / 1e9;
  if (!error_stack_is_empty(errors)) {
    error_stack_print_and_reset(errors);
    log_fatal("peg1exact endgame_solve failed");
  }
  error_stack_destroy(errors);
  thread_control_destroy(control);
  int depth = 0, root_done = 0, root_total = 0, ply2_done = 0, ply2_total = 0;
  endgame_ctx_get_progress(exact->ctx, &depth, &root_done, &root_total,
                           &ply2_done, &ply2_total);
  exact->capped =
      exact->cap_seconds > 0 && elapsed >= 0.98 * exact->cap_seconds;
  exact->depth = depth;
  *proven = depth < exact->plies && !exact->capped;
  return endgame_results_get_value(exact->results, ENDGAME_RESULT_BEST);
}

double peg_exact_final_spread(PegExactSolver *exact, const Game *root,
                              const Move *move, int seat, Game *game,
                              bool *proven, double *solve_ms, double *peg_ms,
                              int *peg_turns) {
  game_copy(game, root);
  play_move(move, game, NULL);
  *peg_turns = 0;
  *peg_ms = 0;
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 0 &&
         *peg_turns < PEG_NET_MAX_TURNS) {
    Move next;
    double ms;
    default_peg_pick(game, 1, NULL, NULL, &next, &ms);
    *peg_ms += ms;
    play_move(&next, game, NULL);
    (*peg_turns)++;
  }
  *proven = true;
  *solve_ms = 0;
  double spread = spread_for_player(game, seat);
  if (!game_over(game)) {
    if (bag_get_letters(game_get_bag(game)) != 0) {
      log_fatal("peg1exact bag not empty before the exact solve");
    }
    const int on_turn = game_get_player_on_turn_index(game);
    const int64_t start = ctimer_monotonic_ns();
    const int value = peg_exact_value(exact, game, proven);
    *solve_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    spread += on_turn == seat ? value : -value;
  }
  return spread;
}

int peg2_play_drawing_turns(Game *game, Peg1NetChooser *net,
                            Peg2ReplyPolicy reply, MoveList *static_list,
                            double *reply_ms) {
  int turns = 0;
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 0 &&
         turns < PEG_NET_MAX_TURNS) {
    Move next;
    double ms = 0;
    const int64_t start = ctimer_monotonic_ns();
    if (reply == PEG2_REPLY_NET && peg1_net_chooser_pick(net, 0, game, &next)) {
      ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    } else if (reply == PEG2_REPLY_PEG) {
      default_peg_pick(game, 1, NULL, NULL, &next, &ms);
    } else {
      move_copy(&next, top_equity_move(game, static_list));
    }
    *reply_ms += ms;
    play_move(&next, game, NULL);
    turns++;
  }
  return turns;
}

double peg2_fixed_depth_label(Game *game, int mover, int plies,
                              EndgameCtx **ctx, EndgameResults *results,
                              ThreadControl *control) {
  const double margin = spread_for_player(game, mover);
  if (game_over(game)) {
    return margin;
  }
  if (bag_get_letters(game_get_bag(game)) != 0) {
    log_fatal("peg2 label before the bag is empty");
  }
  const EndgameArgs args = {.game = game,
                            .thread_control = control,
                            .plies = plies,
                            .tt_fraction_of_mem = 0,
                            .initial_small_move_arena_size =
                                DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                            .num_threads = 1,
                            .use_heuristics = true,
                            .num_top_moves = 1,
                            .incremental_movegen = true,
                            .enable_pv_display = true,
                            .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT};
  endgame_solve_inline(ctx, &args, results);
  if (endgame_results_get_status(results) != ENDGAME_RESULT_STATUS_FINISHED) {
    log_fatal("peg2 incomplete endgame label");
  }
  const double value = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
  return game_get_player_on_turn_index(game) == mover ? margin + value
                                                      : margin - value;
}

int peg3_play_drawing_turns(Game *game, Peg1NetChooser *net,
                            Peg2NetChooser *chooser, int slot_idx,
                            Peg2ReplyPolicy reply, MoveList *static_list,
                            double *reply_ms, int *static_turns) {
  int turns = 0;
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 0 &&
         turns < PEG_NET_MAX_TURNS) {
    Move next;
    double ms = 0;
    const int64_t start = ctimer_monotonic_ns();
    if (reply == PEG2_REPLY_PEG) {
      default_peg_pick(game, 1, NULL, NULL, &next, &ms);
    } else if (peg1_net_chooser_pick(net, slot_idx, game, &next) ||
               peg2_net_chooser_pick(chooser, slot_idx, game, &next)) {
      ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    } else {
      move_copy(&next, top_equity_move(game, static_list));
      (*static_turns)++;
    }
    *reply_ms += ms;
    play_move(&next, game, NULL);
    turns++;
  }
  return turns;
}

// ---- 3PEG oracle (tier A) ---------------------------------------------------
// Scores fixed 3PEG root picks over every hidden world consistent with the
// mover's information: each ordered three-tile bag drawn from the ten unseen
// tiles (the opponent holds the other seven), weighted by its number of
// labelled-tile assignments, so worlds are uniform over the deals. In each
// world the pick is played, the drawing turns are played by the nets
// (peg3_play_drawing_turns, 1PEG net at one tile, 2PEG chooser at two, static
// otherwise) and the endgame is solved exactly. A pick that plays three or
// more tiles draws the whole bag, so its worlds are the bag multisets.
// PEG_JUDGE_BAG (1, 2 or 3; default 3) scores roots with that many tiles in
// the bag the same way: ordered bags of that size from the RACK_SIZE + bag
// unseen tiles, multisets when the pick draws the whole bag. With
// PEG_JUDGE_SAMPLES = K > 0 the worlds are instead K uniformly random deals
// of the unseen tiles (bag order and the opponent's rack; the same K for every
// pick of a root, from PEG_JUDGE_SEED + root), weight 1 each, plus the root's
// actual deal at weight 0 (reported, not averaged; weight 1 with
// PEG_JUDGE_ACTUAL_WEIGHT=1); bags up to RACK_SIZE. With
// PEG_JUDGE_SIM=CANDS,ITERS,PLIES the drawing turns are played by a static sim
// instead of the nets (peg_judge_sim_drawing_turns).
enum {
  PEG_JUDGE_MAX_WORLDS = 720,
  PEG_JUDGE_MAX_PICKS = 16,
  PEG_JUDGE_MAX_THREADS = 64,
  PEG_JUDGE_MAX_BAG = RACK_SIZE,
  PEG_JUDGE_MAX_ENUM_BAG = 3,
};

typedef struct PegJudgeWorld {
  MachineLetter bag[PEG_JUDGE_MAX_BAG];
  int64_t weight;
  bool actual;
  double final;
  bool proven;
  bool capped;
  int static_turns;
} PegJudgeWorld;

// The static sim that plays the drawing turns with PEG_JUDGE_SIM.
typedef struct PegJudgeSim {
  int candidates;
  int iterations;
  int plies;
  WinPct *win_pcts;
  uint64_t seed;
} PegJudgeSim;

typedef struct PegJudgeWorker {
  const Game *root;
  int root_id;
  const Move *pick;
  int bag_size;
  PegJudgeWorld *worlds;
  int n_worlds;
  atomic_int *next;
  Peg1NetChooser *net;
  Peg2NetChooser *chooser;
  int slot_idx;
  Game *game;
  MoveList *static_list;
  PegExactSolver exact;
  // NULL unless PEG_JUDGE_SIM; then the sim's candidates and results.
  const PegJudgeSim *sim;
  MoveList *sim_wide;
  Move *sim_wide_moves;
  MoveList *sim_pool;
  SimResults *sim_results;
} PegJudgeWorker;

// Distinct orderings of a multiset of up to PEG_JUDGE_MAX_BAG tiles.
static int peg_judge_bag_orders(const MachineLetter *tiles, int bag_size) {
  int orders = 1;
  for (int idx = 2; idx <= bag_size; idx++) {
    orders *= idx;
  }
  for (int idx = 0; idx < bag_size; idx++) {
    bool first_copy = true;
    int copies = 0;
    for (int other = 0; other < bag_size; other++) {
      first_copy &= other >= idx || tiles[other] != tiles[idx];
      copies += tiles[other] == tiles[idx];
    }
    for (int factor = 2; first_copy && factor <= copies; factor++) {
      orders /= factor;
    }
  }
  return orders;
}

// Appends the bags that extend tiles[0..depth) to worlds: ordered draws, or
// non-decreasing multisets (each weighted by its orderings) when whole_bag.
// weight is the product of the remaining counts drawn so far.
static void peg_judge_extend_bags(int *count, MachineLetter *tiles, int depth,
                                  int bag_size, bool whole_bag, int64_t weight,
                                  const MachineLetter *actual,
                                  PegJudgeWorld *worlds, int *n_worlds) {
  if (depth == bag_size) {
    PegJudgeWorld *world = &worlds[(*n_worlds)++];
    memset(world, 0, sizeof(*world));
    world->actual = true;
    for (int idx = 0; idx < bag_size; idx++) {
      world->bag[idx] = tiles[idx];
      world->actual &= tiles[idx] == actual[idx];
    }
    world->weight =
        whole_bag ? weight * peg_judge_bag_orders(tiles, bag_size) : weight;
    return;
  }
  const int lowest = whole_bag && depth > 0 ? tiles[depth - 1] : 0;
  for (int ml = lowest; ml < VALUE_NET_TILE_TYPES; ml++) {
    if (count[ml] == 0) {
      continue;
    }
    tiles[depth] = (MachineLetter)ml;
    const int64_t extended = weight * count[ml]--;
    peg_judge_extend_bags(count, tiles, depth + 1, bag_size, whole_bag,
                          extended, actual, worlds, n_worlds);
    count[ml]++;
  }
}

// The worlds for a pick from the root's unseen counts; the root's own bag
// order is flagged as the actual world. Returns the number of worlds.
static int peg_judge_all_worlds(const Game *root, const int *unseen,
                                int bag_size, bool whole_bag,
                                PegJudgeWorld *worlds) {
  MachineLetter actual[RACK_SIZE];
  if (bag_peek_tiles(game_get_bag(root), actual) != bag_size) {
    log_fatal("peg3oracle root bag does not hold %d tiles", bag_size);
  }
  if (whole_bag) {
    for (int lo = 0; lo < bag_size - 1; lo++) {
      for (int hi = lo + 1; hi < bag_size; hi++) {
        if (actual[hi] < actual[lo]) {
          const MachineLetter tmp = actual[lo];
          actual[lo] = actual[hi];
          actual[hi] = tmp;
        }
      }
    }
  }
  int count[VALUE_NET_TILE_TYPES];
  memcpy(count, unseen, sizeof(count));
  MachineLetter tiles[PEG_JUDGE_MAX_BAG];
  int n_worlds = 0;
  peg_judge_extend_bags(count, tiles, 0, bag_size, whole_bag, 1, actual, worlds,
                        &n_worlds);
  return n_worlds;
}

// The sampled worlds: the root's actual deal (weight actual_weight, 0 or 1)
// and samples uniformly random deals of the unseen tiles (weight 1), the same
// for every pick of the root. A loaded CGP's bag is in an order drawn from the
// config's time-based seed, so the actual deal keeps the root's opponent rack
// and takes its bag order from a shuffle seeded by splitmix64(seed), apart
// from the samples' stream. Returns the number of worlds.
static int peg_judge_sampled_worlds(const Game *root, const int *unseen,
                                    int bag_size, int samples, uint64_t seed,
                                    int actual_weight, PegJudgeWorld *worlds) {
  MachineLetter tiles[2 * RACK_SIZE];
  int n_unseen = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    for (int copy = 0; copy < unseen[ml]; copy++) {
      tiles[n_unseen++] = (MachineLetter)ml;
    }
  }
  memset(&worlds[0], 0, sizeof(worlds[0]));
  if (bag_peek_tiles(game_get_bag(root), worlds[0].bag) != bag_size) {
    log_fatal("peg3oracle root bag does not hold %d tiles", bag_size);
  }
  worlds[0].actual = true;
  worlds[0].weight = actual_weight;
  MachineLetter *actual = worlds[0].bag;
  for (int idx = 1; idx < bag_size; idx++) {
    const MachineLetter moved = actual[idx];
    int slot = idx;
    while (slot > 0 && actual[slot - 1] > moved) {
      actual[slot] = actual[slot - 1];
      slot--;
    }
    actual[slot] = moved;
  }
  XoshiroPRNG *actual_prng = prng_create(splitmix64(seed));
  for (int idx = bag_size - 1; idx > 0; idx--) {
    const int other =
        (int)prng_get_random_number(actual_prng, (uint64_t)idx + 1);
    const MachineLetter tmp = actual[idx];
    actual[idx] = actual[other];
    actual[other] = tmp;
  }
  prng_destroy(actual_prng);
  XoshiroPRNG *prng = prng_create(seed);
  for (int world_idx = 1; world_idx <= samples; world_idx++) {
    for (int idx = n_unseen - 1; idx > 0; idx--) {
      const int other = (int)prng_get_random_number(prng, (uint64_t)idx + 1);
      const MachineLetter tmp = tiles[idx];
      tiles[idx] = tiles[other];
      tiles[other] = tmp;
    }
    PegJudgeWorld *world = &worlds[world_idx];
    memset(world, 0, sizeof(*world));
    memcpy(world->bag, tiles, sizeof(MachineLetter) * (size_t)bag_size);
    world->weight = 1;
  }
  prng_destroy(prng);
  return samples + 1;
}

// root with the opponent's rack and the bag replaced by one world.
static void peg_judge_set_world(const Game *root, const int *unseen,
                                const MachineLetter *bag_tiles, int bag_size,
                                Game *world) {
  const int mover = game_get_player_on_turn_index(root);
  game_copy(world, root);
  int rest[VALUE_NET_TILE_TYPES];
  memcpy(rest, unseen, sizeof(rest));
  for (int idx = 0; idx < bag_size; idx++) {
    rest[bag_tiles[idx]]--;
  }
  Rack *opponent = player_get_rack(game_get_player(world, 1 - mover));
  rack_reset(opponent);
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    if (rest[ml] < 0) {
      log_fatal("peg3oracle world uses an unavailable tile");
    }
    for (int copy = 0; copy < rest[ml]; copy++) {
      rack_add_letter(opponent, (MachineLetter)ml);
    }
  }
  bag_set_to_tiles(game_get_bag(world), bag_tiles, bag_size);
}

// Plays the turns that still draw by a static sim for the player on turn, set
// as pegsim's arm 0 sims a root: the best `candidates` placements by equity,
// `iterations` rollouts each of `plies` plies, round robin, no BAI stop, no
// inference, no bag cycling, rollout PAT off, utility (1, 0.5, 100). The sim
// draws the opponent's rack and the bag from the unseen tiles itself
// (sim_rollout returns the opponent's rack to the bag first), so the player on
// turn knows neither. Each sim runs on one thread, seeded by the judge seed,
// the root, the world and the turn, so a world's result does not depend on
// the judge's thread count. One placement is played without a sim; a player
// with none plays static top 1 (a pass), counted in static_turns.
static void peg_judge_sim_drawing_turns(PegJudgeWorker *worker, int world_idx,
                                        int *static_turns) {
  const PegJudgeSim *sim = worker->sim;
  Game *game = worker->game;
  ErrorStack *errors = error_stack_create();
  int turns = 0;
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 0 &&
         turns < PEG_NET_MAX_TURNS) {
    Move next;
    const int n_placements = peg_world_all_placements(game, worker->sim_wide,
                                                      worker->sim_wide_moves);
    move_list_reset(worker->sim_pool);
    for (int rank = 0; rank < n_placements && rank < sim->candidates; rank++) {
      move_list_add_move(worker->sim_pool, &worker->sim_wide_moves[rank]);
    }
    move_list_set_rack(worker->sim_pool, move_list_get_rack(worker->sim_wide));
    const int cands = move_list_get_count(worker->sim_pool);
    if (cands == 0) {
      move_copy(&next, top_equity_move(game, worker->static_list));
      (*static_turns)++;
    } else if (cands == 1) {
      move_copy(&next, &worker->sim_wide_moves[0]);
    } else {
      const uint64_t seed = splitmix64(
          sim->seed ^ splitmix64(((uint64_t)worker->root_id << 24) ^
                                 ((uint64_t)world_idx << 8) ^ (uint64_t)turns));
      ThreadControl *control = thread_control_create();
      thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
      SimArgs sim_args;
      sim_args_fill(sim->plies, worker->sim_pool, cands, NULL, sim->win_pcts,
                    NULL, control, game, false, false, /*num_threads=*/1, 0,
                    cands, sim->plies, seed,
                    (uint64_t)sim->iterations * (uint64_t)cands, 1, 0.0,
                    BAI_THRESHOLD_NONE, 0.0, BAI_SAMPLING_RULE_ROUND_ROBIN, 0.0,
                    1.0, 0.5, 100.0, false, NULL, &sim_args);
      sim_args.bag_cycle = false;
      sim_args.pat_rollout_disabled = true;
      simulate_without_ctx(&sim_args, worker->sim_results, errors);
      if (!error_stack_is_empty(errors)) {
        error_stack_print_and_reset(errors);
        log_fatal("peg3oracle playout sim failed on root %d", worker->root_id);
      }
      thread_control_destroy(control);
      const int best = sim_results_get_best_move_index(worker->sim_results);
      move_copy(&next, simmed_play_get_move(sim_results_get_simmed_play(
                           worker->sim_results, best)));
    }
    play_move(&next, game, NULL);
    turns++;
  }
  error_stack_destroy(errors);
}

static void *peg_judge_worker(void *arg) {
  PegJudgeWorker *worker = arg;
  const int seat = game_get_player_on_turn_index(worker->root);
  int unseen[VALUE_NET_TILE_TYPES];
  peg_world_public_unseen(worker->root, unseen);
  while (true) {
    const int world_idx = atomic_fetch_add(worker->next, 1);
    if (world_idx >= worker->n_worlds) {
      break;
    }
    PegJudgeWorld *world = &worker->worlds[world_idx];
    peg_judge_set_world(worker->root, unseen, world->bag, worker->bag_size,
                        worker->game);
    play_move(worker->pick, worker->game, NULL);
    double reply_ms = 0;
    world->static_turns = 0;
    if (worker->sim != NULL) {
      peg_judge_sim_drawing_turns(worker, world_idx, &world->static_turns);
    } else {
      (void)peg3_play_drawing_turns(
          worker->game, worker->net, worker->chooser, worker->slot_idx,
          PEG2_REPLY_NET, worker->static_list, &reply_ms, &world->static_turns);
    }
    double final = spread_for_player(worker->game, seat);
    world->proven = true;
    world->capped = false;
    if (!game_over(worker->game)) {
      if (bag_get_letters(game_get_bag(worker->game)) != 0) {
        log_fatal("peg3oracle bag not empty before the exact solve");
      }
      const int on_turn = game_get_player_on_turn_index(worker->game);
      const int value =
          peg_exact_value(&worker->exact, worker->game, &world->proven);
      final += on_turn == seat ? value : -value;
      world->capped = worker->exact.capped;
    }
    world->final = final;
  }
  return NULL;
}

// The root move whose string (string_builder_add_move, as peg3eval writes it)
// is text: a placement from the wide pool or one of the static top 64. An
// empty text names the static top-64 candidate of rank code
// (peg_world_static_pool).
static bool peg_judge_find_move(const Game *root, const char *text, int code,
                                MoveList *wide, Move *wide_moves,
                                MoveList *pool, StringBuilder *name,
                                Move *move) {
  Move candidates[PEG_STATIC_POOL_SIZE];
  int pass_rank;
  if (text[0] == '\0') {
    const int count = peg_world_static_pool(root, pool, candidates, &pass_rank);
    if (code < 0 || code >= count) {
      return false;
    }
    move_copy(move, &candidates[code]);
    return true;
  }
  const int n_placements = peg_world_all_placements(root, wide, wide_moves);
  const int count = peg_world_static_pool(root, pool, candidates, &pass_rank);
  for (int idx = 0; idx < n_placements + count; idx++) {
    const Move *cand =
        idx < n_placements ? &wide_moves[idx] : &candidates[idx - n_placements];
    string_builder_clear(name);
    string_builder_add_move(name, game_get_board(root), cand, game_get_ld(root),
                            false);
    if (strings_equal(string_builder_peek(name), text)) {
      move_copy(move, cand);
      return true;
    }
  }
  return false;
}

// Two pick rows name the same move: equal texts, or equal ranks for rank
// picks (empty text).
static bool peg_judge_same_pick(const char *text, int code,
                                const char *other_text, int other_code) {
  return strings_equal(text, other_text) &&
         (text[0] != '\0' || code == other_code);
}

void peg_judge_mode(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 15) {
    log_fatal("peg3oracle needs positions prefix, picks, output prefix, "
              "plies, tt fraction, first, second|-, shortlist, w_spread, "
              "scale, 2PEG model, 2PEG first|-, stage N, top K");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *picks_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int plies = atoi(string_splitter_get_item(fields, 4));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 5), NULL);
  const char *threads_env = getenv("PEG_JUDGE_THREADS");
  const int threads =
      threads_env != NULL && atoi(threads_env) > 1 ? atoi(threads_env) : 1;
  const char *bag_env = getenv("PEG_JUDGE_BAG");
  const int bag_size = bag_env != NULL ? atoi(bag_env) : 3;
  const char *samples_env = getenv("PEG_JUDGE_SAMPLES");
  const int samples = samples_env != NULL ? atoi(samples_env) : 0;
  const char *seed_env = getenv("PEG_JUDGE_SEED");
  const uint64_t sample_seed =
      seed_env != NULL ? strtoull(seed_env, NULL, 10) : 0;
  const char *actual_env = getenv("PEG_JUDGE_ACTUAL_WEIGHT");
  const int actual_weight = actual_env != NULL ? atoi(actual_env) : 0;
  const char *sim_env = getenv("PEG_JUDGE_SIM");
  PegJudgeSim sim = {.seed = sample_seed};
  if (sim_env != NULL && sscanf(sim_env, "%d,%d,%d", &sim.candidates,
                                &sim.iterations, &sim.plies) != 3) {
    log_fatal("PEG_JUDGE_SIM must be CANDS,ITERS,PLIES");
  }
  if (plies < 1 || plies > 25 || tt_fraction <= 0 ||
      tt_fraction * threads > 0.4 || threads > PEG_JUDGE_MAX_THREADS ||
      bag_size < 1 ||
      bag_size > (samples > 0 ? PEG_JUDGE_MAX_BAG : PEG_JUDGE_MAX_ENUM_BAG) ||
      samples < 0 || samples + 1 > PEG_JUDGE_MAX_WORLDS || actual_weight < 0 ||
      actual_weight > 1 || (actual_weight > 0 && samples == 0) ||
      (sim_env != NULL && (sim.candidates < 2 || sim.iterations < 1 ||
                           sim.plies < 1 || sim.plies > MAX_PLIES))) {
    log_fatal("peg3oracle invalid plies, tt fraction, threads, bag, samples, "
              "actual weight or sim");
  }
  // The picks: root, code and move text per row.
  int num_picks = 0, pick_capacity = 64;
  int *pick_root = malloc_or_die(sizeof(int) * (size_t)pick_capacity);
  int *pick_code = malloc_or_die(sizeof(int) * (size_t)pick_capacity);
  char **pick_text = malloc_or_die(sizeof(char *) * (size_t)pick_capacity);
  char *line = NULL;
  size_t capacity = 0;
  FILE *picks = fopen_or_die(picks_path, "r");
  (void)getline_ignore_carriage_return(&line, &capacity, picks);
  while (getline_ignore_carriage_return(&line, &capacity, picks) > 0) {
    int root_id, code;
    char *open = strchr(line, '"'), *close = strrchr(line, '"');
    if (sscanf(line, "%d,%d,", &root_id, &code) != 2 || open == NULL ||
        close <= open) {
      log_fatal("peg3oracle invalid pick row");
    }
    *close = '\0';
    if (num_picks == pick_capacity) {
      pick_capacity *= 2;
      pick_root =
          realloc_or_die(pick_root, sizeof(int) * (size_t)pick_capacity);
      pick_code =
          realloc_or_die(pick_code, sizeof(int) * (size_t)pick_capacity);
      pick_text =
          realloc_or_die(pick_text, sizeof(char *) * (size_t)pick_capacity);
    }
    pick_root[num_picks] = root_id;
    pick_code[num_picks] = code;
    pick_text[num_picks] = string_duplicate(open + 1);
    num_picks++;
  }
  fclose(picks);
  // Roots already in the output are done.
  char *out_path = get_formatted_string("%s.oracle.csv", prefix);
  bool *root_done = calloc_or_die((size_t)num_picks, sizeof(bool));
  FILE *previous = fopen(out_path, "r");
  const bool fresh = previous == NULL;
  if (previous != NULL) {
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) > 0) {
      const int root_id = atoi(line);
      for (int idx = 0; idx < num_picks; idx++) {
        root_done[idx] |= pick_root[idx] == root_id;
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  char *worlds_path = get_formatted_string("%s.worlds.csv", prefix);
  FILE *worlds_out = fopen_or_die(worlds_path, "a");
  free(worlds_path);
  if (fresh) {
    fprintf(output, "root,pick,codes,tiles,worlds,weight,win,mean,capped,"
                    "unproven,static_worlds,actual_final,ms,move\n");
    fprintf(worlds_out,
            samples > 0 ? "root,pick,bag,weight,actual,final,proven\n"
                        : "root,pick,b0,b1,b2,weight,actual,final,proven\n");
  }
  Config *config = peg_net_config_create();
  ErrorStack *errors = error_stack_create();
  if (sim_env != NULL) {
    config_load_win_pcts(config, errors);
    if (!error_stack_is_empty(errors)) {
      error_stack_print_and_reset(errors);
      log_fatal("peg3oracle could not load the win percentages");
    }
    sim.win_pcts = config_get_win_pcts(config);
  }
  Game *root = config_get_game(config);
  Peg1NetChooser *net = peg1_net_chooser_create(
      string_splitter_get_item(fields, 6), string_splitter_get_item(fields, 7),
      atoi(string_splitter_get_item(fields, 8)),
      strtod(string_splitter_get_item(fields, 9), NULL),
      strtod(string_splitter_get_item(fields, 10), NULL), root, threads);
  Peg2NetChooser *chooser = peg2_net_chooser_create(
      string_splitter_get_item(fields, 11),
      string_splitter_get_item(fields, 12),
      atoi(string_splitter_get_item(fields, 13)),
      atoi(string_splitter_get_item(fields, 14)), root, threads);
  net->cache = true;
  chooser->cache = true;
  PegJudgeWorker workers[PEG_JUDGE_MAX_THREADS];
  cpthread_t thread_ids[PEG_JUDGE_MAX_THREADS];
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    workers[thread_idx] = (PegJudgeWorker){
        .net = net,
        .chooser = chooser,
        .slot_idx = thread_idx,
        .game = game_duplicate(root),
        .static_list = move_list_create(1),
        .exact = peg_exact_create(plies, tt_fraction),
    };
    if (sim_env != NULL) {
      workers[thread_idx].sim = &sim;
      workers[thread_idx].sim_wide =
          move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
      workers[thread_idx].sim_wide_moves =
          malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
      workers[thread_idx].sim_pool = move_list_create(sim.candidates);
      workers[thread_idx].sim_results = sim_results_create(0.0);
    }
  }
  PegJudgeWorld *worlds =
      malloc_or_die(sizeof(PegJudgeWorld) * PEG_JUDGE_MAX_WORLDS);
  MoveList *pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
  MoveList *wide = move_list_create(PEG_WORLD_ALL_PLACEMENTS_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * PEG_WORLD_ALL_PLACEMENTS_CAP);
  StringBuilder *name = string_builder_create();
  StringBuilder *codes = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id =
        peg_positions_next_root(positions, &line, &capacity, root, errors,
                                bag_size, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    // The root's distinct picks, in pick-file order.
    int distinct[PEG_JUDGE_MAX_PICKS];
    int n_distinct = 0;
    for (int idx = 0; idx < num_picks; idx++) {
      if (pick_root[idx] != root_id || root_done[idx]) {
        continue;
      }
      bool seen = false;
      for (int prev = 0; prev < n_distinct; prev++) {
        seen |= peg_judge_same_pick(pick_text[distinct[prev]],
                                    pick_code[distinct[prev]], pick_text[idx],
                                    pick_code[idx]);
      }
      if (!seen) {
        if (n_distinct == PEG_JUDGE_MAX_PICKS) {
          log_fatal("peg3oracle too many distinct picks for root %d", root_id);
        }
        distinct[n_distinct++] = idx;
      }
    }
    if (n_distinct == 0) {
      continue;
    }
    int unseen[VALUE_NET_TILE_TYPES];
    if (peg_world_public_unseen(root, unseen) != RACK_SIZE + bag_size) {
      log_fatal("peg3oracle root does not have %d unseen tiles",
                RACK_SIZE + bag_size);
    }
    peg1_net_chooser_reset(net);
    peg2_net_chooser_reset(chooser);
    for (int pick_idx = 0; pick_idx < n_distinct; pick_idx++) {
      const char *text = pick_text[distinct[pick_idx]];
      const int code = pick_code[distinct[pick_idx]];
      Move pick;
      if (!peg_judge_find_move(root, text, code, wide, wide_moves, pool, name,
                               &pick)) {
        log_fatal("peg3oracle root %d has no move %s (code %d)", root_id, text,
                  code);
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      const int n_worlds =
          samples > 0
              ? peg_judge_sampled_worlds(root, unseen, bag_size, samples,
                                         sample_seed + (uint64_t)root_id,
                                         actual_weight, worlds)
              : peg_judge_all_worlds(root, unseen, bag_size,
                                     move_get_tiles_played(&pick) >= bag_size,
                                     worlds);
      atomic_int next;
      atomic_init(&next, 0);
      const int64_t start = ctimer_monotonic_ns();
      for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
        workers[thread_idx].root = root;
        workers[thread_idx].root_id = root_id;
        workers[thread_idx].pick = &pick;
        workers[thread_idx].bag_size = bag_size;
        workers[thread_idx].worlds = worlds;
        workers[thread_idx].n_worlds = n_worlds;
        workers[thread_idx].next = &next;
        cpthread_create(&thread_ids[thread_idx], peg_judge_worker,
                        &workers[thread_idx]);
      }
      for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
        cpthread_join(thread_ids[thread_idx]);
      }
      const double ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
      int64_t weight_sum = 0;
      double win_sum = 0, spread_sum = 0, actual_final = NAN;
      int capped = 0, unproven = 0, static_worlds = 0;
      for (int world_idx = 0; world_idx < n_worlds; world_idx++) {
        const PegJudgeWorld *world = &worlds[world_idx];
        weight_sum += world->weight;
        win_sum += (double)world->weight * (world->final > 0   ? 1.0
                                            : world->final < 0 ? 0.0
                                                               : 0.5);
        spread_sum += (double)world->weight * world->final;
        capped += world->capped;
        unproven += !world->proven;
        static_worlds += world->static_turns > 0;
        if (world->actual) {
          actual_final = world->final;
        }
        if (samples > 0) {
          fprintf(worlds_out, "%d,%d,", root_id, pick_idx);
          for (int idx = 0; idx < bag_size; idx++) {
            fprintf(worlds_out, "%s%d", idx > 0 ? "." : "", world->bag[idx]);
          }
          fprintf(worlds_out, ",%lld,%d,%.9g,%d\n", (long long)world->weight,
                  world->actual, world->final, world->proven);
        } else {
          fprintf(worlds_out, "%d,%d,%d,%d,%d,%lld,%d,%.9g,%d\n", root_id,
                  pick_idx, world->bag[0], bag_size > 1 ? world->bag[1] : -1,
                  bag_size > 2 ? world->bag[2] : -1, (long long)world->weight,
                  world->actual, world->final, world->proven);
        }
      }
      string_builder_clear(codes);
      for (int idx = 0; idx < num_picks; idx++) {
        if (pick_root[idx] == root_id &&
            peg_judge_same_pick(pick_text[idx], pick_code[idx], text, code)) {
          string_builder_add_formatted_string(
              codes, "%s%d", string_builder_peek(codes)[0] ? ";" : "",
              pick_code[idx]);
        }
      }
      fprintf(output,
              "%d,%d,%s,%d,%d,%lld,%.9g,%.9g,%d,%d,%d,%.9g,%.9g,\"%s\"\n",
              root_id, pick_idx, string_builder_peek(codes),
              move_get_tiles_played(&pick), n_worlds, (long long)weight_sum,
              100.0 * win_sum / (double)weight_sum,
              spread_sum / (double)weight_sum, capped, unproven, static_worlds,
              actual_final, ms, string_builder_peek(name));
    }
    fflush(worlds_out);
    fflush(output);
    fsync(fileno(worlds_out));
    fsync(fileno(output));
  }
  free(line);
  fclose(positions);
  fclose(output);
  fclose(worlds_out);
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    game_destroy(workers[thread_idx].game);
    move_list_destroy(workers[thread_idx].static_list);
    peg_exact_destroy(&workers[thread_idx].exact);
    if (sim_env != NULL) {
      move_list_destroy(workers[thread_idx].sim_wide);
      free(workers[thread_idx].sim_wide_moves);
      move_list_destroy(workers[thread_idx].sim_pool);
      sim_results_destroy(workers[thread_idx].sim_results);
    }
  }
  for (int idx = 0; idx < num_picks; idx++) {
    free(pick_text[idx]);
  }
  free(pick_text);
  free(pick_root);
  free(pick_code);
  free(root_done);
  free(worlds);
  free(wide_moves);
  move_list_destroy(pool);
  move_list_destroy(wide);
  string_builder_destroy(name);
  string_builder_destroy(codes);
  error_stack_destroy(errors);
  peg1_net_chooser_destroy(net);
  peg2_net_chooser_destroy(chooser);
  config_destroy(config);
}
