#include "pat_rollout_value_test.h"

#include "../src/def/game_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "pat_move_choice_test.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Does PAT add value by steering forward-play *during* a Monte Carlo
// rollout, beyond whatever it already contributed to picking the candidates
// the rollout is handed? Both conditions below round-robin simulate the
// SAME frozen top-K static-equity candidate list (generated once, with PAT
// on, since that is what real candidate selection uses); they differ only
// in whether the rollout's own forward-play plies (get_top_equity_move,
// called at every simulated ply -- see random_variable.c's rv_sim_sample)
// have PAT available. If simming with PAT off in the rollout picks the same
// candidate as simming with it on, PAT's rollout-time steering was moot for
// that position. Where they disagree, this file's oracle resolves which
// candidate was actually better.
//
// This measures something narrower than pat_move_choice_test.c's harness:
// that one asks whether a PAT variant changes the single move a static (or
// reranked-static) chooser picks; this one holds the candidate list itself
// fixed and asks only whether PAT should also be consulted while resolving
// which of those already-PAT-informed candidates simulates best.
//
// Positions are the champion's own self-play, one per seed, played down to
// a bag size drawn uniformly from PAT_MOVE_CHOICE_DEFAULT_BAG_LO/HI (the
// same range and mechanism pat_move_choice_test.c uses), so they are
// independent of each other.
//
// TWO-PHASE DESIGN. Finding a disagreement (self-play position generation
// plus the round-robin PAT-on/PAT-off comparison above) and scoring one
// (the oracle below) are deliberately decoupled:
//   Phase 1 generates positions, freezes each one's top-K candidates,
//   round-robin sims them twice (PAT on/off in the rollout's forward
//   play), and -- on a disagreement -- both logs it to
//   PAT_ROLLOUT_VALUE_DISAGREEMENT_LOG_PATH (position CGP plus both
//   candidate move strings, appended, one run's worth per invocation) and
//   keeps it in memory for phase 2. Nothing in phase 1 depends on how
//   disagreements get scored.
//   Phase 2 scores every disagreement phase 1 found, with the nested-sim
//   oracle below, and times that step on its own (see
//   nested_sim_elapsed_seconds in the summary print), separately from
//   phase 1's cost. A future change to the oracle only touches phase 2;
//   it does not require regenerating positions or re-running the
//   round-robin comparison, and the log file is a durable record of what
//   phase 1 found even across process runs.
//
// SCALE: PAT_ROLLOUT_VALUE_NUM_POSITIONS was 80 for the mechanics-
// validation pilot (7-8 disagreements, ~10% rate); bumped to 12000 for the
// full run once the nested-sim oracle's cost and correctness were
// confirmed on the pilot's disagreements. Candidate count, ply depth and
// iterations-per-candidate are at the same scale pat_opening_sim_test.c
// uses for its own round-robin sim, since doubling the position count is
// cheap relative to doubling either of those.
#define PAT_ROLLOUT_VALUE_LEXICON "CSW21"
#define PAT_ROLLOUT_VALUE_NUM_CANDIDATES 12
#define PAT_ROLLOUT_VALUE_PLIES 4
#define PAT_ROLLOUT_VALUE_ITERATIONS_PER_CANDIDATE 400
#define PAT_ROLLOUT_VALUE_THREADS 10
#define PAT_ROLLOUT_VALUE_NUM_POSITIONS 12000
#define PAT_ROLLOUT_VALUE_NUM_WORLDS PAT_MOVE_CHOICE_DEFAULT_WORLDS
#define PAT_ROLLOUT_VALUE_POSITION_SEED_BASE 5100000000ULL
// Offsets applied to a position's own seed for the round-robin sim's
// internal RNG and for the disagreement scoring's paired worlds, so the
// three seeded streams (self-play position setup, round robin, reference
// continuation) never collide for the same attempt.
#define PAT_ROLLOUT_VALUE_SIM_SEED_OFFSET 111000000ULL
#define PAT_ROLLOUT_VALUE_WORLD_SEED_OFFSET 500000000ULL

#define PAT_ROLLOUT_VALUE_DISAGREEMENT_LOG_PATH                                \
  "test/pat_rollout_value_disagreements.log"

// ---------------------------------------------------------------------
// Nested-sim oracle.
//
// pat_move_choice_test.c's pat_move_choice_reference_value scores a
// disagreement by playing each candidate and then picking BOTH players'
// next pat_move_choice_reference_plies replies with a single
// get_top_equity_move lookup: the top of a static-equity list, no
// lookahead beyond that. That is a crude stand-in for what either side
// would actually do -- real play (including the very round-robin sim
// this file is adjudicating) chooses moves by simulating forward, not by
// taking the top of a static list. The oracle below replaces that lookup,
// at each of the reference continuation's plies, with the result of an
// actual (deliberately small) round-robin simulation: generate a top-K
// candidate list at that ply's position and Monte Carlo simulate all of
// them, then play whichever one the mini-sim liked best. This is a
// materially better approximation of real play than greedy lookahead; it
// is not a second full-fidelity simulation (see the constants below).
//
// PAT NEUTRALITY, verified rather than assumed. nested_sim_config (below)
// is created once, and no "-pat" (or "-pat1"/"-pat2") token is ever
// passed to it in this file. players_data.c documents PAT as strictly
// opt-in ("PAT weights are likewise opt-in: they load only when named
// explicitly with -pat") and players_data_create explicitly sets every
// player's PAT slot to NULL at creation before any opt-in flag is
// processed. nested_sim_config_create asserts player_get_pat() == NULL
// for both of its players immediately after creation and after every
// position load, rather than trusting the absence of "-pat" silently.
// move_gen.c's PAT branch is `if (pat && !args->disable_pat && ...)`
// where pat is player_get_pat(player) -- a NULL pat short-circuits it
// unconditionally, with no dependence on disable_pat at all. That NULL
// pat is read by BOTH of the two places this oracle's inner sim performs
// move generation: get_top_simming_move's own top-K `generate_moves`
// call (the inner sim's own candidate ranking), and every forward-play
// ply inside that same call's simulate() (the inner sim's own forward
// play, via get_top_equity_move -- see random_variable.c's
// rv_sim_sample). So PAT is inactive at *every* level of this oracle:
// its own candidate selection and its own simulated continuations, all
// the way down. Nothing about the oracle ever touches game's or
// rollout_game's players' PAT state either (see
// pat_nested_sim_reference_value below) -- the oracle never calls
// get_top_equity_move or generate_moves against them directly, only
// play_move with a move nested_sim_config already chose, so whatever PAT
// those players carry (the champion's, for `game`) is simply never read.
//
// The ONE place PAT is active anywhere in this file is the thing being
// measured: the outer round-robin comparison's PAT-on condition, in
// test_pat_rollout_value's phase 1, which is exactly what a PAT-neutral
// judge must not share an assumption with (see the file comment above
// and pat_rollout_value_test.h).
//
// COST. 8 candidates and 75 iterations are the midpoints of the ranges
// this feature was speced with (6-8 candidates, 50-100 iterations per
// continuation ply): enough breadth and volume to be a real simulation
// rather than a single sample, deliberately far short of the outer
// round-robin's own 12x400. 4 plies matches the outer round-robin's own
// ply depth (PAT_ROLLOUT_VALUE_PLIES) so the inner sim looks exactly as
// far ahead per decision as real candidate selection does; only breadth
// and volume are cut down, not lookahead depth. 4 threads is enough
// parallelism for a workload this small (600 total playouts per
// decision) without paying disproportionate thread-pool overhead on
// every one of the many nested-sim calls a single disagreement requires;
// see the file's test for the measured per-disagreement wall-clock cost
// this yields.
#define PAT_NESTED_SIM_NUM_CANDIDATES 8
#define PAT_NESTED_SIM_ITERATIONS_PER_CANDIDATE 75
#define PAT_NESTED_SIM_PLIES 4
#define PAT_NESTED_SIM_THREADS 4
// Distinguishes the inner sim's own seeded RNG stream from the outer
// round-robin's and the reference continuation's world-pairing stream
// (PAT_ROLLOUT_VALUE_SIM_SEED_OFFSET / _WORLD_SEED_OFFSET above).
#define PAT_NESTED_SIM_SEED_OFFSET 900000000ULL
// How many plies the reference continuation plays after the candidate;
// same convention (and same value) as
// pat_move_choice_test.c's pat_move_choice_reference_plies.
#define PAT_NESTED_SIM_REFERENCE_PLIES 2

