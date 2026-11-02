# Inference parity fixes and standalone softmax inference: plan

Source: review of `main` at `57c8f810` (`v0.1.4`), open PR `#931`
(`origin/pegadjust-table` at `f2cf18d1`), and the prior inference and rollout
branches (`origin/archive/2026-10-04/claude/inference-scorer` at `91381856`,
`origin/claude/value-net-pat` at `02d0a3af` / `204a1300` / `dc070105`,
`origin/peg-inference` at `8dccbd5c`, and `origin/claude/peg-nets-main`).

Companion roadmap documents:
- `docs/fast-inference-and-movegen-plan.md` — high-speed inference
  (`census_play_scores`, RIT exchanges, batched subtrees, `#839`) and move
  generation / memory footprint optimizations.
- `docs/lategame-search-and-solver-plan.md` — late-game racing sim (`bag` 5–8),
  `PlayChooser` upgrades, endgame/PEG scaling (`#698`, tail lending), and the
  lexicon strategy pipeline.

---

## Overview and PR split

Keep correctness parity separate from the probabilistic posterior change
(`AGENTS.md`, "Keep PRs focused" and "Keep performance work and quality work
separate"):

| PR | Scope | Behavior impact | Key files |
|---|---|---|---|
| **PR 1** | Fix live position/equity parity bugs in `inference.c` on `main` (and document the companion 3-line patch for `#931`) | Bugfix: puts `E_obs` (`target_equity_cutoff - equity_margin`) and `E_top` (`move_get_equity(top_move)`) on the exact same bag count and static-equity formula | `src/ent/inference_args.h`, `src/impl/inference.c`, `src/impl/autoplay.c`, `test/infer_test.c` |
| **PR 2** | Split `MoveGen`'s lower WMP anchor floor from `stop_on_threshold`, add continuous weights to `AliasMethod` / `InferenceResults` / `LeaveRackList` (zero cost when off), and add standalone static/PAT softmax inference | Quality feature (flag-gated; byte-identical and zero-cost when off) | `src/impl/wmp_move_gen.h`, `src/impl/move_gen_base.h`, `src/impl/move_gen.c`, `src/impl/gameplay.c`, `src/ent/alias_method.h`, `src/ent/inference_results.{h,c}`, `src/ent/leave_rack.{h,c}`, `src/impl/inference.c`, `src/impl/config.c`, `test/infer_test.c`, `test/infer_cmp_test.c` |

PR 1 is both an immediate bugfix on `main` and a hard prerequisite for PR 2:
softmax weights depend continuously on the equity regret
$\Delta E(L) = E_{\text{top}}(L) - E_{\text{obs}}(L)$, so any term present in
`MoveGen` (`E_top`) and missing from `evaluate_possible_leave` (`E_obs`) — or
vice versa — corrupts $\Delta E(L)$ or makes it negative on legal plays.

---

## Part 1 (PR 1): Static inference position & equity parity fixes

### 1.1 Answer on the WMP anchor-cutoff question (not live on `main`)

`MoveGen.target_equity_cutoff` (`src/impl/move_gen_base.h:189`) currently serves
two roles:
1. **Upper early-exit threshold** when `gen->stop_on_threshold` is true
   (`src/impl/move_gen_base.h:685-688`, `src/impl/move_gen.c:717-720`): stop as
   soon as `move_equity_or_score > gen->target_equity_cutoff`.
2. **Lower WMP subanchor prune floor** in `wmp_move_gen_add_anchors`
   (`src/impl/wmp_move_gen.h:1022-1027`): drop any subanchor with
   `anchor->highest_possible_equity < inference_cutoff_equity`.

On `main`, **both callers are sound**:
- `evaluate_possible_leave` (`src/impl/inference.c:230-236`) only tests the
  boolean predicate `target_equity_cutoff >= move_get_equity(top_move)` (whether
  *any* move strictly exceeds `target_equity_cutoff`). Dropping subanchors with
  upper bound `< target_equity_cutoff` can never discard a move strictly
  `> target_equity_cutoff`; even if all subanchors are dropped and `top_move`
  falls back to a pass (`EQUITY_PASS_VALUE`),
  `target_equity_cutoff >= EQUITY_PASS_VALUE` still holds.
- `BestMoveCarry` (`gen_seed_carried_best_move`, `src/impl/move_gen.c:4420-4426`)
  runs with `stop_on_threshold == false` and sets `target_equity_cutoff` to the
  equity of an already-seeded legal move on that rack.

This conflation is therefore **not** a live bug on `main`; it becomes active as
soon as softmax reads `move_get_equity(top_move)` below the early-exit cutoff
(addressed in Part 2 below). Auditing every caller of that path, however,
uncovered **three live parity bugs** (two on `main`, one in open PR `#931`).

---

### 1.2 Bug 1A (live on `main`): Phantom legal exchanges in pre-endgame inference (`1 <= bag <= 6`)

#### Root cause
`gen_look_up_leaves_and_record_exchanges` (`src/impl/move_gen.c:3429-3432`) and
the WMP subrack cache gate (`src/impl/move_gen.c:4526-4529`) check exchange
legality assuming the player on turn holds a full rack already drawn from
`game->bag`, while the opponent's unseen tiles remain in `game->bag`:

```c
// Assumes the player has drawn a full rack but not the opponent.
const bool add_exchange = gen->number_of_tiles_in_bag +
                              rack_get_total_letters(&gen->opponent_rack) >=
                          (RACK_SIZE * 2);
```

In `complete_inference_setup` (`src/impl/inference.c:307-363`):
1. Both players' racks are returned to `game_get_bag(inference->game)` (leaving
   all $T_{\text{off}}$ off-board tiles in `game->bag`, where for a pre-move
   bag of $B$ tiles and full 7-tile racks, $T_{\text{off}} = B + 14$).
