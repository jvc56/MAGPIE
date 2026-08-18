#include "play_chooser_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/config_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_timer.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/play_chooser.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static void test_game_timer(void) {
  GameTimer game_timer;
  game_timer_reset(&game_timer, 0.0);
  assert(game_timer_is_untimed(&game_timer));
  assert(!game_timer_is_expired(&game_timer, 0));
  assert(game_timer_get_seconds_remaining(&game_timer, 0) > 1000000.0);

  game_timer_reset(&game_timer, 60.0);
  assert(!game_timer_is_untimed(&game_timer));
  game_timer_start_turn(&game_timer, 0);
  ctime_nap(0.02);
  game_timer_end_turn(&game_timer);
  assert(game_timer_get_seconds_used(&game_timer, 0) > 0.0);
  assert(game_timer_get_seconds_used(&game_timer, 1) == 0.0);
  assert(game_timer_get_seconds_remaining(&game_timer, 0) < 60.0);
  assert(game_timer_get_seconds_remaining(&game_timer, 1) == 60.0);
  assert(!game_timer_is_expired(&game_timer, 0));

  // Starting a new turn ends the previous one.
  game_timer_start_turn(&game_timer, 1);
  ctime_nap(0.02);
  game_timer_start_turn(&game_timer, 0);
  assert(game_timer_get_seconds_used(&game_timer, 1) > 0.0);
  game_timer_end_turn(&game_timer);

  game_timer_reset(&game_timer, 0.01);
  game_timer_start_turn(&game_timer, 0);
  ctime_nap(0.02);
  game_timer_end_turn(&game_timer);
  assert(game_timer_is_expired(&game_timer, 0));
  assert(!game_timer_is_expired(&game_timer, 1));
  assert(game_timer_get_overtime_seconds(&game_timer, 0) > 0.0);
  assert(game_timer_get_overtime_seconds(&game_timer, 1) == 0.0);

  game_timer_reset_for_players(&game_timer, 0.01, 0.0);
  assert(!game_timer_is_untimed(&game_timer));
  assert(!game_timer_player_is_untimed(&game_timer, 0));
  assert(game_timer_player_is_untimed(&game_timer, 1));
  assert(game_timer_get_seconds_remaining(&game_timer, 0) == 0.01);
  assert(isinf(game_timer_get_seconds_remaining(&game_timer, 1)));
}

static void drain_bag(const Game *game) {
  Bag *bag = game_get_bag(game);
  while (!bag_is_empty(bag)) {
    bag_draw_random_letter(bag, 0);
  }
}