// UTILITY BLEND (win% vs spread), applied explicitly to BOTH the outer
// round-robin comparison's sim config and this file's inner nested-sim
// oracle config, so neither relies on an unstated implicit default and
// both judge candidates by the same kind of criterion.
//
// compare_simmed_plays (src/ent/sim_results.c) only takes the continuous
// blended-utility path when utility_w_spread > 0.0; at exactly 0.0 it
// falls back to sorting by win% alone (with equity only as a tiebreak
// when win pcts are within a small cutoff of each other or of 0/100) --
// see sim_results.c's own comment on that branch. Win% is a coarse
// statistic at a small sample count (1/75 ~= 1.33 percentage points per
// step for this file's inner oracle, coarser than the outer round-robin's
// 1/400): two candidates whose true win rates differ can easily land on
// the identical sample win% and never reach a real gradient at all.
//
// The codebase's OWN established default for a real (non-degenerate)
// blend -- not something invented for this file -- is set at Config
// creation (config.c: config->utility_w_winpct = 1.0; utility_w_spread =
// 0.5; utility_spread_scale = 100.0), matching the -uwin/-uspread CLI
// help text verbatim ("Default 1.0, blended with the default uspread of
// 0.5" / "Default 0.5"). That is a real 2:1 win%:spread blend after
// normalization (sim_utility_blend in sim_args.h), NOT "blending
// disabled" -- the 0.0 value that actually disables blending is a
// different, explicit opt-out (see -uspread's help text: "set -uspread 0
// to restore the pure win% utility"), not this codebase's considered
// default. An older comment in sim_args.h describing "(1.0, 0.0, 100.0)"
// as the default is stale relative to config.c's actual initialization
// and should not be trusted -- confirmed against config.c directly, and
// against a live Config via config_get_utility_w_winpct/_w_spread/
// _spread_scale (see the verification prints below), not assumed from
// either comment.
//
// Both the outer sim (test_pat_rollout_value's "set" command, 400
// iterations/candidate) and this file's inner oracle sim (75
// iterations/candidate) use this SAME default blend: no documented
// precedent anywhere in this codebase (other tests, scripts, or config.c
// itself) recommends a different blend for a smaller-sample sim, and
// consistency of criterion between the two matters more here than
// hand-tuning weights per sample size would.
#define UTILITY_W_WINPCT_BLEND 1.0
#define UTILITY_W_SPREAD_BLEND 0.5
#define UTILITY_SPREAD_SCALE_BLEND 100.0