2. `temp_target_rack = target_played_tiles U target_known_rack` ($p + k_t$
   tiles) is drawn into `target_index`'s rack.
3. `nontarget_known_rack` ($K_{\text{obs}} \in [0, 7]$ tiles) is drawn into
   `1 - target_index`'s rack (`gen->opponent_rack`).
4. The remaining $T_{\text{off}} - (p + k_t) - K_{\text{obs}}$ tiles in
   `game_get_bag(inference->game)` are copied into `inference->bag_as_rack`.
5. During recursion (`infer_worker` / `increment_letter_for_inference`,
   `src/impl/inference.c:293-298, 506-509`), `tiles_to_infer = RACK_SIZE - (p + k_t)`
   candidate leave tiles are added to `inference->current_target_rack` and
   removed from `inference->bag_as_rack`, **without removing `tiles_to_infer`
   tiles from `game_get_bag(inference->game)`**.

Consequently, `gen_load_position` (`src/impl/move_gen.c:3318`) reads:
$$\text{gen->number\_of\_tiles\_in\_bag} = T_{\text{off}} - (p + k_t) - K_{\text{obs}} = (B + 7 - K_{\text{obs}}) + \text{tiles\_to\_infer}$$
and `add_exchange` evaluates:
$$\text{number\_of\_tiles\_in\_bag} + K_{\text{obs}} = B + 7 + \text{tiles\_to\_infer} \ge 14 \iff B + \text{tiles\_to\_infer} \ge 7.$$

Whenever the true pre-move bag satisfies $1 \le B \le 6$ and
$B + \text{tiles\_to\_infer} \ge 7$ (for example, $B = 5$ and the opponent plays
3 tiles so $\text{tiles\_to\_infer} = 4 \implies 5 + 4 = 9 \ge 7$, or $B = 2$
and the opponent plays 2 tiles so $2 + 5 = 7 \ge 7$), **exchanging was illegal
in the real game, yet `MoveGen` generates exchanges** on the candidate rack.
Whenever exchanging a clunky rack (e.g. `UUUVW` or `QVVWW`) has static equity
exceeding the opponent's forced low-scoring board play by `> equity_margin`,
`evaluate_possible_leave` falsely rejects the leave — frequently rejecting the
opponent's **actual leave** and returning 0 valid leaves.

#### Zero-overhead fix & proof for all $K_{\text{obs}} \in [0, 7]$
Verified across `src/`:
- `MoveGen` never reads tile identities from `game_get_bag(game)` — its sole
  access is `bag_get_letters(game_get_bag(game))` at `src/impl/move_gen.c:3318`.
- `PAT` (`src/ent/pat_eval.c:413`, if later enabled in inference) computes
  unseen tile identities from `ld - board - player_rack`, never from `Bag`.
- `get_best_exchange_equity` (`src/impl/inference.c:204`) also calls
  `bag_get_letters(game_get_bag(game))`; previously it saw `B + 7` instead of
  `B`.

Therefore, in `complete_inference_setup` (`src/impl/inference.c:355-363`),
**after** `inference->bag_as_rack` has been populated from `bag`, draw
`bag_tiles_to_remove = min(tiles_to_infer, bag_get_letters(bag))` tiles out of
`Bag *bag = game_get_bag(inference->game)` (using `bag_draw_random_letter(bag, args->target_index)`,
which is $O(1)$ pointer decrement/increment with no PRNG call in `src/ent/bag.c:158-171`):

```c
  Bag *bag = game_get_bag(inference->game);
  int bag_letters_array[MAX_ALPHABET_SIZE];
  memset(bag_letters_array, 0, sizeof(bag_letters_array));
  bag_increment_unseen_count(bag, bag_letters_array);
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    rack_add_letters(inference->bag_as_rack, ml, bag_letters_array[ml]);
  }

  // MoveGen reads only the count of tiles in game->bag (not their identities)
  // and assumes the target's full rack has already been drawn out of it.
  // Remove the tiles_to_infer slots that recursion will place onto
  // current_target_rack so MoveGen sees the true pre-move bag count.
  const int tiles_to_infer =
      RACK_SIZE - rack_get_total_letters(inference->current_target_rack);
  const int current_bag_count = bag_get_letters(bag);
  const int bag_tiles_to_remove =
      tiles_to_infer < current_bag_count ? tiles_to_infer : current_bag_count;
  for (int draw_idx = 0; draw_idx < bag_tiles_to_remove; draw_idx++) {
    bag_draw_random_letter(bag, args->target_index);
  }
```