// The opponent holds AABDDET, whose only bingo is the triple-triple
// 1H DEADB(E)AT for 185 through the lane's E (AABDDET has no 7-letter
// words and DEADBEAT is its only 8-letter word with E placeable on the
// board, so that one lane span is provably the opponent's only bingo).
// Instead of playing it, the
// opponent tries the phony DEADB(E)TA through the same lane with the
// same tiles. The chooser should challenge the phony off (keeping it
// would let the opponent go out with 185), learn the opponent's full
// rack from the empty bag, and then kill the bingo span with its own
// play.
//
// The block must be informed by the rack knowledge, not just reflexive:
// the chooser's rack is CCH, and the board has an MM pocket at I15 that
// only hooks H, M, or U — letters the opponent does not hold (the V at
// F14 breaks the row-14 runway over the pocket). The chooser's pocket
// play (CH down to the H15 triple word square, making (H)MM for 51) and
// every lane play available to the chooser (HE, EH, CHE, ECH through
// the lane's E) consume the chooser's only H, so blocking forfeits the
// pocket points. A chooser that believes the opponent holds a junk rack
// provably spends its H at the pocket and leaves the bingo alive; only
// the chooser that knows the opponent kept AABDDET sacrifices the
// pocket to block. Because AABDDET has no 7-letter words and no
// 8-letter words with C, H, M, or V, any chooser tile in the lane span
// denies every bingo — including replays through the chooser's own
// tiles.
static void test_challenge_off_blocks_bingo(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 12E2/12J2/12Q2/12V2/15/15/15/15/15/15/15/15/15/5V9/8MM5 "
              "AABDDET/CCH 300/300 0 -lex CSW21;");
  Game *game = config_get_game(config);
  drain_bag(game);
  assert(game_get_player_on_turn_index(game) == 0);

  // Sanity check: the opponent's triple-triple bingo is playable.
  {
    Game *pre_move_game = game_duplicate(game);
    MoveList *move_list = move_list_create(1);
    const Move *opp_best = get_top_equity_move(pre_move_game, move_list);
    assert(equity_to_int(move_get_score(opp_best)) == 185);
    assert(move_get_tiles_played(opp_best) == 7);
    move_list_destroy(move_list);
    game_destroy(pre_move_game);
  }

  // The opponent announces the phony instead.
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms =
      validated_moves_create(game, 0, "1H DEADB.TA", true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_is_phony(vms, 0));
  const Move *phony_move = validated_moves_get_move(vms, 0);

  const PlayChooserStrategy strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
      .fixed_seconds_per_move = 5.0,
      .enable_challenges = true,
      .challenge_decision_seconds = 4.0,
      .num_threads = 2,
      .seed = 42,
  };
  PlayChooser *play_chooser = play_chooser_create(&strategy);

  // The kept phony is an outplay, so the keep branch is exact (game
  // over) without a solve and the challenge branch is settled by the
  // decider's null-window resolve against that exact value; its reported
  // challenge_value is the proven bound rather than an exact spread.
  ChallengeDecision decision;
  play_chooser_decide_challenge(play_chooser, game, phony_move, &decision,
                                error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(decision.move_is_phony);
  assert(decision.should_challenge);
  assert(decision.challenge_value > decision.keep_value);

  // Apply the challenge to the live game.
  game_set_backup_mode(game, BACKUP_MODE_GCG);
  play_move_without_drawing_tiles(phony_move, game);
  play_chooser_challenge_off(game, phony_move);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  assert(game_get_player_on_turn_index(game) == 1);

  // With the bag empty, the challenged-off play reveals the opponent's
  // entire rack.
  const LetterDistribution *ld = game_get_ld(game);
  assert_rack_equals_string(
      ld, player_get_known_rack_from_phonies(game_get_player(game, 0)),
      "AABDDET");

  // Counterfactual: a chooser that does not know the opponent kept
  // AABDDET (here, one that believes they hold inert junk) spends its H
  // at the MM pocket instead of blocking, leaving the bingo alive.
  {
    Game *naive_game = game_duplicate(game);
    Rack junk_rack;
    rack_set_dist_size_and_reset(&junk_rack, ld_get_size(ld));
    rack_set_to_string(ld, &junk_rack, "DLLNNOO");
    rack_copy(player_get_rack(game_get_player(naive_game, 0)), &junk_rack);
    rack_reset(
        player_get_known_rack_from_phonies(game_get_player(naive_game, 0)));

    PlayChooser *naive_play_chooser = play_chooser_create(&strategy);
    Move naive_move;
    play_chooser_choose_move(naive_play_chooser, naive_game, &naive_move,
                             error_stack);
    assert(error_stack_is_empty(error_stack));
    // The naive move is at the pocket, not in the lane.
    assert(move_get_row_start(&naive_move) != 0);

    // Played against the opponent's real rack, the naive move leaves the
    // triple-triple available.
    Game *real_game_copy = game_duplicate(game);
    play_move(&naive_move, real_game_copy, NULL);
    MoveList *naive_move_list = move_list_create(1);
    const Move *opp_best_after_naive =
        get_top_equity_move(real_game_copy, naive_move_list);
    assert(equity_to_int(move_get_score(opp_best_after_naive)) == 185);
    assert(move_get_tiles_played(opp_best_after_naive) == 7);
    move_list_destroy(naive_move_list);
    game_destroy(real_game_copy);
    play_chooser_destroy(naive_play_chooser);
    game_destroy(naive_game);
  }

  // The informed chooser gives up the pocket points to block the
  // triple-triple lane with its only H.
  Move our_move;
  play_chooser_choose_move(play_chooser, game, &our_move, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(move_get_type(&our_move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_row_start(&our_move) == 0);
  play_move(&our_move, game, NULL);

  // The opponent's bingo is gone.
  assert(game_get_player_on_turn_index(game) == 0);
  MoveList *move_list = move_list_create(1);
  const Move *opp_best = get_top_equity_move(game, move_list);
  assert(equity_to_int(move_get_score(opp_best)) < 100);
  assert(move_get_tiles_played(opp_best) < 7);
  move_list_destroy(move_list);

  play_chooser_destroy(play_chooser);
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// The opponent plays the low-scoring phony (M1) EV(L) for 6 points, but
// the E it places at M1 opens a triple-triple lane: the chooser holds
// AINRRST and can play 1H TRAIN(E)RS through it for 131. Challenging the
// phony off would close the lane, so the chooser should keep the phony
// on the board and take the triple-triple.
static void test_keep_phony_for_triple_triple(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/10FOLD1/15/15/15/15/15/15/15/15/15/15/15/15 "
              "DEGUUVW/AINRRST 0/0 0 -lex CSW21;");
  Game *game = config_get_game(config);
  assert(game_get_player_on_turn_index(game) == 0);

  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms =
      validated_moves_create(game, 0, "M1 EV.", true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_is_phony(vms, 0));
  const Move *phony_move = validated_moves_get_move(vms, 0);
  assert(equity_to_int(move_get_score(phony_move)) == 6);

  const PlayChooserStrategy strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .enable_challenges = true,
      .challenge_decision_seconds = 2.0,
      .num_threads = 1,
      .seed = 42,
  };
  PlayChooser *play_chooser = play_chooser_create(&strategy);

  ChallengeDecision decision;
  play_chooser_decide_challenge(play_chooser, game, phony_move, &decision,
                                error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(decision.move_is_phony);
  assert(!decision.should_challenge);
  assert(decision.keep_value > decision.challenge_value);

  // The chooser declines to challenge: the phony stands and the opponent
  // replenishes their rack.
  play_move_without_drawing_tiles(phony_move, game);
  draw_to_full_rack(game, 0);
  assert(game_get_player_on_turn_index(game) == 1);

  // The chooser takes the triple-triple through the kept phony.
  Move our_move;
  play_chooser_choose_move(play_chooser, game, &our_move, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(move_get_type(&our_move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(equity_to_int(move_get_score(&our_move)) == 131);
  assert(move_get_row_start(&our_move) == 0);
  assert(move_get_col_start(&our_move) == 7);
  assert(move_get_tiles_played(&our_move) == 7);

  // A simming chooser budgeting its time from a game timer finds the
  // same triple-triple.
  GameTimer game_timer;
  game_timer_reset(&game_timer, 24.0);
  game_timer_start_turn(&game_timer, 1);
  WinPct *win_pcts =
      win_pct_create(DEFAULT_TEST_DATA_PATH, DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));
  const PlayChooserStrategy sim_strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
      .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .sim_plies = 2,
      .sim_max_candidates = 5,
      .game_timer = &game_timer,
      .win_pcts = win_pcts,
      .num_threads = 2,
      .seed = 42,
  };
  PlayChooser *sim_play_chooser = play_chooser_create(&sim_strategy);
  Move sim_move;
  play_chooser_choose_move(sim_play_chooser, game, &sim_move, error_stack);
  game_timer_end_turn(&game_timer);
  assert(error_stack_is_empty(error_stack));
  assert(equity_to_int(move_get_score(&sim_move)) == 131);
  assert(game_timer_get_seconds_used(&game_timer, 1) > 0.0);
  play_chooser_destroy(sim_play_chooser);
  win_pct_destroy(win_pcts);

  play_chooser_destroy(play_chooser);
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Shared board core for the endgame challenge tests below: a sealed
// V(M)V / Y cluster. The only live square is G4, directly above the M of
// the vertical MY: any tile there forms ?(MY) vertically, and no such
// three-letter word exists, so a play at G4 is always a phony. The Vs at
// F5/H5 seal every other hook (no two-letter word starts or ends with V,
// and ?V?/VMV patterns have no fits for the racks involved).
static const char *const SEALED_CLUSTER_ROWS =
    "15/15/15/15/5VMV7/6Y8/15/15/15/15/15/15/15/15/15";

static const PlayChooserStrategy ENDGAME_CHALLENGE_STRATEGY = {
    .pre_endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
    .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
    .fixed_seconds_per_move = 5.0,
    .enable_challenges = true,
    .challenge_decision_seconds = 4.0,
    .num_threads = 2,
    .seed = 42,
};

// Endgame keep: the chooser holds TRANQ, and the only way to ever play
// the Q is through the opponent's phony. The opponent plays G4 I(MY) —
// IMY is not a word — but the I turns the dead G4 column into R(IMY),
// the chooser's unique TRANQ placement (3F TRANQ with the R making
// RIMY). Rack knowledge is free here (the bag is empty), so the only
// reason to decline the challenge is the board: challenging the I off
// leaves the chooser provably stuck with the Q (and everything else)
// while the opponent eats a smaller rack penalty. The chooser keeps the
// phony and goes out through it.
static void test_endgame_keep_phony_for_only_q_play(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  char *cgp_command = get_formatted_string(
      "cgp %s IFFGWWY/TRANQ 300/300 0 -lex CSW21;", SEALED_CLUSTER_ROWS);
  load_and_exec_config_or_die(config, cgp_command);
  free(cgp_command);
  Game *game = config_get_game(config);
  drain_bag(game);
  assert(game_get_player_on_turn_index(game) == 0);

  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms =
      validated_moves_create(game, 0, "G4 I..", true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_is_phony(vms, 0));
  const Move *phony_move = validated_moves_get_move(vms, 0);
  assert(equity_to_int(move_get_score(phony_move)) == 8);

  PlayChooser *play_chooser = play_chooser_create(&ENDGAME_CHALLENGE_STRATEGY);
  ChallengeDecision decision;
  play_chooser_decide_challenge(play_chooser, game, phony_move, &decision,
                                error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(decision.move_is_phony);
  assert(!decision.should_challenge);
  assert(decision.keep_value > decision.challenge_value);

  // Counterfactual: had the chooser challenged the I off, its best play
  // would be a small dump (ARMY or VANT-class plays through the cluster)
  // — it can never go out, because the Q has no spot without the
  // opponent's I.
  {
    Game *challenged_game = game_duplicate(game);
    game_set_backup_mode(challenged_game, BACKUP_MODE_GCG);
    play_move_without_drawing_tiles(phony_move, challenged_game);
    play_chooser_challenge_off(challenged_game, phony_move);
    game_set_backup_mode(challenged_game, BACKUP_MODE_OFF);
    assert(game_get_player_on_turn_index(challenged_game) == 1);
    MoveList *move_list = move_list_create(1);
    const Move *our_best = get_top_equity_move(challenged_game, move_list);
    assert(move_get_tiles_played(our_best) < 5);
    move_list_destroy(move_list);
    game_destroy(challenged_game);
  }

  // The phony stands; the chooser goes out through it: 3F TRANQ, playing
  // all five tiles with the R making R(IMY).
  play_move_without_drawing_tiles(phony_move, game);
  assert(game_get_player_on_turn_index(game) == 1);
  Move our_move;
  play_chooser_choose_move(play_chooser, game, &our_move, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(move_get_type(&our_move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_played(&our_move) == 5);
  assert(move_get_row_start(&our_move) == 2);
  assert(move_get_col_start(&our_move) == 5);

  play_chooser_destroy(play_chooser);
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Endgame challenge, the typical case: the opponent's phony G4 Z(MY)
// scores 17 and dumps their otherwise unplayable Z. Challenging it off
// denies the points, strands the Z (and the rest of ZFFWW — none of it
// plays anywhere), and lets the chooser go out with H5 (V)AT, collecting
// double the opponent's full 26-point rack instead of just their
// post-phony leftovers.
static void test_endgame_challenge_off_phony(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  char *cgp_command = get_formatted_string(
      "cgp %s ZFFWW/AT 300/300 0 -lex CSW21;", SEALED_CLUSTER_ROWS);
  load_and_exec_config_or_die(config, cgp_command);
  free(cgp_command);
  Game *game = config_get_game(config);
  drain_bag(game);
  assert(game_get_player_on_turn_index(game) == 0);

  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms =
      validated_moves_create(game, 0, "G4 Z..", true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_is_phony(vms, 0));
  const Move *phony_move = validated_moves_get_move(vms, 0);
  assert(equity_to_int(move_get_score(phony_move)) == 17);

  PlayChooser *play_chooser = play_chooser_create(&ENDGAME_CHALLENGE_STRATEGY);
  ChallengeDecision decision;
  play_chooser_decide_challenge(play_chooser, game, phony_move, &decision,
                                error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(decision.move_is_phony);
  assert(decision.should_challenge);
  assert(decision.challenge_value > decision.keep_value);

  // Apply the challenge; the chooser goes out with (V)AT against the
  // opponent's stranded 26-point rack.
  game_set_backup_mode(game, BACKUP_MODE_GCG);
  play_move_without_drawing_tiles(phony_move, game);
  play_chooser_challenge_off(game, phony_move);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  assert(game_get_player_on_turn_index(game) == 1);
  Move our_move;
  play_chooser_choose_move(play_chooser, game, &our_move, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(move_get_type(&our_move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_played(&our_move) == 2);

  play_chooser_destroy(play_chooser);
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Endgame keep, to deny a better replay: same phony G4 Z(MY) for 17, but
// this board also has an A at H2 under the H1 triple word square. With
// the Z back in hand the opponent's ZC plays H1 C(A)Z for 42 — and goes
// out, doubling the chooser's stuck QC on top. The chooser's QC is dead
// wood (no Q or C plays anywhere) so it cannot occupy H1 first.
// Challenging the phony off hands the tiles back for that sequence;
// keeping it entombs the Z in the 17-point phony and leaves the lone C
// stranded. The chooser eats the 17.
static void test_endgame_keep_phony_to_deny_better_replay(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 15/7A7/15/15/5VMV7/6Y8/15/15/15/15/15/15/15/15/15 "
              "ZC/QC 300/300 0 -lex CSW21;");
  const Game *game = config_get_game(config);
  drain_bag(game);
  assert(game_get_player_on_turn_index(game) == 0);

  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms =
      validated_moves_create(game, 0, "G4 Z..", true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_is_phony(vms, 0));
  const Move *phony_move = validated_moves_get_move(vms, 0);
  assert(equity_to_int(move_get_score(phony_move)) == 17);

  PlayChooser *play_chooser = play_chooser_create(&ENDGAME_CHALLENGE_STRATEGY);
  ChallengeDecision decision;
  play_chooser_decide_challenge(play_chooser, game, phony_move, &decision,
                                error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(decision.move_is_phony);
  assert(!decision.should_challenge);
  assert(decision.keep_value > decision.challenge_value);

  // Sanity check the threat the chooser is avoiding: if the phony were
  // challenged off, the opponent's best reply would be the 42-point
  // H1 C(A)Z outplay.
  {
    Game *challenged_game = game_duplicate(game);
    game_set_backup_mode(challenged_game, BACKUP_MODE_GCG);
    play_move_without_drawing_tiles(phony_move, challenged_game);
    play_chooser_challenge_off(challenged_game, phony_move);
    game_set_backup_mode(challenged_game, BACKUP_MODE_OFF);
    game_set_player_on_turn_index(challenged_game, 0);
    MoveList *move_list = move_list_create(1);
    const Move *opp_best = get_top_equity_move(challenged_game, move_list);
    assert(equity_to_int(move_get_score(opp_best)) == 42);
    assert(move_get_tiles_played(opp_best) == 2);
    move_list_destroy(move_list);
    game_destroy(challenged_game);
  }

  play_chooser_destroy(play_chooser);
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Solve game to the full requested plies with a single thread, optionally
// with a fixed [window_alpha, window_beta] search window, and return the
// reported value. The scores in the test position are equal, so the
// reported value (a spread delta) and the window units (final spread)
// coincide.
static int32_t solve_endgame_with_window(Game *game, EndgameCtx **endgame_ctx,
                                         EndgameResults *endgame_results,
                                         bool use_window, int32_t window_alpha,
                                         int32_t window_beta) {
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = thread_control;
  endgame_args.game = game;
  endgame_args.tt_fraction_of_mem = 0.1;
  endgame_args.plies = MAX_VARIANT_LENGTH;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 1;
  endgame_args.use_heuristics = true;
  endgame_args.num_top_moves = 1;
  endgame_args.seed = 42;
  endgame_args.use_initial_window = use_window;
  endgame_args.initial_alpha = window_alpha;
  endgame_args.initial_beta = window_beta;
  ErrorStack *error_stack = error_stack_create();
  endgame_solve(endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  thread_control_destroy(thread_control);
  assert(endgame_results_get_depth(endgame_results, ENDGAME_RESULT_BEST) ==
         MAX_VARIANT_LENGTH);
  return (int32_t)endgame_results_get_value(endgame_results,
                                            ENDGAME_RESULT_BEST);
}

// Verify the fixed-window solve semantics that the challenge decider's
// null-window resolve relies on: against the exact value V of a
// position, a [V, V+1] window must fail low (the position does not beat
// V) and a [V-1, V] window must fail high (it does beat V-1).
static void test_endgame_initial_window(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 15/7A7/15/15/5VMV7/6Y8/15/15/15/15/15/15/15/15/15 "
              "ZC/QC 300/300 0 -lex CSW21;");
  Game *game = config_get_game(config);
  drain_bag(game);

  EndgameCtx *endgame_ctx = endgame_ctx_create();
  EndgameResults *endgame_results = endgame_results_create();
  const int32_t exact_value = solve_endgame_with_window(
      game, &endgame_ctx, endgame_results, false, 0, 0);
  const int32_t at_value = solve_endgame_with_window(
      game, &endgame_ctx, endgame_results, true, exact_value, exact_value + 1);
  assert(at_value <= exact_value);
  const int32_t below_value = solve_endgame_with_window(
      game, &endgame_ctx, endgame_results, true, exact_value - 1, exact_value);
  assert(below_value >= exact_value);

  endgame_results_destroy(endgame_results);
  endgame_ctx_destroy(endgame_ctx);
  config_destroy(config);
}

// Pre-endgame challenge decisions driven by the PEG solver. In a real
// 4-in-bag CSW24 position the chooser (DEIINOS, ahead by 36) faces a
// phony hook on BEATY announced by the opponent (ACEINOP). With
// pre_endgame_eval = PLAY_CHOOSER_EVAL_PEG the chooser values the keep
// branch (phony stands, opponent draws) and the challenge branch (phony
// off, opponent loses the turn) by the score+win utility. A detected phony
// is challenged by default; the chooser keeps it only when keeping is
// strictly better. A one-tile hook leaves the bag non-empty in both
// branches, so this is a genuine pre-endgame-vs-pre-endgame decision solved
// by PEG, not a fall-through to the endgame.
//
// The branch valuation uses PEG's greedy seed (stage 0): it ranks the full
// candidate field over every bag-draw scenario with a deterministic playout
// and no open-ended deep endgame solves, so the value is bounded and
// machine-independent once the seed completes (the generous decision budget
// guarantees that) rather than depending on how many refinement stages a
// wall-clock budget happened to finish.
//
// The branch value is the simmer's score+win utility (sim_utility_blend):
// win% blended with a sigmoid of the mean spread, so margin matters and
// equal-win% branches are separated by score. With utility_w_spread > 0 a
// certain-win branch is discounted below 1.0 by its finite margin (see the
// CBEATY keep-branch assertion below), which is exactly what pure win% could
// not express.
//
// The same position yields opposite verdicts depending on the phony:
//   - OBEATY: challenging it off (denying the opponent the points and the
//     turn) raises the chooser's utility, so the chooser challenges.
//   - CBEATY: keeping it locks in the opponent's weak play and the chooser
//     wins nearly for certain, whereas challenging would hand the opponent a
//     do-over with its strong ACEINOP rack, so the chooser declines.
static void test_peg_challenge_decision_case(
    const PlayChooserStrategy *strategy, const Game *game,
    const char *phony_str, bool expect_challenge, ChallengeDecision *decision) {
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms =
      validated_moves_create(game, 0, phony_str, true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_is_phony(vms, 0));
  const Move *phony = validated_moves_get_move(vms, 0);

  PlayChooser *play_chooser = play_chooser_create(strategy);
  play_chooser_decide_challenge(play_chooser, game, phony, decision,
                                error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(decision->move_is_phony);
  // PEG branch values are score+win utilities in [0, 1].
  assert(decision->keep_value >= 0.0 && decision->keep_value <= 1.0);
  assert(decision->challenge_value >= 0.0 && decision->challenge_value <= 1.0);
  // A detected phony is challenged by default: the chooser keeps it only with
  // two valid branch evaluations where keeping is strictly better. For these
  // cases (both branches valid, or the degenerate both-invalid 0/0) the verdict
  // therefore matches challenge_value >= keep_value.
  assert(decision->should_challenge ==
         (decision->challenge_value >= decision->keep_value));
  assert(decision->should_challenge == expect_challenge);
  if (expect_challenge) {
    assert(decision->challenge_value >= decision->keep_value);
  } else {
    assert(decision->keep_value > decision->challenge_value);
  }

  play_chooser_destroy(play_chooser);
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
}

static void test_peg_challenge_decision(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  load_and_exec_config_or_die(
      config,
      "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
      "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/"
      "12NaM/12ATE/13ST/14H ACEINOP/DEIINOS 361/397 0 -lex CSW24;");
  const Game *game = config_get_game(config);
  // Pre-endgame regime: the opponent (ACEINOP) is on turn with four tiles
  // in the bag, so play_chooser keeps the PEG evaluation (it only falls
  // back to SIM/STATIC above PEG_MAX_BAG).
  assert(game_get_player_on_turn_index(game) == 0);
  assert(bag_get_letters(game_get_bag(game)) == 4);

  // The budget is a generous ceiling, not a target: the greedy seed completes
  // the full-field enumeration well under it (no deep endgame solves to run
  // long), so the utilities reach their bounded, deterministic values and the
  // verdicts do not depend on machine speed. Several threads just reach that
  // completion sooner; the completed utility is thread-count independent.
  // utility_w_spread > 0 turns on the sigmoid-of-spread term.
  const PlayChooserStrategy strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_PEG,
      .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
      .fixed_seconds_per_move = 40.0,
      .enable_challenges = true,
      .challenge_decision_seconds = 40.0,
      .num_threads = 8,
      .utility_w_winpct = 1.0,
      .utility_w_spread = 1.0,
      .utility_spread_scale = 100.0,
      .seed = 42,
  };

  ChallengeDecision o_decision;
  test_peg_challenge_decision_case(&strategy, game, "2A O.....",
                                   /*expect_challenge=*/true, &o_decision);

  ChallengeDecision c_decision;
  test_peg_challenge_decision_case(&strategy, game, "2A C.....",
                                   /*expect_challenge=*/false, &c_decision);
  // The kept CBEATY is a near-certain win for the chooser, but the score+win
  // utility discounts it below 1.0 for its finite margin -- pure win% would
  // report exactly 1.0 and lose that information. This is the spread tiebreak
  // in action, and it stays above the challenge branch's utility so the
  // verdict is still "don't challenge".
  assert(c_decision.keep_value < 1.0);
  assert(c_decision.keep_value > c_decision.challenge_value);

  // Severe-budget degenerate case: the deadline is blown before the greedy
  // seed finishes any candidate, so both arms fall back to 0.0. A detected
  // phony must still be taken off -- a 0/0 no-information tie challenges rather
  // than gifting the opponent an unchallenged invalid play. (CBEATY, which the
  // full-budget analysis above chose to keep, is challenged here: with no time
  // to find the strategic reason to keep it, the default wins.)
  PlayChooserStrategy severe = strategy;
  severe.fixed_seconds_per_move = 1e-6;
  severe.challenge_decision_seconds = 1e-6;
  ChallengeDecision degenerate;
  test_peg_challenge_decision_case(&severe, game, "2A C.....",
                                   /*expect_challenge=*/true, &degenerate);
  assert(degenerate.keep_value == 0.0 && degenerate.challenge_value == 0.0);
  assert(degenerate.should_challenge);

  config_destroy(config);
}

// One stage-combination challenge case: the keep branch (bag B-T, the opponent
// draws its T replacements) and the challenge branch (bag B, no draw) are each
// valued by the method for their own stage and projected onto the shared [0, 1]
// utility. Verifies the per-branch routing runs end to end and both branch
// values land on that comparable scale. game_before_move keeps ownership.
static void assert_stage_combo_case(const PlayChooserStrategy *strategy,
                                    const Game *game_before_move,
                                    const char *phony_str, bool utility_scale) {
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(game_before_move, 0, phony_str,
                                               true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_is_phony(vms, 0));
  const Move *phony = validated_moves_get_move(vms, 0);

  PlayChooser *play_chooser = play_chooser_create(strategy);
  ChallengeDecision decision;
  play_chooser_decide_challenge(play_chooser, game_before_move, phony,
                                &decision, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(decision.move_is_phony);
  assert(isfinite(decision.keep_value) && isfinite(decision.challenge_value));
  // A sim/PEG/endgame-valued branch reports the score+win utility in [0, 1]; a
  // both-endgame decision keeps its exact-spread scale (its higher-spread
  // verdict equals the higher-utility one, so the branches stay comparable).
  if (utility_scale) {
    assert(decision.keep_value >= 0.0 && decision.keep_value <= 1.0);
    assert(decision.challenge_value >= 0.0 && decision.challenge_value <= 1.0);
  }

  play_chooser_destroy(play_chooser);
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
}

// Exercises all six keep/challenge game-stage combinations of a challenge
// decision (challenge/keep): SIM/SIM, SIM/PEG, SIM/EG, PEG/PEG, PEG/EG, EG/EG.
// The challenge branch keeps the pre-move bag; the keep branch draws the
// phony's tiles, so it is always at the same or a later stage. Each branch must
// be valued by the method for its own bag size, all on the shared utility
// scale.
static void test_challenge_stage_combinations(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(DEFAULT_TEST_DATA_PATH, DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const PlayChooserStrategy strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_PEG,
      .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
      .enable_challenges = true,
      .challenge_decision_seconds = 2.0,
      .num_threads = 2,
      .win_pcts = win_pcts,
      .utility_w_winpct = 1.0,
      .utility_w_spread = 1.0,
      .utility_spread_scale = 100.0,
      .seed = 42,
  };

  // SIM/SIM: the SEALED_CLUSTER board leaves a full (~82-tile) bag, so both
  // branches (keep bag ~81, challenge bag ~82) are sims. "G4 I.." is a 1-tile
  // phony. A real bag keeps the game state consistent for the sim's rollouts.
  {
    char *cgp = get_formatted_string(
        "cgp %s IFFGWWY/AEILNOT 300/300 0 -lex CSW21;", SEALED_CLUSTER_ROWS);
    load_and_exec_config_or_die(config, cgp);
    free(cgp);
    const Game *game = config_get_game(config);
    assert(bag_get_letters(game_get_bag(game)) > PEG_MAX_BAG + 1);
    assert(game_get_player_on_turn_index(game) == 0);
    assert_stage_combo_case(&strategy, game, "G4 I..", /*utility_scale=*/true);
  }

  // PEG/PEG: a real 4-in-the-bag position with a 1-tile hook phony -- the keep
  // branch draws one (bag 3, PEG) and the challenge branch keeps its bag 4
  // (PEG). (Same fixture as test_peg_challenge_decision.)
  {
    load_and_exec_config_or_die(
        config,
        "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
        "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/"
        "12NaM/12ATE/13ST/14H ACEINOP/DEIINOS 361/397 0 -lex CSW24;");
    const Game *game = config_get_game(config);
    assert(bag_get_letters(game_get_bag(game)) == 4);
    assert_stage_combo_case(&strategy, game, "2A O.....",
                            /*utility_scale=*/true);
  }

  // SIM/PEG: the same real fixture with one fewer tile on the mover's rack, so
  // the bag is 5: the keep branch draws one (bag 4, PEG) and the challenge
  // branch keeps its bag 5 (sim).
  {
    load_and_exec_config_or_die(
        config,
        "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
        "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/"
        "12NaM/12ATE/13ST/14H ACEINO/DEIINOS 361/397 0 -lex CSW24;");
    const Game *game = config_get_game(config);
    assert(bag_get_letters(game_get_bag(game)) == 5);
    assert_stage_combo_case(&strategy, game, "2A O.....",
                            /*utility_scale=*/true);
  }

  // SIM/EG: same real bag-5 fixture, but the phony 1H (W)CANOE* plays five
  // tiles that hook the row-1 W into the non-word WCANOE (no cross-words). The
  // keep branch draws five and empties the bag (endgame); the challenge branch
  // keeps its bag 5 (sim). Exercises the single-branch endgame -> utility
  // projection.
  {
    load_and_exec_config_or_die(
        config,
        "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
        "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/"
        "12NaM/12ATE/13ST/14H ACEINO/DEIINOS 361/397 0 -lex CSW24;");
    const Game *game = config_get_game(config);
    assert(bag_get_letters(game_get_bag(game)) == 5);
    assert_stage_combo_case(&strategy, game, "1H .CANOE",
                            /*utility_scale=*/true);
  }

  // PEG/EG: the real bag-4 fixture with the four-tile phony 1H (W)CANO* (hooks
  // the row-1 W into the non-word WCANO). The keep branch draws four and
  // empties the bag (endgame); the challenge branch keeps its bag 4 (PEG).
  {
    load_and_exec_config_or_die(
        config,
        "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
        "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/"
        "12NaM/12ATE/13ST/14H ACEINOP/DEIINOS 361/397 0 -lex CSW24;");
    const Game *game = config_get_game(config);
    assert(bag_get_letters(game_get_bag(game)) == 4);
    assert_stage_combo_case(&strategy, game, "1H .CANO",
                            /*utility_scale=*/true);
  }

  // EG/EG: drain the bag (both branches at bag 0 -- consistent, since an
  // endgame draws no tiles). Endgame eval on both.
  {
    char *cgp = get_formatted_string(
        "cgp %s IFFGWWY/AEILNOT 300/300 0 -lex CSW21;", SEALED_CLUSTER_ROWS);
    load_and_exec_config_or_die(config, cgp);
    free(cgp);
    const Game *game = config_get_game(config);
    drain_bag(game);
    assert(bag_get_letters(game_get_bag(game)) == 0);
    assert_stage_combo_case(&strategy, game, "G4 I..", /*utility_scale=*/false);
  }

  // EG/EG single-threaded: the decider solves the two branches sequentially
  // rather than spawning a second solver thread (thread-budget respected).
  {
    char *cgp = get_formatted_string(
        "cgp %s IFFGWWY/AEILNOT 300/300 0 -lex CSW21;", SEALED_CLUSTER_ROWS);
    load_and_exec_config_or_die(config, cgp);
    free(cgp);
    const Game *game = config_get_game(config);
    drain_bag(game);
    PlayChooserStrategy single_thread_strategy = strategy;
    single_thread_strategy.num_threads = 1;
    assert_stage_combo_case(&single_thread_strategy, game, "G4 I..",
                            /*utility_scale=*/false);
  }

  // SIM/SIM with a zero spread weight (pure win%): the simmer skips the
  // utility_stat on this path, so the SIM branch must read the win% directly.
  // The SEALED_CLUSTER position is roughly even, so both branch win%s sit well
  // inside (0, 1) -- a strictly-positive check catches a regression to the
  // empty-utility_stat 0.0 sentinel, which the [0, 1] range check would not.
  {
    PlayChooserStrategy pure_winpct_strategy = strategy;
    pure_winpct_strategy.utility_w_spread = 0.0;
    char *cgp = get_formatted_string(
        "cgp %s IFFGWWY/AEILNOT 300/300 0 -lex CSW21;", SEALED_CLUSTER_ROWS);
    load_and_exec_config_or_die(config, cgp);
    free(cgp);
    const Game *game = config_get_game(config);
    ValidatedMoves *vms =
        validated_moves_create(game, 0, "G4 I..", true, true, error_stack);
    assert(error_stack_is_empty(error_stack));
    const Move *phony = validated_moves_get_move(vms, 0);
    PlayChooser *play_chooser = play_chooser_create(&pure_winpct_strategy);
    ChallengeDecision decision;
    play_chooser_decide_challenge(play_chooser, game, phony, &decision,
                                  error_stack);
    assert(error_stack_is_empty(error_stack));
    assert(decision.keep_value > 0.0 && decision.keep_value < 1.0);
    assert(decision.challenge_value > 0.0 && decision.challenge_value < 1.0);
    play_chooser_destroy(play_chooser);
    validated_moves_destroy(vms);
  }

  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_play_chooser(void) {
  test_game_timer();
  test_keep_phony_for_triple_triple();
  test_challenge_off_blocks_bingo();
  test_endgame_keep_phony_for_only_q_play();
  test_endgame_challenge_off_phony();
  test_endgame_keep_phony_to_deny_better_replay();
  test_endgame_initial_window();
  test_peg_challenge_decision();
  test_challenge_stage_combinations();
}