static Config *nested_sim_config_create(void) {
  char *set_cmd = get_formatted_string(
      "set -lex %s -s1 equity -s2 equity -r1 all -r2 all -numplays %d "
      "-plies %d -threads %d -iter %d -sr rr -scond none -threshold none "
      "-sinfer false -uwin %f -uspread %f -uspreadscale %f",
      PAT_ROLLOUT_VALUE_LEXICON, PAT_NESTED_SIM_NUM_CANDIDATES,
      PAT_NESTED_SIM_PLIES, PAT_NESTED_SIM_THREADS,
      PAT_NESTED_SIM_NUM_CANDIDATES * PAT_NESTED_SIM_ITERATIONS_PER_CANDIDATE,
      UTILITY_W_WINPCT_BLEND, UTILITY_W_SPREAD_BLEND,
      UTILITY_SPREAD_SCALE_BLEND);
  Config *nested_config = config_create_or_die(set_cmd);
  free(set_cmd);
  load_and_exec_config_or_die(
      nested_config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  // Structural PAT-neutrality, confirmed rather than assumed: see the file
  // comment above nested_sim_config_create's declaration.
  assert(player_get_pat(game_get_player(config_get_game(nested_config), 0)) ==
         NULL);
  assert(player_get_pat(game_get_player(config_get_game(nested_config), 1)) ==
         NULL);
  // Utility-blend verification, printed unconditionally (not just
  // asserted) because this file is run with NDEBUG in the release build
  // used for real timing, which compiles asserts out -- see the file
  // comment above these constants for why a nonzero spread weight matters
  // at this sim's small per-candidate sample count.
  const double actual_uwin = config_get_utility_w_winpct(nested_config);
  const double actual_uspread = config_get_utility_w_spread(nested_config);
  const double actual_uscale = config_get_utility_spread_scale(nested_config);
  fprintf(stderr,
          "[utility-verify] nested_config: uwin=%.4f uspread=%.4f "
          "uspreadscale=%.4f\n",
          actual_uwin, actual_uspread, actual_uscale);
  assert(actual_uwin == UTILITY_W_WINPCT_BLEND);
  assert(actual_uspread == UTILITY_W_SPREAD_BLEND);
  assert(actual_uscale == UTILITY_SPREAD_SCALE_BLEND);
  if (actual_uwin != UTILITY_W_WINPCT_BLEND ||
      actual_uspread != UTILITY_W_SPREAD_BLEND ||
      actual_uscale != UTILITY_SPREAD_SCALE_BLEND) {
    fprintf(stderr, "[utility-verify] FATAL: nested_config utility weights "
                    "did not take effect as requested\n");
    abort();
  }
  return nested_config;
}

// Picks a move for whichever player is on turn in rollout_game by
// round-robin simulating a small top-K candidate list, entirely within
// nested_config (which never has PAT loaded -- see above). rollout_game
// itself is read-only here: its position is round-tripped into
// nested_config through CGP (board, both racks, scores, consecutive-zero
// count -- everything the inner sim needs), so nested_config's own
// mini-rollout never touches rollout_game's players or their PAT state.
// game_get_cgp's write_player_on_turn_first=true puts the on-turn
// player's rack in the slot game_load_cgp always assigns index 0 to, so
// nested_config's "player 0" is the mover after every reload regardless
// of rollout_game's actual player indices.
static void nested_sim_pick_move(Config *nested_config,
                                 const Game *rollout_game, uint64_t seed,
                                 Move *move_out) {
  char *cgp = game_get_cgp(rollout_game, /*write_player_on_turn_first=*/true);
  char *cgp_cmd = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(nested_config, cgp_cmd);
  free(cgp_cmd);
  free(cgp);
  assert(player_get_pat(game_get_player(config_get_game(nested_config), 0)) ==
         NULL);
  assert(player_get_pat(game_get_player(config_get_game(nested_config), 1)) ==
         NULL);
  char *seed_cmd =
      get_formatted_string("set -seed %llu", (unsigned long long)seed);
  load_and_exec_config_or_die(nested_config, seed_cmd);
  free(seed_cmd);
  load_and_exec_config_or_die(nested_config, "gen");
  MoveList *nested_move_list = config_get_move_list(nested_config);
  if (move_list_get_count(nested_move_list) <= 1) {
    move_copy(move_out, move_list_get_move(nested_move_list, 0));
    return;
  }
  SimResults *nested_sim_results = config_get_sim_results(nested_config);
  const error_code_t status = config_simulate_and_return_status(
      nested_config, NULL, NULL, nested_sim_results);
  if (status != ERROR_STATUS_SUCCESS) {
    // A rare mini-sim failure shouldn't crash a pilot run; fall back to
    // the top of the (still PAT-free) static-equity list it already
    // generated above. PAT-neutrality is unaffected either way.
    move_copy(move_out, move_list_get_move(nested_move_list, 0));
    return;
  }
  move_copy(move_out, sim_results_get_best_move(nested_sim_results));
}

// Same shape and same final valuation (spread plus each side's KLV leave
// value at the horizon) as pat_move_choice_test.c's
// pat_move_choice_reference_value, but each continuation ply's move comes
// from nested_sim_pick_move instead of a single get_top_equity_move
// lookup. No reference_pat parameter: unlike the greedy oracle, nothing
// here ever runs move generation against `game`'s or rollout_game's own
// players (only play_move, which does not consult PAT), so their PAT
// state -- whatever it is -- is never read. rollout_game's players' KLV
// (independent of PAT; see player.c) still comes from `game`, exactly as
// in the greedy oracle, since the horizon valuation is unrelated to which
// oracle picked the continuation.
static double pat_nested_sim_reference_value(Config *nested_config,
                                             const Game *game, const Move *move,
                                             int mover_index,
                                             uint64_t world_seed) {
  Game *rollout_game = game_duplicate(game);
  game_seed(rollout_game, world_seed);
  const int opponent_index = 1 - mover_index;
  Rack last_leave[2];
  bool has_leave[2] = {false, false};
  for (int p = 0; p < 2; p++) {
    rack_set_dist_size(&last_leave[p], ld_get_size(game_get_ld(game)));
    rack_reset(&last_leave[p]);
  }
  play_move(move, rollout_game, &last_leave[mover_index]);
  has_leave[mover_index] = true;
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    set_random_rack(rollout_game, opponent_index, NULL);
  }
  Move reply;
  for (int ply = 0; ply < PAT_NESTED_SIM_REFERENCE_PLIES; ply++) {
    if (game_get_game_end_reason(rollout_game) != GAME_END_REASON_NONE) {
      break;
    }
    const int on_turn = game_get_player_on_turn_index(rollout_game);
    // A prime stride distinct from the one pairing worlds across
    // candidates (1000003, below), so per-ply nested-sim seeds don't
    // collide with the world-pairing sequence.
    const uint64_t nested_seed =
        world_seed + PAT_NESTED_SIM_SEED_OFFSET + (uint64_t)ply * 7919ULL;
    nested_sim_pick_move(nested_config, rollout_game, nested_seed, &reply);
    play_move(&reply, rollout_game, &last_leave[on_turn]);
    has_leave[on_turn] = true;
  }
  const Player *mover = game_get_player(rollout_game, mover_index);
  const Player *opponent = game_get_player(rollout_game, opponent_index);
  double value =
      equity_to_double(player_get_score(mover) - player_get_score(opponent));
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    if (has_leave[mover_index]) {
      value += equity_to_double(
          klv_get_leave_value(player_get_klv(mover), &last_leave[mover_index]));
    }
    if (has_leave[opponent_index]) {
      value -= equity_to_double(klv_get_leave_value(
          player_get_klv(opponent), &last_leave[opponent_index]));
    }
  }
  game_destroy(rollout_game);
  return value;
}

static void pat_nested_sim_reference_values(Config *nested_config,
                                            const Game *game, const Move *move,
                                            int mover_index, uint64_t base_seed,
                                            int num_worlds,
                                            double *values_out) {
  for (int world = 0; world < num_worlds; world++) {
    const uint64_t world_seed = base_seed + (uint64_t)world * 1000003ULL;
    values_out[world] = pat_nested_sim_reference_value(
        nested_config, game, move, mover_index, world_seed);
  }
}

// A disagreement found in phase 1, held in memory for phase 2. cgp is
// heap-owned (from game_get_cgp); freed once phase 2 has scored it.
typedef struct PendingDisagreement {
  uint64_t position_seed;
  char *cgp;
  Move move_pat_on;
  Move move_pat_off;
} PendingDisagreement;

static void log_disagreement(uint64_t position_seed, const char *cgp,
                             const Move *move_pat_on, const Move *move_pat_off,
                             const Board *board, const LetterDistribution *ld) {
  FILE *log_file = fopen(PAT_ROLLOUT_VALUE_DISAGREEMENT_LOG_PATH, "a");
  if (!log_file) {
    return;
  }
  StringBuilder *on_sb = string_builder_create();
  string_builder_add_move(on_sb, board, move_pat_on, ld, true);
  char *on_str = string_builder_dump(on_sb, NULL);
  string_builder_destroy(on_sb);
  StringBuilder *off_sb = string_builder_create();
  string_builder_add_move(off_sb, board, move_pat_off, ld, true);
  char *off_str = string_builder_dump(off_sb, NULL);
  string_builder_destroy(off_sb);
  fprintf(log_file, "seed=%llu\tcgp=%s\tpat_on=%s\tpat_off=%s\n",
          (unsigned long long)position_seed, cgp, on_str, off_str);
  fclose(log_file);
  free(on_str);
  free(off_str);
}