**Check across all observer knowledge regimes ($T_{\text{off}} \ge \text{RACK\_SIZE} + K_{\text{obs}}$):**
- Before removing `tiles_to_infer`: `bag` has
  $T_{\text{off}} - (p + k_t) - K_{\text{obs}}$ tiles.
- Since the target held a full rack when $B > 0$,
  $T_{\text{off}} - K_{\text{obs}} \ge \text{RACK\_SIZE}$, so
  $T_{\text{off}} - (p + k_t) - K_{\text{obs}} \ge \text{RACK\_SIZE} - (p + k_t) = \text{tiles\_to\_infer}$.
- After removing `tiles_to_infer`:
  $$\text{bag\_get\_letters(bag)} = T_{\text{off}} - \text{RACK\_SIZE} - K_{\text{obs}}.$$
- In `MoveGen`, `add_exchange` tests:
  $$(T_{\text{off}} - \text{RACK\_SIZE} - K_{\text{obs}}) + K_{\text{obs}} = T_{\text{off}} - \text{RACK\_SIZE} \ge 2 \cdot \text{RACK\_SIZE} \iff T_{\text{off}} \ge 3 \cdot \text{RACK\_SIZE}.$$
  This is **invariant to $K_{\text{obs}}$** (works identically whether the
  observer specifies 7 known tiles, 4 known tiles as in `infer_test.c:608`, or 0
  known tiles in spectator CLI `infer`) and holds **if and only if the true
  pre-move bag had $B \ge 7$ tiles**.
- When $K_{\text{obs}} = 7$ (the standard simulation / autoplay / `analyze`
  case), `bag_get_letters(bag)` is the exact pre-move bag $B$, so `peg_adjust`
  and `B == 0` endgame evaluation also see the exact $B$.

#### Critical `B == 0` endgame check (`bag_get_letters(bag) == 0`)
When $B = 0$ and $K_{\text{obs}}$ covers the observer's full rack (e.g.
`vs_jeremy.gcg` at `test/infer_test.c:648`, where P1 plays 5 of 7 tiles leaving
2 tiles, P2 holds 6 tiles `AHIILR`, $T_{\text{off}} = 13$, $p = 5$,
$K_{\text{obs}} = 6$, `tiles_to_infer = 2`), removing `tiles_to_infer = 2` tiles
leaves `bag_get_letters(bag) == 0` (previously it left `2`, falsely putting
`MoveGen` into `number_of_tiles_in_bag > 0` KLV mode during an empty-bag turn!).

With `bag_get_letters(bag) == 0`:
- `MoveGen` evaluates moves with `standard_endgame_adjustment` instead of
  `klv_get_leave_value` (`src/ent/static_eval.h:119-121`).