void test_pat_rollout_value(void) {
  Config *config = pat_move_choice_config_create();

  // Speed-only tables, when this lexicon has them; matches
  // pat_opening_sim_test.c's own setup (they change no move choice).
  char *wmp_path =
      get_formatted_string("./data/lexica/%s.wmp", PAT_ROLLOUT_VALUE_LEXICON);
  char *rit_path =
      get_formatted_string("./data/lexica/%s.rit", PAT_ROLLOUT_VALUE_LEXICON);
  char *wit_path =
      get_formatted_string("./data/lexica/%s.wit", PAT_ROLLOUT_VALUE_LEXICON);
  const bool have_wmp = access(wmp_path, R_OK) == 0;
  const bool have_rit = access(rit_path, R_OK) == 0;
  const bool have_wit = access(wit_path, R_OK) == 0;
  free(wmp_path);
  free(rit_path);
  free(wit_path);
  char *set_cmd = get_formatted_string(
      "set -wmp %s %s %s -numplays %d -plies %d -threads %d -iter %d -sr rr "
      "-scond none -threshold none -uwin %f -uspread %f -uspreadscale %f",
      have_wmp ? "true" : "false", have_rit ? "-rit true -ritmmap true" : "",
      have_wit ? "-wit true" : "", PAT_ROLLOUT_VALUE_NUM_CANDIDATES,
      PAT_ROLLOUT_VALUE_PLIES, PAT_ROLLOUT_VALUE_THREADS,
      PAT_ROLLOUT_VALUE_NUM_CANDIDATES *
          PAT_ROLLOUT_VALUE_ITERATIONS_PER_CANDIDATE,
      UTILITY_W_WINPCT_BLEND, UTILITY_W_SPREAD_BLEND,
      UTILITY_SPREAD_SCALE_BLEND);
  load_and_exec_config_or_die(config, set_cmd);
  free(set_cmd);
  // Utility-blend verification for the OUTER round-robin comparison's own
  // config -- printed unconditionally (see nested_sim_config_create's
  // identical check) since the release build used for the real run
  // compiles asserts out. This is the criterion that now defines "best
  // candidate" for BOTH conditions A and B below (PAT on/off in
  // rollout), i.e. it defines which positions count as disagreements.
  {
    const double actual_uwin = config_get_utility_w_winpct(config);
    const double actual_uspread = config_get_utility_w_spread(config);
    const double actual_uscale = config_get_utility_spread_scale(config);
    fprintf(stderr,
            "[utility-verify] outer config: uwin=%.4f uspread=%.4f "
            "uspreadscale=%.4f\n",
            actual_uwin, actual_uspread, actual_uscale);
    assert(actual_uwin == UTILITY_W_WINPCT_BLEND);
    assert(actual_uspread == UTILITY_W_SPREAD_BLEND);
    assert(actual_uscale == UTILITY_SPREAD_SCALE_BLEND);
    if (actual_uwin != UTILITY_W_WINPCT_BLEND ||
        actual_uspread != UTILITY_W_SPREAD_BLEND ||
        actual_uscale != UTILITY_SPREAD_SCALE_BLEND) {
      fprintf(stderr, "[utility-verify] FATAL: outer config utility weights "
                      "did not take effect as requested\n");
      abort();
    }
  }

  Game *game = config_get_game(config);
  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  SimResults *sim_results = config_get_sim_results(config);
  MoveList *setup_move_list = move_list_create(1);

  PendingDisagreement *pending = malloc_or_die(sizeof(PendingDisagreement) *
                                               PAT_ROLLOUT_VALUE_NUM_POSITIONS);
  int num_pending = 0;

  int num_positions_considered = 0;
  int num_disagreements = 0;

  // ---- Phase 1: generate positions, find disagreements, log them. ----
  for (int attempt = 0; attempt < PAT_ROLLOUT_VALUE_NUM_POSITIONS; attempt++) {
    const uint64_t position_seed =
        PAT_ROLLOUT_VALUE_POSITION_SEED_BASE + (uint64_t)attempt;
    game_reset(game);
    game_seed(game, position_seed);
    draw_starting_racks(game);
    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);
    const int target_bag =
        PAT_MOVE_CHOICE_DEFAULT_BAG_LO +
        (int)(position_seed % (uint64_t)(PAT_MOVE_CHOICE_DEFAULT_BAG_HI -
                                         PAT_MOVE_CHOICE_DEFAULT_BAG_LO + 1));
    bool position_ok = true;
    while (bag_get_letters(game_get_bag(game)) > target_bag) {
      const Move *setup_move = get_top_equity_move(game, setup_move_list);
      play_move(setup_move, game, NULL);
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        position_ok = false;
        break;
      }
    }
    if (!position_ok || bag_get_letters(game_get_bag(game)) == 0) {
      continue;
    }
    const Board *board = game_get_board(game);
    if (board_get_transposed(board) || !board_get_cross_sets_valid(board)) {
      continue;
    }
    num_positions_considered++;

    // Step 2a: freeze the top-K static-equity candidates (PAT on, since
    // this is what real candidate selection uses). Both conditions below
    // round-robin simulate this exact list; neither regenerates it.
    load_and_exec_config_or_die(config, "gen");
    if (move_list_get_count(config_get_move_list(config)) < 2) {
      continue; // nothing to disagree about
    }

    // Same seed for both conditions per position, set once: config->seed
    // is untouched by simulate() itself, so it carries over unchanged from
    // condition A to condition B below (the shared-randomness-within-a-pair
    // principle pat_move_choice_test.c's world seeding also relies on).
    char *seed_cmd = get_formatted_string(
        "set -seed %llu",
        (unsigned long long)(position_seed +
                             PAT_ROLLOUT_VALUE_SIM_SEED_OFFSET));
    load_and_exec_config_or_die(config, seed_cmd);
    free(seed_cmd);

    // Condition A: PAT active during rollout forward-play (the flag's
    // default, unset state -- current behavior, unchanged).
    const error_code_t status_on =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    if (status_on != ERROR_STATUS_SUCCESS) {
      continue;
    }
    Move move_pat_on;
    move_copy(&move_pat_on, sim_results_get_best_move(sim_results));

    // Condition B: PAT forced off for the rollout's forward-play plies
    // only; the frozen candidate list above is untouched by this flag.
    player_set_rollout_disable_pat(player0, true);
    player_set_rollout_disable_pat(player1, true);
    const error_code_t status_off =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);
    if (status_off != ERROR_STATUS_SUCCESS) {
      continue;
    }
    Move move_pat_off;
    move_copy(&move_pat_off, sim_results_get_best_move(sim_results));

    if (move_get_type(&move_pat_on) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_type(&move_pat_off) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    if (compare_moves_without_equity(&move_pat_on, &move_pat_off, true) == -1) {
      continue; // the two conditions agree; not informative
    }
    num_disagreements++;

    // Log first (position CGP plus both candidate move strings), then
    // keep it for phase 2. Logging never depends on how (or whether) the
    // disagreement ends up scored.
    char *cgp = game_get_cgp(game, /*write_player_on_turn_first=*/true);
    log_disagreement(position_seed, cgp, &move_pat_on, &move_pat_off, board,
                     game_get_ld(game));
    pending[num_pending].position_seed = position_seed;
    pending[num_pending].cgp = cgp; // ownership transferred
    move_copy(&pending[num_pending].move_pat_on, &move_pat_on);
    move_copy(&pending[num_pending].move_pat_off, &move_pat_off);
    num_pending++;

    if ((attempt + 1) % 20 == 0) {
      printf("  %d/%d positions attempted (%d considered, %d "
             "disagreements)\n",
             attempt + 1, PAT_ROLLOUT_VALUE_NUM_POSITIONS,
             num_positions_considered, num_disagreements);
      fflush(stdout);
    }
  }

  move_list_destroy(setup_move_list);

  printf("\n[PAT off in rollout] vs [PAT on in rollout], frozen top-%d "
         "candidates, %d plies, %d iterations/candidate: %d positions "
         "considered, %d disagreements (%.2f%%), logged to %s\n",
         PAT_ROLLOUT_VALUE_NUM_CANDIDATES, PAT_ROLLOUT_VALUE_PLIES,
         PAT_ROLLOUT_VALUE_ITERATIONS_PER_CANDIDATE, num_positions_considered,
         num_disagreements,
         num_positions_considered > 0
             ? 100.0 * num_disagreements / num_positions_considered
             : 0.0,
         PAT_ROLLOUT_VALUE_DISAGREEMENT_LOG_PATH);

  // ---- Phase 2: score every disagreement phase 1 found. For context,
  // each disagreement is scored three ways: greedy-PAT-aware (the
  // pre-fix design: pat_move_choice_reference_value's 2-ply continuation
  // with the champion's PAT active, reference_pat set and
  // rollout_disable_pat left false -- what a from-scratch rerun of the
  // *original* oracle would have produced on these same disagreements,
  // since no artifact of the original 80-position pilot's actual scores
  // survived to compare against directly; see the file/task history),
  // greedy-PAT-neutral (the same 2-ply continuation with
  // rollout_disable_pat forced true, i.e. the fix that predates this
  // file's nested-sim oracle), and nested-sim-PAT-neutral (this file's
  // oracle, below). Only the nested-sim step is wall-clock timed here --
  // that is the expensive, newly-introduced cost this pilot exists to
  // measure; the two greedy comparisons are cheap and exist only to show
  // how much rescoring the same 7 disagreements shifts the answer.
  Config *nested_config = nested_sim_config_create();
  const PATWeights *reference_pat = player_get_pat(player0);
  assert(reference_pat);
  double sum_means = 0.0;
  double sum_means_sq = 0.0;
  double sum_within = 0.0;
  double sum_means_greedy_aware = 0.0;
  double sum_means_greedy_neutral = 0.0;
  double values_pat_on[PAT_ROLLOUT_VALUE_NUM_WORLDS];
  double values_pat_off[PAT_ROLLOUT_VALUE_NUM_WORLDS];

  double oracle_elapsed_seconds = 0.0;

  for (int i = 0; i < num_pending; i++) {
    // Reload phase 1's exact position (board, both racks, scores,
    // consecutive-zero count) into config's own game. load_and_exec_config
    // does not retain cmd beyond this call (same pattern as seed_cmd
    // above), so it is freed immediately after.
    char *cgp_cmd = get_formatted_string("cgp %s", pending[i].cgp);
    load_and_exec_config_or_die(config, cgp_cmd);
    free(cgp_cmd);
    const Game *scored_game = config_get_game(config);
    const int mover_index = game_get_player_on_turn_index(scored_game);
    const uint64_t world_seed =
        pending[i].position_seed + PAT_ROLLOUT_VALUE_WORLD_SEED_OFFSET;

    // Greedy-PAT-aware: the original (pre-fix) design. player0/1's
    // rollout_disable_pat is false here (phase 1 always restores it), so
    // pat_move_choice_reference_value's continuation plies see PAT active.
    pat_move_choice_reference_values(
        scored_game, &pending[i].move_pat_on, mover_index, reference_pat,
        world_seed, PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_on);
    pat_move_choice_reference_values(
        scored_game, &pending[i].move_pat_off, mover_index, reference_pat,
        world_seed, PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_off);
    double sum_d_greedy_aware = 0.0;
    for (int world = 0; world < PAT_ROLLOUT_VALUE_NUM_WORLDS; world++) {
      sum_d_greedy_aware += values_pat_off[world] - values_pat_on[world];
    }
    const double position_mean_greedy_aware =
        sum_d_greedy_aware / PAT_ROLLOUT_VALUE_NUM_WORLDS;
    sum_means_greedy_aware += position_mean_greedy_aware;

    // Greedy-PAT-neutral: the fix that predates the nested-sim oracle --
    // same 2-ply continuation, PAT forced off for both players first (see
    // pat_rollout_value_test.h and player_set_rollout_disable_pat).
    player_set_rollout_disable_pat(player0, true);
    player_set_rollout_disable_pat(player1, true);
    pat_move_choice_reference_values(
        scored_game, &pending[i].move_pat_on, mover_index, reference_pat,
        world_seed, PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_on);
    pat_move_choice_reference_values(
        scored_game, &pending[i].move_pat_off, mover_index, reference_pat,
        world_seed, PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_off);
    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);
    double sum_d_greedy_neutral = 0.0;
    for (int world = 0; world < PAT_ROLLOUT_VALUE_NUM_WORLDS; world++) {
      sum_d_greedy_neutral += values_pat_off[world] - values_pat_on[world];
    }
    const double position_mean_greedy_neutral =
        sum_d_greedy_neutral / PAT_ROLLOUT_VALUE_NUM_WORLDS;
    sum_means_greedy_neutral += position_mean_greedy_neutral;

    // Nested-sim-PAT-neutral: this file's oracle. Timed on its own.
    struct timespec oracle_start;
    struct timespec oracle_end;
    clock_gettime(CLOCK_MONOTONIC, &oracle_start);
    pat_nested_sim_reference_values(
        nested_config, scored_game, &pending[i].move_pat_on, mover_index,
        world_seed, PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_on);
    pat_nested_sim_reference_values(
        nested_config, scored_game, &pending[i].move_pat_off, mover_index,
        world_seed, PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_off);
    clock_gettime(CLOCK_MONOTONIC, &oracle_end);
    oracle_elapsed_seconds +=
        (double)(oracle_end.tv_sec - oracle_start.tv_sec) +
        (double)(oracle_end.tv_nsec - oracle_start.tv_nsec) / 1.0e9;
    double sum_d = 0.0;
    double sum_d_sq = 0.0;
    for (int world = 0; world < PAT_ROLLOUT_VALUE_NUM_WORLDS; world++) {
      const double d = values_pat_off[world] - values_pat_on[world];
      sum_d += d;
      sum_d_sq += d * d;
    }
    const double position_mean = sum_d / PAT_ROLLOUT_VALUE_NUM_WORLDS;
    const double position_var = (sum_d_sq / PAT_ROLLOUT_VALUE_NUM_WORLDS -
                                 position_mean * position_mean) *
                                ((double)PAT_ROLLOUT_VALUE_NUM_WORLDS /
                                 (PAT_ROLLOUT_VALUE_NUM_WORLDS - 1));
    sum_means += position_mean;
    sum_means_sq += position_mean * position_mean;
    sum_within += position_var;
    printf("  scored disagreement %d/%d (seed=%llu): greedy-aware %.2f, "
           "greedy-neutral %.2f, nested-sim %.2f\n",
           i + 1, num_pending, (unsigned long long)pending[i].position_seed,
           position_mean_greedy_aware, position_mean_greedy_neutral,
           position_mean);
    fflush(stdout);
  }

  printf("\nnested-sim oracle: scored %d disagreements in %.1fs (%.2fs per "
         "disagreement, %d candidates x %d iterations x %d plies per "
         "continuation-ply decision)\n",
         num_pending, oracle_elapsed_seconds,
         num_pending > 0 ? oracle_elapsed_seconds / num_pending : 0.0,
         PAT_NESTED_SIM_NUM_CANDIDATES, PAT_NESTED_SIM_ITERATIONS_PER_CANDIDATE,
         PAT_NESTED_SIM_PLIES);

  if (num_pending > 0) {
    printf("  mean paired effect by scoring method, same %d disagreements: "
           "greedy-PAT-aware (pre-fix) %.4f, greedy-PAT-neutral %.4f, "
           "nested-sim-PAT-neutral %.4f\n",
           num_pending, sum_means_greedy_aware / num_pending,
           sum_means_greedy_neutral / num_pending, sum_means / num_pending);
  }

  if (num_pending > 1) {
    const int n = num_pending;
    const double mean = sum_means / n;
    const double var_means =
        (sum_means_sq / n - mean * mean) * ((double)n / (n - 1));
    const double se = sqrt(var_means / n);
    const double within = sum_within / n;
    const double between = var_means - within / PAT_ROLLOUT_VALUE_NUM_WORLDS;
    printf("  paired effect (PAT off in rollout - PAT on in rollout), "
           "nested-sim-scored: mean %.4f, SE %.4f, 95%% CI [%.4f, %.4f]\n",
           mean, se, mean - 1.96 * se, mean + 1.96 * se);
    printf("  effect per sampled decision (mean x disagreement rate): "
           "%.4f\n",
           mean * n / num_positions_considered);
    printf("  variance decomposition: within-position %.2f, "
           "between-position %.2f (R = %d worlds); shares of Var(mean): "
           "between %.1f%%, within %.1f%%\n",
           within, between, PAT_ROLLOUT_VALUE_NUM_WORLDS,
           100.0 * between / var_means,
           100.0 * (within / PAT_ROLLOUT_VALUE_NUM_WORLDS) / var_means);
  }

  for (int i = 0; i < num_pending; i++) {
    free(pending[i].cgp);
  }
  free(pending);
  config_destroy(nested_config);

  // Sanity check on the mechanics, not the finding: position generation
  // (self-play down to a random bag size) should succeed for the large
  // majority of attempts, the same way test_pat_move_choice_controls
  // asserts on its own position yield.
  assert(num_positions_considered > PAT_ROLLOUT_VALUE_NUM_POSITIONS / 2);
  config_destroy(config);
}

// ---------------------------------------------------------------------
// Continuous accumulation mode.
//
// test_pat_rollout_value above finds all disagreements over a fixed
// position count first (phase 1), then scores all of them (phase 2).
// That batch boundary means an interrupted run can leave disagreements
// found-but-unscored. This entry point removes the boundary: for each
// position, run the outer PAT-on/PAT-off comparison, and if it is a
// disagreement, immediately run the nested-sim oracle on it right there
// and log the fully-scored result in one line, before moving to the next
// position. At any point this is stopped, everything already logged is
// already fully scored -- there is no dangling found-but-unscored batch.
//
// It runs for a wall-clock time budget (PAT_ROLLOUT_VALUE_TIME_BUDGET_SECONDS)
// rather than a fixed position count, checked only between positions (so a
// position or its disagreement's oracle scoring, once started, always
// finishes -- the budget check never cuts one off mid-flight). Positions
// use a continuously incrementing seed starting from
// PAT_ROLLOUT_VALUE_CONTINUOUS_SEED_START, chosen to not overlap the
// PAT_ROLLOUT_VALUE_NUM_POSITIONS (12000) positions test_pat_rollout_value
// already consumed (seeds PAT_ROLLOUT_VALUE_POSITION_SEED_BASE + [0, 12000))
// -- so this mode's positions are always genuinely new, never a repeat of
// what is already in test/pat_rollout_value_disagreements.log. There is no
// upper bound on how many positions/disagreements a run of this accumulates
// (limited only by the time budget), so unlike test_pat_rollout_value there
// is no PendingDisagreement array to size in advance: each disagreement is
// scored and logged immediately and nothing needs to be held in memory
// past that point.
//
// Same validated methodology throughout, unchanged from
// test_pat_rollout_value: same utility blend (UTILITY_W_*_BLEND) on both
// the outer config and the nested-sim oracle's own config, same PAT
// neutrality guarantees on the oracle (see nested_sim_config_create's file
// comment), same PAT_ROLLOUT_VALUE_NUM_CANDIDATES/PLIES/
// ITERATIONS_PER_CANDIDATE/THREADS and PAT_NESTED_SIM_* constants. Every
// disagreement is scored three ways (greedy-PAT-aware, greedy-PAT-neutral,
// nested-sim-PAT-neutral) exactly as test_pat_rollout_value's phase 2 did,
// for the same reference-vs-nested-sim comparison; only the timing and
// control flow changed.
//
// Different log file from test_pat_rollout_value's
// PAT_ROLLOUT_VALUE_DISAGREEMENT_LOG_PATH: that file's lines have no score
// fields (logged before scoring existed as a separate phase), so this
// mode's fully-scored lines use their own, clearly-distinguished path and
// format (see log_scored_disagreement) rather than silently changing the
// meaning of existing lines an old parser might expect.
#define PAT_ROLLOUT_VALUE_TIME_BUDGET_SECONDS (11.0 * 3600.0)
#define PAT_ROLLOUT_VALUE_CHECKPOINT_INTERVAL_SECONDS 3600.0
// Originally set to PAT_ROLLOUT_VALUE_NUM_POSITIONS so this mode's seeds
// would never repeat test_pat_rollout_value's own 12000 positions
// (5100000000..5100011999) during the utility-blend-era accumulation run.
// Reset to 0 for the horizon-fix restart: that run's data (both
// test/pat_rollout_value_disagreements.log and
// test/pat_rollout_value_scored_disagreements.log) was archived to
// *.pre-horizon-fix.log rather than left live, specifically so this
// restart is free to reuse the whole seed space from scratch under the
// new (horizon-fix) net-value formula -- there is nothing live left for
// seed 5100000000 to collide with.
#define PAT_ROLLOUT_VALUE_CONTINUOUS_SEED_START 0ULL