- In `evaluate_possible_leave` (`src/impl/inference.c:222-225`), computing the
  non-opening portion via `static_eval_get_nonopening_move_equity` instead of
  raw `target_score + current_leave_value` ensures `target_equity_cutoff`
  **also** uses `standard_endgame_adjustment` when `bag_get_letters(bag) == 0`!
  (Note also that when $B = 0$ and $K_{\text{obs}}$ equals the observer's full
  rack, `inference->bag_as_rack` had exactly `tiles_to_infer` tiles before
  recursion and is therefore empty at leaf depth, where line 236's
  `|| rack_is_empty(inference->bag_as_rack)` already accepts the unique legal
  rack; using `static_eval_get_nonopening_move_equity` keeps `E_obs` and
  `E_top` 100% consistent for PR 2's $\Delta E$ calculation as well.)

---

### 1.3 Bug 1B (live on `main`): Opening-move `placement_adjustment` missing from `target_equity_cutoff`

#### Root cause
On Turn 1 (`board_get_tiles_played(board) == 0`), `MoveGen` scores every tile
placement via `static_eval_get_move_equity_with_leave_value`
(`src/impl/move_gen.c:542-547`, `src/ent/static_eval.h:177-181`), adding
`placement_adjustment(...) <= 0` (`-0.7` to `-3.5+` points whenever a vowel is
placed adjacent to a DLS/TLS/DWS/TWS lane premium).

Meanwhile, `evaluate_possible_leave` (`src/impl/inference.c:223-224`) computes:
```c
target_equity_cutoff = inference->target_score + current_leave_value +
                       inference->equity_margin;
```
omitting `placement_adjustment`. Whenever the opening play incurred a negative
`placement_adjustment`, `target_equity_cutoff` is too high by
`|placement_adjustment|`, inflating the effective margin on Turn 1 (and in PR 2
making `E_obs > E_top` on the true best play, producing negative regret
$\Delta E < 0$).

#### Subtlety with `-pat` and `pat_active_classes`
Read `src/impl/gameplay.c:961-976`, `src/impl/move_gen.c:3377-3395`, and
`src/ent/pat_eval.h:263-269` carefully:
- `get_top_equity_move_for_inferences` passes `args.disable_pat = true`.
- When `args->disable_pat` is true, `gen_load_position` executes
  `pat_eval_context_disable(&gen->pat_eval_ctx)` (`weights = NULL`).
- `pat_eval_context_get_active_classes(&gen->pat_eval_ctx)` therefore returns
  **`0`** (not the player's loaded PAT class mask!), so `MoveGen` applies
  **both** `opening_move_word_penalties` and `opening_move_letter_penalties`
  during inference even if the player has a `-pat` loaded.
- Therefore, as long as `get_top_equity_move_for_inferences` disables PAT,
  `inference.c` **must pass `pat_active_classes = 0`** to
  `placement_adjustment` so `E_obs` matches `MoveGen` millipoint-for-millipoint
  whether `-pat` is loaded or not. (When PAT-aware inference `use_pat` is later
  enabled in PR 2, both `MoveGen` and `E_obs` will share the same active
  `PATEvalContext` and its `pat_eval_context_get_active_classes`.)

#### Caller audit and zero-inner-loop-cost plumbing
Every inference caller in the repository falls into one of two cases:
1. **Exact `Move` is already in hand:**
   - `args->use_game_history == true` (`infer` with no args at
     `src/impl/config.c:3664`, `sim -sinfer true` at `src/impl/config.c:3882`,
     `analyze -sinfer true` at `src/impl/analyze.c:1168`,
     `infer_for_test_with_history` at `test/infer_test.c:143`):
     `populate_inference_args_with_game_history` (`src/impl/inference.c:695`)
     already fetches `const Move *move = validated_moves_get_move(last_move, 0);`.
   - `autoplay` with sim inference (`src/impl/autoplay.c:1029-1032`): already
     holds `&game_runner->previous_move`.
   - `test/infer_cmp_test.c:140-147`: already holds `top_move`.
2. **Only tile multiset + integer score supplied (no board coordinates):**
   - Explicit CLI `infer <player> <tiles> <score> ...`
     (`src/impl/config.c:3778`) and `infer_for_test` (`test/infer_test.c:86`):
     no coordinates exist, so `args->target_move == NULL` and
     `target_placement_adjustment` defaults to `0`.

Implementation steps:
1. In `src/ent/inference_args.h`:
   - `#include "move.h"` (or `"../ent/move.h"`, formatted with `python3 format.py --write`).
   - Add `const Move *target_move;` to `InferenceArgs`.
   - Initialize `args->target_move = NULL;` in `infer_args_fill`.
2. In `src/impl/autoplay.c` (right after `infer_args_fill` around line 1031):
   - Set `infer_args->target_move = &game_runner->previous_move;`.
3. In `src/impl/inference.c`:
   - Add `Equity target_placement_adjustment;` and `int target_tiles_played;` to
     `struct Inference`.
   - In `populate_inference_args_with_game_history`:
     ```c
     args->target_move =
         (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) ? move : NULL;
     ```
     (Note: `game_play_n_events` at line 741 only replays up to
     `most_recent_move_event_index` into `game_dup` without mutating
     `game_history`, so `move` remains valid throughout `infer()`.)
   - In `complete_inference_setup`, precompute `inference->target_tiles_played`
     and `inference->target_placement_adjustment` **once per inference** (zero
     work per candidate leave):
     ```c
     inference->target_tiles_played =
         rack_get_total_letters(args->target_played_tiles);
     inference->target_placement_adjustment = 0;
     const Board *board = game_get_board(inference->game);
     if (args->target_num_exch == 0 && args->target_move != NULL &&
         move_get_type(args->target_move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
         board_get_tiles_played(board) == 0) {
       Equity word_penalties[2 * BOARD_DIM];
       Equity letter_penalties[2 * BOARD_DIM];
       board_copy_opening_penalties(board, word_penalties, letter_penalties);
       inference->target_placement_adjustment =
           placement_adjustment(game_get_ld(inference->game), args->target_move,
                                word_penalties, letter_penalties, 0);
     }
     ```
   - In `evaluate_possible_leave` (`src/impl/inference.c:218-226`):
     ```c
     Equity target_equity_cutoff;
     if (is_exchange) {
       target_equity_cutoff =
           get_best_exchange_equity(inference) + inference->equity_margin;
     } else {
       const Game *game = inference->game;
       const Equity nonopening_equity = static_eval_get_nonopening_move_equity(
           game_get_ld(game), inference->current_target_leave,
           player_get_rack(game_get_player(game, 1 - inference->target_index)),
           inference->target_tiles_played,
           bag_get_letters(game_get_bag(game)), inference->target_score,
           current_leave_value);
       target_equity_cutoff = nonopening_equity +
                              inference->target_placement_adjustment +
                              inference->equity_margin;
     }
     ```
     Look at what calling `static_eval_get_nonopening_move_equity` here achieves:
     - When `bag > 0` on `main`, it compiles down to
       `target_score + current_leave_value + peg_adjust_values[...]` (identical
       instruction count).
     - When `bag == 0`, it automatically applies `standard_endgame_adjustment`,
       matching `MoveGen` in empty-bag inference!
     - When PR `#931` (`origin/pegadjust-table`) rebases onto PR 1, updating
       this single `static_eval_get_nonopening_move_equity` call to pass the
       target player's `peg_adjust_get(peg_adjust, bag_plus_rack_size)`
       automatically fixes Bug 1C below!

---

### 1.4 Bug 1C (live in open PR `#931` `origin/pegadjust-table`): Tile-placement `peg_adjust` omitted in `evaluate_possible_leave`

When rebasing PR `#931` (`origin/pegadjust-table`, `f2cf18d1`) onto PR 1:
- `#931` updated `get_best_exchange_equity` (`src/impl/inference.c:193-205`) to
  pass `peg_adjustment = 0`, and updated `MoveGen` to add
  `gen->peg_adjust_by_tiles_played[tiles_played]` to every generated tile
  placement.
- However, `#931` left `evaluate_possible_leave`'s tile-placement branch as
  `target_score + current_leave_value + equity_margin` without the played move's
  `peg_adjustment` (and without PR 1's bag-count fix, `MoveGen` also indexed the
  table at `B + tiles_to_infer` instead of `B`). With Quackle's `+10.0` pt
  1-in-bag adjustment (`index 8`), any play leaving 1 tile in the bag got `+10`
  pts in `MoveGen` and `0` pts in `target_equity_cutoff`, rejecting even the
  opponent's true leave under default `-eqmargin 5`.
- **Rebase fix on `#931`:** Precompute `inference->target_peg_adjustment` once
  in `complete_inference_setup` from
  `player_get_peg_adjust(game_get_player(inference->game, args->target_index))`
  at `bag_get_letters(bag) - inference->target_tiles_played + RACK_SIZE` (after
  PR 1's `bag_draw_random_letter` loop so `bag_get_letters(bag)` is `B`), pass
  it to `static_eval_get_nonopening_move_equity` in `evaluate_possible_leave`,
  and add a regression test in `test/peg_adjust_test.c` running `infer` on a
  move that leaves 1 tile in the bag with `quackle_pegadjust.csv` loaded.

---

### 1.5 Exact regression tests for PR 1 (`test/infer_test.c`)

1. **`test_preendgame_no_phantom_exchange(Config *config)`:**
   - Load a mid/late-game CGP with `1 <= B <= 6` unseen bag tiles (e.g., modify
     an existing late-game position or construct a sparse board where `B = 5`
     bag tiles remain, Player 1 holds 7 known tiles, and Player 0 holds a clunky
     vowel/V/W-heavy rack such as `AEUUUVW` or `IIUUVWW` where:
     - every legal board placement scores poorly and keeps a bad 4–5 tile leave
       so the true best legal board move $m^*$ has equity $E(m^*) < -5.0$ pts;
     - exchanging 5–7 tiles *would* have equity $\approx 0$ to $+3$ pts — more
       than `10` points higher than $E(m^*)$ — if `B >= 7` made exchanges legal).
   - Commit $m^*$ (`generate_moves` confirms $m^*$ is the #1 legal move on that
     rack because `B = 5 < 7` forbids exchanges).
   - Run inference with `equity_margin = 0` (and `5`), testing **both**
     `use_infer_cutoff_optimization = true` and `false`, and **both**
     `use_game_history = true` and direct `infer_for_test`.
   - Assert `leave_rack_list_get_count(lrl) >= 1` and that Player 0's true
     kept leave appears in `results` with positive draws. (Without Bug 1A's fix,
     `MoveGen` sees `5 + tiles_to_infer >= 7`, generates the illegal exchange,
     and returns `0` leaves.)

2. **`test_opening_placement_adjustment_parity(Config *config)`:**
   - Start from an empty CSW21 board (`cgp` empty or `new` game).
   - Pick a 7-tile rack whose true #1 static-equity opening play places a vowel
     on a DLS-adjacent penalized square (e.g., 5- or 6-letter play spanning
     `8D`–`8H` or `8G`–`8K` with a vowel on `8D` or `8K`, incurring
     `placement_adjustment < 0`), AND construct a second candidate leave from
     the bag whose best opening move on that rack has `0` vowel penalty and
     beats `score + leave + placement_adjustment` by `0.2`–`0.6` pts (strictly
     between `0` and `|placement_adjustment|`).
   - Commit the penalized opening move and run history-based inference at
     `-ima 0` (`equity_margin = 0`) with cutoff optimization both `true` and
     `false`:
     - Assert the true rack's leave **is** accepted at `-ima 0`.
     - Assert the second leave (whose best move beats the penalized opening
       equity by `0.2`–`0.6` pts) **is rejected** at `-ima 0` (before Bug 1B's
       fix, omitting `placement_adjustment` inflated `target_equity_cutoff` and
       falsely accepted it).
     - Assert cutoff-on and cutoff-off produce identical `InferenceResults`.

---

## Part 2 (PR 2): Standalone static/PAT softmax inference

### 2.1 Motivation and prior branch findings

Across prior experiments:
- `origin/archive/2026-10-04/claude/inference-scorer` (`91381856`) built
  `test/infer_score_test.c` and `test/infer_score_summarize.py` to benchmark
  Boltzmann/Luce softmax posteriors across $\tau \in \{0.5, 1, 2, 4, 6, 10, 15, 25, 40\}$
  against hard step margins $m \in \{0, 2, 5, 10\}$ on `static`, `pat`, and
  `PlayChooser` movers using out-of-sample $\log P(\text{true leave})$, true-leave
  rank, posterior entropy, and zero-probability miss rate.
- `origin/claude/value-net-pat` (`02d0a3af`, `204a1300`, `dc070105`) added
  `ValueNetLeaveInference` (softmax over value-net utilities for 1–2 tile
  leaves), `use_pat` static inference (`gen_last_pat_term`),
  `inference_uniform_mix`, and Fisher noncentral hypergeometric `LeaveOdds`
  tilted by $\exp(\beta \cdot \text{KLV}(L))$ for 4–6 tile leaves.
- `origin/peg-inference` (`8dccbd5c`) showed why uncalibrated flat-weight
  hard-margin posteriors hurt deep search: wide margins assign identical
  per-draw likelihood to a $-12$ pt suboptimality and a $0$ pt best play,
  diluting the true leave (`-0.071` to `-0.171` win% vs uniform draws in deep
  PEG), whereas tight margins (`5` pts) assign **zero probability** whenever the
  opponent makes a 7-pt defensive block or sim sacrifice.
- Stronger rollout policies — `1PEG`/`2PEG` nets, 2-ply bag-0 endgame search
  (`origin/claude/peg-nets-main`), or value-net replies — actively exploit
  specific unseen tiles and outplay threats, amplifying hard-margin errors that
  noisy static rollouts previously washed out. At 7 tiles in the bag (`14`
  unseen tiles), a 3–5 tile play leaves `2–4` kept tiles (`91` to `1,001`
  distinct leaves), small enough for exact static/PAT softmax enumeration in a
  few milliseconds.

---

### 2.2 Splitting `MoveGen`'s WMP anchor floor from `stop_on_threshold`

#### Why naive softmax breaks `use_infer_cutoff_optimization`
Under regret-Boltzmann softmax with temperature $\tau > 0$ and tail truncation
margin $M_{\text{tail}} \approx 6\tau$ (where $\exp(-6) \approx 0.00248$; leaves
with $\Delta E > M_{\text{tail}}$ are dropped or given floor weight):
- We still want `#838`'s early-exit speedup: as soon as `MoveGen` finds **any**
  move with equity $> E_{\text{obs}} + M_{\text{tail}}$, abort move generation
  for this candidate leave (`stop_on_threshold`).
- For every surviving candidate leave ($E_{\text{top}} \le E_{\text{obs}} + M_{\text{tail}}$),
  we need the **exact** value of $E_{\text{top}}$ so we can compute
  $\Delta E = E_{\text{top}} - E_{\text{obs}} \in [0, M_{\text{tail}}]$ and
  weight $w(L) = \text{draws}(L) \cdot \exp(-\Delta E / \tau)$.

If we pass `target_equity = E_obs + M_tail` into today's `MoveGen`:
- `wmp_move_gen_add_anchors` (`src/impl/wmp_move_gen.h:1025-1026`) skips every
  WMP subanchor with `highest_possible_equity < E_obs + M_tail`!
- Any subanchor whose best move lies in $[E_{\text{obs}}, E_{\text{obs}} + M_{\text{tail}})$
  can be discarded, causing `MoveGen` to return an underestimated `top_move`
  (or `EQUITY_PASS_VALUE`) and falsely assign $\Delta E = 0 \implies \text{weight} = 1.0$
  to suboptimal racks — silently collapsing softmax back into a flat step
  function!

#### Exact two-threshold fix
Separate the two bounds in `MoveGenArgs` (`src/impl/move_gen.h`) and `MoveGen`
(`src/impl/move_gen_base.h`):
1. `target_equity` (or `stop_equity_cutoff`): upper early-exit threshold
   (`stop_on_threshold = args->target_equity != EQUITY_MAX_VALUE`; sets
   `threshold_exceeded = true` when `move_equity_or_score > gen->target_equity_cutoff`).
   - Hard-margin inference: `E_obs + equity_margin`.
   - Softmax inference: `E_obs + M_tail`.
2. `anchor_equity_floor` (new field on `MoveGenArgs` / `MoveGen`, defaulting to
   `EQUITY_INITIAL_VALUE` when `0` / unset, or set explicitly by callers):
   lower bound passed into `wmp_move_gen_add_anchors`:
   - Hard-margin inference: `anchor_equity_floor = E_obs + equity_margin`
     (identical to today — zero performance change when softmax is off!).
   - `BestMoveCarry` (`gen_seed_carried_best_move`): sets
     `anchor_equity_floor = carried_equity` (with `stop_on_threshold = false`,
     cleaner than mutating `target_equity_cutoff`!).
   - Softmax inference on tile placements: since the observed placement $m_{\text{obs}}$
     is itself a legal move on every candidate rack, $E_{\text{top}} \ge E_{\text{obs}}$
     is guaranteed (once PR 1's parity fixes are in!), so set
     **`anchor_equity_floor = E_obs`**! (For exchanges where $B < 7$ is
     impossible, $E_{\text{top}} \ge E_{\text{best\_k\_exch}}$ also holds
     because the best $k$-tile exchange is generated during
     `gen_look_up_leaves_and_record_exchanges` before WMP anchors run!)
   - Also check `gen_record_scoring_plays` (`src/impl/move_gen.c:2950-2995`):
     as anchors are popped in descending `highest_possible_equity` order, once
     `best_move` reaches `>= anchor->highest_possible_equity`, standard branch-and-bound
     already stops naturally. Seeding `anchor_equity_floor = E_obs` only prunes
     WMP subanchors strictly `< E_obs`, which can never hold a move `> E_obs`,
     so every move strictly beating $E_{\text{obs}}$ is guaranteed to be found!
     And if no move strictly beats $E_{\text{obs}}$, $\Delta E = \max(0, E_{\text{top}} - E_{\text{obs}}) = 0$
     either way!

**Mandatory invariant test (`test/infer_cmp_test.c`):**
Extend `test_infer_cutoff_optimization` to run softmax inference across multiple
temperatures $\tau$ with `use_infer_cutoff_optimization = true` vs `false` (and
WMP `true` vs `false`) and assert the posterior weights and `AliasMethod`
probabilities match to floating-point tolerance (`1e-12`).

---

### 2.3 Regret Boltzmann vs full Luce denominator

Two candidate likelihood models should be compared directly in `inferscore` and
`PlayChooser` game pairs:

1. **Regret Boltzmann (`MOVE_RECORD_BEST` only):**
   $$w(L) = \text{draws}(L) \cdot \exp\!\left(-\frac{\max(0, E_{\text{top}}(L) - E_{\text{obs}}(L))}{\tau}\right)$$
   - Requires only `E_top(L)` — compatible with `MOVE_RECORD_BEST`, WMP
     subanchor pruning at `E_obs`, early exit at `E_obs + M_tail`, and the
     single-pass `census_play_scores` accelerator (`docs/fast-inference-and-movegen-plan.md`).
   - Fast enough to run by default in `sim -sinfer true` and `PlayChooser`.

2. **Truncated Luce / Boltzmann with crowded-alternatives normalization:**
   $$w(L) = \text{draws}(L) \cdot \frac{\exp((E_{\text{obs}}(L) - E_{\text{top}}(L))/\tau)}{\sum_{m \in \mathcal{M}_{M_{\text{tail}}}(L)} \exp((E(m) - E_{\text{top}}(L))/\tau)}$$
   - Captures the Bayesian "crowded alternatives" effect: a rack where
     $m_{\text{obs}}$ is uniquely best ($P \approx 1$) receives $7\times$ the
     posterior weight of a rack where 6 other legal plays tie $m_{\text{obs}}$
     ($P \approx 1/7$).
   - **Performance note if Luce wins in `inferscore`:** Do **not** use
     `MOVE_RECORD_ALL` into a 20,000-slot `MoveList`. Instead, use
     `MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST` with `eq_margin_movegen = M_tail`
     (note: audit WMP subrack/anchor deduplication first so no play is counted
     twice, and keep `anchor_equity_floor` off or at `E_obs - M_tail` when
     accumulating competitors below `E_obs`). Because `MOVE_RECORD_BEST` is
     dramatically faster, start PR 2 with **Regret Boltzmann** as the fast C
     engine path and only add a Luce denominator pass (or top-leaves two-stage
     rescoring on surviving leaves) if `inferscore` + game pairs show a
     statistically significant win% lift per wall-clock second.

---

### 2.4 Continuous weights without slowing down the flag-off path

Per `AGENTS.md` ("Feature flags must be free when off"):

1. **`AliasMethod` (`src/ent/alias_method.h`):**
   - Today `AliasMethodItem` stores `Rack rack; int count;` and
     `alias_method_generate_tables` (`lines 88-156`) immediately divides
     `(double)count / (double)total_count`.
   - Note that `ValueNetLeaveInference` (`origin/claude/value-net-pat`) scaled
     *normalized* probabilities by `10^6` into `int count`, which underflows to
     `0` in the tail when `4–6` tile inferences have $30{,}000+$ leaves with
     individual $P(L) < 5 \times 10^{-7}$.
   - Switching `AliasMethodItem.count` (and `total_count`) to `double weight`
     (on 64-bit platforms `AliasMethodItem` already pads `int count` after
     `Rack`, or check `sizeof(AliasMethodItem)` — `Rack` is 2-byte aligned, so
     verify struct size; alternatively store `uint64_t scaled_weight` computed
     as `draws * (uint64_t)llround(ldexp(likelihood, 24))`, which never
     underflows because unnormalized `likelihood >= exp(-M_tail / tau)` is at
     least `exp(-6) * 2^24 ≈ 41,585` even for 1 draw, and overflows `uint64_t`
     only above $2^{64} / 2^{24} = 2^{40}$ total draws, far above standard
     100-tile bags!). Using `double weight` in `AliasMethod` is even simpler
     and eliminates division precision concerns across all board sizes
     (`BOARD_DIM=21`, 200 tiles). Benchmark `infer` with softmax off to confirm
     `~0` wall-clock delta.

2. **`InferenceResults`, `Stat`, and `LeaveRackList`
   (`src/ent/inference_results.{h,c}`, `src/ent/stats.{h,c}`,
   `src/ent/leave_rack.{h,c}`):**
   - Keep **raw integer `draws` (`uint64_t`)** alongside **continuous posterior
     `weight` (`double`)** (when softmax is off, `weight = (double)draws`).
   - `Stat` (`src/ent/stats.h`) already uses `double` internally for mean/stdev;
     check `stat_push(stat, value, weight)` — if its weight parameter is
     currently `uint64_t`, adding or widening to `double` weights lets leave-value
     mean/stdev reflect the softmax posterior while `total_draws` continues to
     report exact combinatorial bag draws.
   - In `LeaveRack`, storing both `draws` and `weight` (or `equity_loss` in
     millipoints) allows CLI `infer` and `ucgi` / `wasmentry` to display exact
     bag draw counts, posterior probability `%`, and $\Delta E$.

3. **Intra-rack $k$-tile exchange splitting (`#837`):**
   - In `evaluate_possible_leave` (`src/impl/inference.c:243-275`), an exchange
     of $k$ tiles currently assigns 100% of candidate rack $R$'s weight to the
     single argmax `best_exchange_leave`.
   - Under softmax, during `find_best_exchange_leave`, accumulate
     $z_i = \exp((E(\text{keep}_i) - E(\text{best\_keep})) / \tau)$ over the
     $\binom{7}{7-k} \le 35$ subracks of size $7-k$, and distribute rack $R$'s
     posterior weight $w(R)$ across each competitive kept leave proportionally
     to $z_i / \sum_j z_j$ (skipping subracks with regret $> M_{\text{tail}}$).
     Since $\binom{7}{k} \le 35$, this stack buffer has at most 35 entries and
     fixes arbitrary KLV tie-breaks on near-equal exchanges.

4. **Optional PAT-aware static inference (`-infpat` / `use_pat`):**
   - Port `02d0a3af`'s opt-in `use_pat` flag: when enabled and the target
     player has PAT weights loaded, pass `disable_pat = false` to
     `get_top_equity_move_for_inferences` and build a position-level
     `PATEvalContext` once per worker in `complete_inference_setup` (updating
     only `unseen_counts` for the candidate rack, or evaluating
     `pat_eval_move_penalty` on `target_move` + `current_target_leave` and
     passing `pat_eval_context_get_active_classes` to `placement_adjustment`).
   - Because PAT prices defensive TWS blocks and setup penalties into static
     equity, `use_pat` shrinks the apparent "blunder" size of strong defensive
     plays and pairs with a tighter temperature $\tau$.

---

### 2.5 Configuration & validation checklist for PR 2

- **CLI / Config options (`src/impl/config.c`, `src/impl/help_table.c`):**
  - `-inftemp` / `-itau <double>` (default `0`, meaning hard `-eqmargin` mode so
    default behavior and performance are unchanged until calibrated and turned
    on in a separate defaults PR).
  - Keep `-eqmargin` / `-ima` working as today when `-inftemp == 0`; when
    `-inftemp > 0`, use `-eqmargin` (if explicitly set) or `6 * tau` as
    $M_{\text{tail}}$, and optionally support `-infumix <double>` (uniform floor
    mixture, default `0`) from `dc070105`.
- **Benchmarks required before merging:**
  1. **Flag-off performance control (`-inftemp 0`):** run fixed-seed `sim` and
     `infer` benchmarks on `make release` before vs after; must measure `~0.0%`
     time difference (`AGENTS.md`).
  2. **Cutoff parity (`infer_cmp_test`):** `-ic true` vs `-ic false` and
     `-wmp true` vs `-wmp false` at $\tau \in \{1.0, 3.0, 8.0\}$.
  3. **Posterior quality (`inferscore`):** compare $\tau$ grid against hard
     margins on out-of-sample `static`, `pat`, and `PlayChooser` games.
  4. **End-to-end game-pair quality (`autoplay -gp true`):**
     - Static 2-ply `sim -sinfer true` with `-inftemp <best>` vs `-inftemp 0`
       (`1000+` game pairs, `-wmp true -threads <N>`).
     - Late-game 7-in-bag rollout benchmark (`pegsim` / 2-ply endgame tail +
       neural rollouts) with softmax inference vs hard-margin vs uniform draws.

---

## CI & pre-push checklist (both PRs)

Run every check from `AGENTS.md` before pushing:
```bash
python3 format.py --write
make magpie_test BUILD=sanitize
./bin/magpie_test infer sim autoplay analyze
make magpie_test BOARD_DIM=21 BUILD=sanitize
./bin/magpie_test infer
./cppcheck.sh
./tidy.sh
python3 find_circ_deps.py
python3 check_portability.py
python3 check_windows_compile.py
```