#define PAT_ROLLOUT_VALUE_SCORED_LOG_PATH                                      \
  "test/pat_rollout_value_scored_disagreements.log"

// One fully-scored disagreement per line: position CGP, both candidate
// move strings, and all three methods' paired-difference value (plus the
// nested-sim value's own across-worlds variance, enough to redo the same
// within/between variance decomposition test_pat_rollout_value's phase 2
// prints, pooled across every line ever logged here). Appends -- never
// truncates -- so repeated runs (or one very long one) accumulate a single
// pooled dataset, per the same durability rationale as log_disagreement.
static void
log_scored_disagreement(uint64_t position_seed, const char *cgp,
                        const Move *move_pat_on, const Move *move_pat_off,
                        const Board *board, const LetterDistribution *ld,
                        double greedy_aware, double greedy_neutral,
                        double nested_sim, double nested_sim_position_var) {
  FILE *log_file = fopen(PAT_ROLLOUT_VALUE_SCORED_LOG_PATH, "a");
  if (!log_file) {
    return;
  }
  StringBuilder *on_sb = string_builder_create();
  string_builder_add_move(on_sb, board, move_pat_on, ld, true);
  char *on_str = string_builder_dump(on_sb, NULL);
  string_builder_destroy(on_sb);
  StringBuilder *off_sb = string_builder_create();
  string_builder_add_move(off_sb, board, move_pat_off, ld, true);
  char *off_str = string_builder_dump(off_sb, NULL);
  string_builder_destroy(off_sb);
  fprintf(log_file,
          "seed=%llu\tcgp=%s\tpat_on=%s\tpat_off=%s\tgreedy_aware=%.6f\t"
          "greedy_neutral=%.6f\tnested_sim=%.6f\tnested_sim_var=%.6f\n",
          (unsigned long long)position_seed, cgp, on_str, off_str, greedy_aware,
          greedy_neutral, nested_sim, nested_sim_position_var);
  fclose(log_file);
  free(on_str);
  free(off_str);
}

static double elapsed_seconds_since(const struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)(now.tv_sec - start->tv_sec) +
         (double)(now.tv_nsec - start->tv_nsec) / 1.0e9;
}

void test_pat_rollout_value_accumulate(void) {
  Config *config = pat_move_choice_config_create();

  char *wmp_path =
      get_formatted_string("./data/lexica/%s.wmp", PAT_ROLLOUT_VALUE_LEXICON);
  char *rit_path =
      get_formatted_string("./data/lexica/%s.rit", PAT_ROLLOUT_VALUE_LEXICON);
  char *wit_path =
      get_formatted_string("./data/lexica/%s.wit", PAT_ROLLOUT_VALUE_LEXICON);
  const bool have_wmp = access(wmp_path, R_OK) == 0;
  const bool have_rit = access(rit_path, R_OK) == 0;
  const bool have_wit = access(wit_path, R_OK) == 0;
  free(wmp_path);
  free(rit_path);
  free(wit_path);
  char *set_cmd = get_formatted_string(
      "set -wmp %s %s %s -numplays %d -plies %d -threads %d -iter %d -sr rr "
      "-scond none -threshold none -uwin %f -uspread %f -uspreadscale %f",
      have_wmp ? "true" : "false", have_rit ? "-rit true -ritmmap true" : "",
      have_wit ? "-wit true" : "", PAT_ROLLOUT_VALUE_NUM_CANDIDATES,
      PAT_ROLLOUT_VALUE_PLIES, PAT_ROLLOUT_VALUE_THREADS,
      PAT_ROLLOUT_VALUE_NUM_CANDIDATES *
          PAT_ROLLOUT_VALUE_ITERATIONS_PER_CANDIDATE,
      UTILITY_W_WINPCT_BLEND, UTILITY_W_SPREAD_BLEND,
      UTILITY_SPREAD_SCALE_BLEND);
  load_and_exec_config_or_die(config, set_cmd);
  free(set_cmd);
  {
    const double actual_uwin = config_get_utility_w_winpct(config);
    const double actual_uspread = config_get_utility_w_spread(config);
    const double actual_uscale = config_get_utility_spread_scale(config);
    fprintf(stderr,
            "[utility-verify] outer config: uwin=%.4f uspread=%.4f "
            "uspreadscale=%.4f\n",
            actual_uwin, actual_uspread, actual_uscale);
    assert(actual_uwin == UTILITY_W_WINPCT_BLEND);
    assert(actual_uspread == UTILITY_W_SPREAD_BLEND);
    assert(actual_uscale == UTILITY_SPREAD_SCALE_BLEND);
    if (actual_uwin != UTILITY_W_WINPCT_BLEND ||
        actual_uspread != UTILITY_W_SPREAD_BLEND ||
        actual_uscale != UTILITY_SPREAD_SCALE_BLEND) {
      fprintf(stderr, "[utility-verify] FATAL: outer config utility weights "
                      "did not take effect as requested\n");
      abort();
    }
  }

  Game *game = config_get_game(config);
  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  SimResults *sim_results = config_get_sim_results(config);
  MoveList *setup_move_list = move_list_create(1);
  const PATWeights *reference_pat = player_get_pat(player0);
  assert(reference_pat);

  Config *nested_config = nested_sim_config_create();

  double values_pat_on[PAT_ROLLOUT_VALUE_NUM_WORLDS];
  double values_pat_off[PAT_ROLLOUT_VALUE_NUM_WORLDS];

  long num_positions_considered = 0;
  long num_disagreements = 0;
  double sum_means = 0.0;
  double sum_means_sq = 0.0;
  double sum_within = 0.0;
  double nested_sim_elapsed_seconds = 0.0;

  struct timespec run_start;
  clock_gettime(CLOCK_MONOTONIC, &run_start);
  struct timespec last_checkpoint = run_start;

  printf("[accumulate] time budget %.1f hours, checkpoint every %.0f "
         "minutes, seeds starting at %llu, scored results -> %s\n",
         PAT_ROLLOUT_VALUE_TIME_BUDGET_SECONDS / 3600.0,
         PAT_ROLLOUT_VALUE_CHECKPOINT_INTERVAL_SECONDS / 60.0,
         (unsigned long long)(PAT_ROLLOUT_VALUE_POSITION_SEED_BASE +
                              PAT_ROLLOUT_VALUE_CONTINUOUS_SEED_START),
         PAT_ROLLOUT_VALUE_SCORED_LOG_PATH);
  fflush(stdout);

  for (uint64_t attempt = 0;; attempt++) {
    if (elapsed_seconds_since(&run_start) >=
        PAT_ROLLOUT_VALUE_TIME_BUDGET_SECONDS) {
      printf("[accumulate] time budget reached; stopping before starting a "
             "new position (nothing in flight was cut off)\n");
      break;
    }
    if (elapsed_seconds_since(&last_checkpoint) >=
        PAT_ROLLOUT_VALUE_CHECKPOINT_INTERVAL_SECONDS) {
      clock_gettime(CLOCK_MONOTONIC, &last_checkpoint);
      const double total_elapsed = elapsed_seconds_since(&run_start);
      printf("\n[accumulate checkpoint] elapsed %.2fh: %ld positions "
             "considered, %ld disagreements (%.2f%%)\n",
             total_elapsed / 3600.0, num_positions_considered,
             num_disagreements,
             num_positions_considered > 0 ? 100.0 * (double)num_disagreements /
                                                (double)num_positions_considered
                                          : 0.0);
      if (num_disagreements > 1) {
        const long n = num_disagreements;
        const double mean = sum_means / (double)n;
        const double var_means = (sum_means_sq / (double)n - mean * mean) *
                                 ((double)n / (double)(n - 1));
        const double se = sqrt(var_means / (double)n);
        printf("[accumulate checkpoint] pooled nested-sim-scored paired "
               "effect: n=%ld mean=%.4f SE=%.4f 95%%CI=[%.4f, %.4f]\n",
               n, mean, se, mean - 1.96 * se, mean + 1.96 * se);
      }
      printf("[accumulate checkpoint] nested-sim scoring: %.1fs total, "
             "%.2fs/disagreement so far\n\n",
             nested_sim_elapsed_seconds,
             num_disagreements > 0
                 ? nested_sim_elapsed_seconds / (double)num_disagreements
                 : 0.0);
      fflush(stdout);
    }

    const uint64_t position_seed = PAT_ROLLOUT_VALUE_POSITION_SEED_BASE +
                                   PAT_ROLLOUT_VALUE_CONTINUOUS_SEED_START +
                                   attempt;
    game_reset(game);
    game_seed(game, position_seed);
    draw_starting_racks(game);
    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);
    const int target_bag =
        PAT_MOVE_CHOICE_DEFAULT_BAG_LO +
        (int)(position_seed % (uint64_t)(PAT_MOVE_CHOICE_DEFAULT_BAG_HI -
                                         PAT_MOVE_CHOICE_DEFAULT_BAG_LO + 1));
    bool position_ok = true;
    while (bag_get_letters(game_get_bag(game)) > target_bag) {
      const Move *setup_move = get_top_equity_move(game, setup_move_list);
      play_move(setup_move, game, NULL);
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        position_ok = false;
        break;
      }
    }
    if (!position_ok || bag_get_letters(game_get_bag(game)) == 0) {
      continue;
    }
    const Board *board = game_get_board(game);
    if (board_get_transposed(board) || !board_get_cross_sets_valid(board)) {
      continue;
    }
    num_positions_considered++;

    load_and_exec_config_or_die(config, "gen");
    if (move_list_get_count(config_get_move_list(config)) < 2) {
      continue;
    }

    char *seed_cmd = get_formatted_string(
        "set -seed %llu",
        (unsigned long long)(position_seed +
                             PAT_ROLLOUT_VALUE_SIM_SEED_OFFSET));
    load_and_exec_config_or_die(config, seed_cmd);
    free(seed_cmd);

    const error_code_t status_on =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    if (status_on != ERROR_STATUS_SUCCESS) {
      continue;
    }
    Move move_pat_on;
    move_copy(&move_pat_on, sim_results_get_best_move(sim_results));

    player_set_rollout_disable_pat(player0, true);
    player_set_rollout_disable_pat(player1, true);
    const error_code_t status_off =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);
    if (status_off != ERROR_STATUS_SUCCESS) {
      continue;
    }
    Move move_pat_off;
    move_copy(&move_pat_off, sim_results_get_best_move(sim_results));

    if (move_get_type(&move_pat_on) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_type(&move_pat_off) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    if (compare_moves_without_equity(&move_pat_on, &move_pat_off, true) == -1) {
      continue;
    }
    num_disagreements++;

    // Inline oracle scoring -- right here, not deferred to a later phase.
    // `game` is still exactly at the disagreement position (nothing below
    // mutates it: pat_move_choice_reference_value(s) and
    // pat_nested_sim_reference_value(s) each duplicate it internally), so
    // no CGP round-trip reload is needed the way test_pat_rollout_value's
    // separate phase 2 required.
    const int mover_index = game_get_player_on_turn_index(game);
    const uint64_t world_seed =
        position_seed + PAT_ROLLOUT_VALUE_WORLD_SEED_OFFSET;

    pat_move_choice_reference_values(
        game, &move_pat_on, mover_index, reference_pat, world_seed,
        PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_on);
    pat_move_choice_reference_values(
        game, &move_pat_off, mover_index, reference_pat, world_seed,
        PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_off);
    double sum_d_greedy_aware = 0.0;
    for (int world = 0; world < PAT_ROLLOUT_VALUE_NUM_WORLDS; world++) {
      sum_d_greedy_aware += values_pat_off[world] - values_pat_on[world];
    }
    const double position_mean_greedy_aware =
        sum_d_greedy_aware / PAT_ROLLOUT_VALUE_NUM_WORLDS;

    player_set_rollout_disable_pat(player0, true);
    player_set_rollout_disable_pat(player1, true);
    pat_move_choice_reference_values(
        game, &move_pat_on, mover_index, reference_pat, world_seed,
        PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_on);
    pat_move_choice_reference_values(
        game, &move_pat_off, mover_index, reference_pat, world_seed,
        PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_off);
    player_set_rollout_disable_pat(player0, false);
    player_set_rollout_disable_pat(player1, false);
    double sum_d_greedy_neutral = 0.0;
    for (int world = 0; world < PAT_ROLLOUT_VALUE_NUM_WORLDS; world++) {
      sum_d_greedy_neutral += values_pat_off[world] - values_pat_on[world];
    }
    const double position_mean_greedy_neutral =
        sum_d_greedy_neutral / PAT_ROLLOUT_VALUE_NUM_WORLDS;

    struct timespec oracle_start;
    struct timespec oracle_end;
    clock_gettime(CLOCK_MONOTONIC, &oracle_start);
    pat_nested_sim_reference_values(
        nested_config, game, &move_pat_on, mover_index, world_seed,
        PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_on);
    pat_nested_sim_reference_values(
        nested_config, game, &move_pat_off, mover_index, world_seed,
        PAT_ROLLOUT_VALUE_NUM_WORLDS, values_pat_off);
    clock_gettime(CLOCK_MONOTONIC, &oracle_end);
    nested_sim_elapsed_seconds +=
        (double)(oracle_end.tv_sec - oracle_start.tv_sec) +
        (double)(oracle_end.tv_nsec - oracle_start.tv_nsec) / 1.0e9;
    double sum_d = 0.0;
    double sum_d_sq = 0.0;
    for (int world = 0; world < PAT_ROLLOUT_VALUE_NUM_WORLDS; world++) {
      const double d = values_pat_off[world] - values_pat_on[world];
      sum_d += d;
      sum_d_sq += d * d;
    }
    const double position_mean = sum_d / PAT_ROLLOUT_VALUE_NUM_WORLDS;
    const double position_var = (sum_d_sq / PAT_ROLLOUT_VALUE_NUM_WORLDS -
                                 position_mean * position_mean) *
                                ((double)PAT_ROLLOUT_VALUE_NUM_WORLDS /
                                 (PAT_ROLLOUT_VALUE_NUM_WORLDS - 1));
    sum_means += position_mean;
    sum_means_sq += position_mean * position_mean;
    sum_within += position_var;

    char *cgp = game_get_cgp(game, /*write_player_on_turn_first=*/true);
    log_scored_disagreement(
        position_seed, cgp, &move_pat_on, &move_pat_off, board,
        game_get_ld(game), position_mean_greedy_aware,
        position_mean_greedy_neutral, position_mean, position_var);
    free(cgp);

    printf("  disagreement %ld (seed=%llu, elapsed=%.0fs): greedy-aware "
           "%.2f, greedy-neutral %.2f, nested-sim %.2f\n",
           num_disagreements, (unsigned long long)position_seed,
           elapsed_seconds_since(&run_start), position_mean_greedy_aware,
           position_mean_greedy_neutral, position_mean);
    fflush(stdout);
  }

  const double total_elapsed = elapsed_seconds_since(&run_start);
  printf("\n[accumulate] FINAL: %.2fh elapsed, %ld positions considered, "
         "%ld disagreements (%.2f%%), %.1fs nested-sim scoring (%.2fs/"
         "disagreement)\n",
         total_elapsed / 3600.0, num_positions_considered, num_disagreements,
         num_positions_considered > 0 ? 100.0 * (double)num_disagreements /
                                            (double)num_positions_considered
                                      : 0.0,
         nested_sim_elapsed_seconds,
         num_disagreements > 0
             ? nested_sim_elapsed_seconds / (double)num_disagreements
             : 0.0);
  if (num_disagreements > 1) {
    const long n = num_disagreements;
    const double mean = sum_means / (double)n;
    const double var_means = (sum_means_sq / (double)n - mean * mean) *
                             ((double)n / (double)(n - 1));
    const double se = sqrt(var_means / (double)n);
    const double within = sum_within / (double)n;
    const double between = var_means - within / PAT_ROLLOUT_VALUE_NUM_WORLDS;
    printf("[accumulate] FINAL pooled nested-sim-scored paired effect (this "
           "run only): mean=%.4f SE=%.4f 95%%CI=[%.4f, %.4f]\n",
           mean, se, mean - 1.96 * se, mean + 1.96 * se);
    printf("[accumulate] FINAL variance decomposition (this run only): "
           "within-position %.2f, between-position %.2f (R=%d worlds); "
           "shares of Var(mean): between %.1f%%, within %.1f%%\n",
           within, between, PAT_ROLLOUT_VALUE_NUM_WORLDS,
           100.0 * between / var_means,
           100.0 * (within / PAT_ROLLOUT_VALUE_NUM_WORLDS) / var_means);
  }
  printf("[accumulate] scored results appended to %s\n",
         PAT_ROLLOUT_VALUE_SCORED_LOG_PATH);

  move_list_destroy(setup_move_list);
  config_destroy(nested_config);
  config_destroy(config);
}
