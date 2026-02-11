# Clock Management System for MAGPIE

## Motivation

In tournament Scrabble, each player has a fixed time bank (typically 25 minutes) with overtime penalties (typically -10 points per minute over). Strong play requires not just finding good moves but finding them *within a time budget that leaves enough time for future turns* — especially the critical late-game and endgame turns where exact search can prove the winning play.

MAGPIE currently has no clock management. It has `time_limit_seconds` for individual simulation calls, but no concept of:
- Total remaining game time
- Time value across turns (spending 1 second now vs saving it)
- When to stop searching because more time won't change the decision
- How to split time between simulation and endgame search
- How to adapt to different hardware speeds

### The Core Problem

A second of thinking is not equally valuable across all turns. In the opening, the top 5 moves often have nearly identical equity — spending 30 seconds to distinguish them is wasteful. In the pre-endgame, the tile distribution is highly constrained and simulation results diverge significantly from static evaluation — every second of simulation matters. In the endgame, exact search can prove the winning line — but only if there's enough time to complete the search.

A good clock manager banks time early when it's cheap, and spends it late when it's valuable. The system must work across different hardware: a laptop with 4 cores needs the same strategic allocation as a server with 64 cores, even though the server completes more samples per second.

## Game Clock Model

### Time Control Parameters

```c
typedef struct TimeControl {
    double initial_time_seconds;      // Total time bank (e.g., 1500.0 for 25 min)
    double overtime_penalty_per_min;  // Points deducted per minute over (e.g., 10.0)
    double increment_seconds;        // Per-move increment, if any (e.g., 0.0)
} TimeControl;
```

### Per-Player Clock State

```c
typedef struct PlayerClock {
    double time_remaining_seconds;    // Remaining time bank
    int turns_played;                 // Turns completed so far
    double total_time_used;           // Cumulative time spent
    // Running statistics for calibration:
    double mean_movegen_time;         // Average move generation time
    double mean_sim_time_per_sample;  // Average time per BAI sample
    double mean_endgame_time_per_node; // Average time per endgame node
} PlayerClock;
```

The clock state is updated after every turn. The running statistics adapt to the current hardware — they are measured, not assumed.

## Time Allocation Policy

### Overview

Each turn, the clock manager computes a **time budget** for that turn. The budget is then split between simulation and endgame search. Within each phase, convergence-based stopping can terminate early, returning unused time to the bank.

```
Turn budget = base_allocation × complexity_multiplier × phase_multiplier
```

### Base Allocation: Proportional with Reserve

The simplest allocation: divide remaining time by estimated remaining turns, with a reserve for the endgame.

```c
double base_allocation(const PlayerClock *clock, int estimated_turns_remaining) {
    // Reserve 20% of remaining time for endgame (last ~3 turns)
    double reserve_fraction = 0.20;
    double available = clock->time_remaining_seconds * (1.0 - reserve_fraction);

    // If we're in the last 3 turns, tap into the reserve
    if (estimated_turns_remaining <= 3) {
        available = clock->time_remaining_seconds;
    }

    double base = available / max(estimated_turns_remaining, 1);

    // Floor: never allocate less than movegen time + tiny margin
    double floor = clock->mean_movegen_time + 0.1;
    return max(base, floor);
}
```

**Estimating remaining turns**: `turns_remaining ≈ (tiles_in_bag + tiles_on_rack) / avg_tiles_per_turn`. Average tiles per turn is ~4.5 for Scrabble. With 60 tiles remaining (bag + racks), expect ~13 more turns (both players combined), ~6-7 for this player.

### Complexity Multiplier

Not all positions need equal time. Cheap features computed after move generation signal how much search will help:

```c
double complexity_multiplier(const MoveList *move_list, int tiles_in_bag,
                             int score_differential) {
    double mult = 1.0;

    // 1. Equity gap between #1 and #2 moves
    //    Large gap → search unlikely to change ranking → spend less time
    Equity gap = move_get_equity(move_list, 0) - move_get_equity(move_list, 1);
    if (gap > 5000) mult *= 0.3;       // >5 points: almost certainly right
    else if (gap > 2000) mult *= 0.6;  // 2-5 points: probably right
    else if (gap < 500) mult *= 1.5;   // <0.5 points: tight, invest more

    // 2. Number of competitive moves
    //    Many moves within 2 points of the best → harder decision
    int competitive_count = count_moves_within(move_list, 2000); // 2.0 equity points
    if (competitive_count > 10) mult *= 1.3;
    if (competitive_count <= 2) mult *= 0.7;

    // 3. Score differential
    //    Close game → more important to get it right
    if (abs(score_differential) < 20000) mult *= 1.2;  // Within 20 points

    // 4. Bag size affects simulation variance
    //    Smaller bag → less variance → simulation converges faster
    if (tiles_in_bag < 15) mult *= 0.8;

    return clamp(mult, 0.2, 3.0);
}
```

### Phase Multiplier

Different game phases have different time value:

```c
double phase_multiplier(int tiles_in_bag, int tiles_on_rack) {
    int total_unseen = tiles_in_bag + tiles_on_rack;

    if (total_unseen > 80) return 0.6;   // Opening: static eval is good enough
    if (total_unseen > 50) return 0.8;   // Early mid-game
    if (total_unseen > 25) return 1.0;   // Mid-game: standard allocation
    if (total_unseen > 14) return 1.5;   // Pre-endgame: simulation matters most
    if (tiles_in_bag > 0)  return 2.0;   // Near-endgame: critical decisions
    return 2.5;                           // Endgame: exact search is possible
}
```

The intuition:
- **Opening**: Leave values are well-calibrated for the full bag. Static evaluation is nearly as good as simulation. Save time.
- **Pre-endgame**: The bag is depleted enough that static leaves are inaccurate (dynamic leaves help here), and simulation results diverge significantly from static eval. Spend time.
- **Endgame**: Exact negamax search can prove the optimal play. Every second of search could find a deeper solution. Spend heavily.

## Diminishing Returns Detection

The clock manager should not blindly spend its budget. Within the allocated time, it monitors search progress and stops early when additional computation won't improve the decision enough to justify the time spent.

### The Right Metric: Expected Equity Loss

MAGPIE's BAI framework is built around **best-arm identification** — maximizing the probability that the top-ranked move is truly the best. But for clock management, the right question is different: **how much equity do we expect to lose by stopping now?**

Consider two scenarios after 1000 simulation samples:
- **Scenario A**: Move X has mean equity 42.5, Move Y has mean equity 42.3. Even if we've ranked them wrong, the expected loss is at most ~0.2 points. Not worth spending more time.
- **Scenario B**: Move X has mean equity 42.5, Move Z has mean equity 38.0, but both have high variance. If Z is actually better and we stop now, we lose ~4.5 points. Worth continuing.

BAI's P(best arm) metric treats both scenarios the same way (both have uncertainty about the ranking). Expected equity loss captures the asymmetry: it weights the probability of a wrong decision by the cost of that wrong decision.

Formally, the expected equity loss (simple regret) is:

```
EEL = E[equity(true_best) - equity(chosen_move)]
```

This is zero when we choose the best move, and positive otherwise. Computing it exactly requires knowing the true equity of each arm, which we don't have. The difficulty is that estimating EEL from simulation data (means, variances, sample counts) is not analytically tractable in a useful form — the mathematical literature on computing simple regret bounds in the multi-armed bandit setting offers asymptotic results but not practical closed-form estimators for the finite-sample case.

### Neural Net Approach

We train a small neural network to predict expected equity loss from simulation state features. This sidesteps the analytical difficulty entirely.

**Input features** (extracted from BAI state at any checkpoint):

```c
typedef struct SimSnapshot {
    // Per-arm statistics (top K arms, sorted by mean equity)
    int num_arms;                   // Number of candidate moves
    int total_samples;              // Total samples across all arms
    // For top K arms (e.g., K=8):
    double mean[MAX_SNAPSHOT_ARMS];     // Current mean equity
    double var[MAX_SNAPSHOT_ARMS];      // Current variance
    int samples[MAX_SNAPSHOT_ARMS];     // Samples allocated to this arm
    // Derived features:
    double gap_1_2;                 // Equity gap between #1 and #2
    double gap_1_3;                 // Equity gap between #1 and #3
    double max_stderr;              // Largest standard error among top arms
    double mean_stderr;             // Average standard error among top arms
    double log_total_samples;       // log(total_samples) — captures diminishing returns
    // Game context:
    int tiles_in_bag;               // Affects simulation variance
    int score_differential;         // Affects how much the decision matters
} SimSnapshot;
```

**Output**: Predicted expected equity loss (in centipoints) if simulation stops now.

**Training data**: Generated from extended self-play:

1. At each position, run simulation for a large budget (e.g., 100K samples)
2. Record the final best move as "ground truth"
3. At multiple checkpoints during the simulation (100, 200, 500, 1K, 2K, 5K, 10K, 20K, 50K samples), record:
   a. The SimSnapshot features at that checkpoint
   b. The move that would be chosen if we stopped at that checkpoint
   c. The equity of that move vs the final ground-truth best move's equity → this is the realized equity loss for this stopping point
4. Average over many random seeds to get expected equity loss per checkpoint

This produces training examples: (SimSnapshot → expected equity loss). The neural net learns patterns like:
- Large gap + low stderr → low expected loss (safe to stop)
- Small gap + high stderr → high expected loss (keep going)
- Many competitive arms + early samples → high expected loss (too soon)

**Architecture**: A small feedforward network (2-3 hidden layers, 32-64 neurons each) is sufficient. The input is low-dimensional (~30 features for K=8 arms), and the mapping is relatively smooth. Inference cost is negligible (<1μs) compared to a simulation sample (~1ms).

### Stopping Criterion

At each convergence check, evaluate the neural net and compare against the value of saving time:

```c
typedef struct SimConvergence {
    SimSnapshot snapshot;           // Current simulation state features
    int check_interval_samples;     // How often to check (adaptive)
    double eel_estimates[MAX_EEL_HISTORY]; // Recent EEL predictions
    int eel_history_count;
} SimConvergence;

bool should_stop_simulation(const SimConvergence *conv,
                            const EELPredictor *eel_model,
                            double time_remaining_for_turn,
                            double estimated_future_time_value) {
    // Predict expected equity loss if we stop now
    double current_eel = eel_predict(eel_model, &conv->snapshot);

    // Convert remaining turn time to equity value (opportunity cost)
    double future_value = time_remaining_for_turn * estimated_future_time_value;

    // Stop if the expected equity loss from stopping is less than the
    // equity value of saving this time for future turns
    if (current_eel < future_value) return true;

    // Also stop if EEL has plateaued (not decreasing meaningfully)
    // This catches cases where more samples aren't helping
    if (conv->eel_history_count >= 3) {
        double recent_decrease = conv->eel_estimates[conv->eel_history_count - 3]
                               - current_eel;
        double time_for_decrease = /* time elapsed over last 3 checks */;
        double marginal_eel_rate = recent_decrease / time_for_decrease;
        if (marginal_eel_rate < estimated_future_time_value) return true;
    }

    return false;
}
```

The key insight: the stopping decision becomes a comparison of two rates:
- **Rate of EEL decrease**: How fast is simulation reducing our expected equity loss?
- **Future time value**: How much equity per second could we gain by saving this time for later turns?

When the first rate drops below the second, stop.

### Fallback Without Neural Net

Before the neural net is trained, a simpler heuristic can approximate EEL:

```c
double estimate_eel_heuristic(const SimSnapshot *snap) {
    // Approximate EEL as: probability of wrong choice × cost of wrong choice
    // Using normal approximation for the gap between #1 and #2
    double gap = snap->gap_1_2;
    double combined_stderr = sqrt(snap->var[0] / snap->samples[0]
                                + snap->var[1] / snap->samples[1]);
    if (combined_stderr < 1e-9) return 0;

    // P(wrong) ≈ Φ(-gap / combined_stderr)
    double z = gap / combined_stderr;
    double p_wrong = 0.5 * erfc(z / sqrt(2.0));

    // Expected cost given wrong ≈ gap (the top two are swapped)
    // This ignores arms #3+ but captures most of the loss
    return p_wrong * gap;
}
```

This is a rough two-arm approximation. It underestimates EEL when arm #3 is competitive, and it relies on normality assumptions that may not hold with few samples. The neural net learns to correct for these limitations.

### For Endgame Search

Endgame uses iterative deepening, which provides natural checkpoints:

```c
typedef struct EndgameTimeControl {
    double time_budget;               // Total time allocated for endgame
    double time_used;                 // Time spent so far
    int deepest_completed_ply;        // Deepest completed depth
    int32_t value_at_depth[MAX_PLIES]; // Best value found at each depth
    bool value_stable;                 // Has the value been stable across depths?
} EndgameTimeControl;
```

**Per-ply decision**: After each iterative deepening depth completes, decide whether to attempt the next depth:

```c
bool should_try_next_endgame_depth(const EndgameTimeControl *etc) {
    double time_remaining = etc->time_budget - etc->time_used;

    // Estimate time for next depth: typically 3-10x the current depth
    // Use the ratio of the last two completed depths as a predictor
    double branching_factor_estimate = estimate_branching_factor(etc);
    double estimated_next_time = time_for_last_depth * branching_factor_estimate;

    // Don't start a depth we probably can't finish
    // Use a safety factor of 1.5x to account for variance
    if (estimated_next_time * 1.5 > time_remaining) return false;

    // If the value has been stable across the last 2+ depths, the remaining
    // depths are unlikely to change it — stop early
    if (etc->deepest_completed_ply >= 2 && etc->value_stable) return false;

    return true;
}
```

**Integration with the per-ply callback**: MAGPIE's endgame already has `EndgamePerPlyCallback`. The clock manager hooks into this:

```c
void clock_endgame_per_ply_callback(int depth, int32_t value,
                                     const PVLine *pv_line,
                                     const Game *game, void *user_data) {
    EndgameTimeControl *etc = (EndgameTimeControl *)user_data;
    etc->time_used = elapsed_since_start(etc);
    etc->value_at_depth[depth] = value;
    etc->deepest_completed_ply = depth;
    etc->value_stable = is_value_stable(etc);

    if (!should_try_next_endgame_depth(etc)) {
        // Signal the endgame solver to stop
        thread_control_set_user_interrupt(etc->thread_control);
    }
}
```

## Hardware Adaptivity

The system must produce good results on a laptop with 4 cores and on a server with 64 cores, under the same time control. The key insight: **don't hardcode sample counts or node counts — calibrate rates at runtime**.

### Calibration

During the first few turns, measure:

```c
void calibrate(PlayerClock *clock, double movegen_time,
               int sim_samples_completed, double sim_time,
               uint64_t endgame_nodes, double endgame_time) {
    // Exponential moving average for stability
    double alpha = 0.3;
    clock->mean_movegen_time =
        alpha * movegen_time + (1.0 - alpha) * clock->mean_movegen_time;

    if (sim_samples_completed > 0) {
        double rate = sim_time / sim_samples_completed;
        clock->mean_sim_time_per_sample =
            alpha * rate + (1.0 - alpha) * clock->mean_sim_time_per_sample;
    }
    if (endgame_nodes > 0) {
        double rate = endgame_time / endgame_nodes;
        clock->mean_endgame_time_per_node =
            alpha * rate + (1.0 - alpha) * clock->mean_endgame_time_per_node;
    }
}
```

### Using Calibrated Rates

When deciding how many simulation samples to attempt:

```c
uint64_t sim_sample_budget(double time_for_sim, double mean_time_per_sample) {
    if (mean_time_per_sample <= 0) return 10000; // Bootstrap default
    return (uint64_t)(time_for_sim / mean_time_per_sample);
}
```

On a fast machine, `mean_time_per_sample` is small → many samples in the budget → higher confidence. On a slow machine, fewer samples → lower confidence, but the same time is allocated. The BAI convergence detection adapts naturally: with fewer samples, the confidence intervals are wider, and the system is more likely to stop on stable-ranking criteria rather than statistical threshold.

### Thread Scaling

BAI and endgame both scale with thread count. The calibration captures this implicitly: `mean_sim_time_per_sample` on a 64-core machine is lower than on a 4-core machine because BAI runs N threads. No explicit thread count adjustment is needed in the clock manager.

## Time Budget Splitting: Simulation vs Endgame

A turn's time budget must be split between simulation and endgame search. The split depends on the game state:

```c
typedef struct TurnBudget {
    double total;        // Total time for this turn
    double movegen;      // Time for move generation (fixed, measured)
    double simulation;   // Time for BAI simulation
    double endgame;      // Time for endgame search
    double cushion;      // Safety margin (5-10%)
} TurnBudget;

TurnBudget compute_turn_budget(const PlayerClock *clock, int tiles_in_bag,
                               int num_candidate_moves, double total_budget) {
    TurnBudget budget;
    budget.total = total_budget;
    budget.movegen = clock->mean_movegen_time;
    budget.cushion = total_budget * 0.05;
    double available = total_budget - budget.movegen - budget.cushion;

    if (tiles_in_bag == 0) {
        // Pure endgame: all time to search
        budget.simulation = 0;
        budget.endgame = available;
    } else if (tiles_in_bag <= 7) {
        // Near-endgame: mostly endgame, some simulation
        budget.endgame = available * 0.7;
        budget.simulation = available * 0.3;
    } else if (tiles_in_bag <= 14) {
        // Pre-endgame: split
        budget.simulation = available * 0.6;
        budget.endgame = available * 0.4;
    } else {
        // Mid-game: all simulation, no endgame
        budget.simulation = available;
        budget.endgame = 0;
    }
    return budget;
}
```

**Unused simulation time rolls into endgame**: If simulation stops early (stable ranking), the saved time is added to the endgame budget, potentially allowing one more depth of search.

## Estimating Future Time Value

The clock manager needs to compare "value of time now" vs "value of time later." This requires estimating how valuable future time will be.

### Simple Model: Phase-Based Time Value

```c
double estimated_future_time_value(int tiles_in_bag, int tiles_on_rack) {
    // Equity points gained per second of additional thinking, averaged
    // over typical positions at each game phase.
    // Trained from self-play data, or hand-tuned initially.

    int total = tiles_in_bag + tiles_on_rack;
    if (total > 60) return 0.01;  // Opening: 0.01 equity/sec
    if (total > 30) return 0.03;  // Mid-game: 0.03 equity/sec
    if (total > 14) return 0.08;  // Pre-endgame: 0.08 equity/sec
    if (total > 0)  return 0.15;  // Near-endgame: 0.15 equity/sec
    return 0.25;                   // Endgame: 0.25 equity/sec
}
```

These values represent the marginal equity gain from one additional second of thinking at that game phase, averaged across positions. They can be calibrated from self-play data:

1. Play games with varying time budgets per phase
2. For each position, measure the equity difference between the move selected with T seconds vs T+1 seconds
3. Average across positions at each phase

### Advanced Model: Position-Specific (Optional)

Instead of phase-based constants, estimate future time value from features of the current position and expected future positions:

```
future_value = f(turns_remaining, bag_size, avg_complexity, time_remaining)
```

This could be trained via regression on self-play data, but the phase-based model is sufficient to start.

## Turn Lifecycle

Putting it all together, here is the flow for a single turn:

```
1. CLOCK MANAGER: compute_turn_budget()
   - Estimate turns remaining
   - Compute base allocation × complexity × phase multipliers
   - Split into movegen / simulation / endgame / cushion

2. MOVE GENERATION (fixed time)
   - Generate and sort candidate moves by static equity
   - Measure movegen_time for calibration

3. QUICK EXIT CHECK
   - If only 1 legal move (or 1 competitive move), skip simulation
   - If equity gap > 10 points, skip simulation (unless endgame)

4. SIMULATION (time-limited)
   - Set BAI time_limit = budget.simulation
   - Set BAI sample_limit from calibrated rate
   - Run BAI with convergence monitoring
   - On each convergence check:
     a. Extract SimSnapshot from BAI state
     b. Predict expected equity loss (neural net or heuristic fallback)
     c. If should_stop_simulation(): interrupt BAI
   - Measure sim_time and samples for calibration
   - Compute time_saved = budget.simulation - sim_time_used

5. ENDGAME SEARCH (if applicable)
   - budget.endgame += time_saved (roll over unused sim time)
   - Set endgame per-ply callback to clock_endgame_per_ply_callback()
   - Run iterative deepening endgame search
   - Per-ply callback decides whether to attempt next depth
   - Measure endgame_time and nodes for calibration

6. FINAL MOVE SELECTION
   - If endgame completed: use proven best move
   - Else if simulation ran: use BAI best arm
   - Else: use static evaluation best move

7. CALIBRATE
   - Update PlayerClock running statistics
   - Update time_remaining
```

## Edge Cases

### Very Low Time (< 5 seconds)

When time is critically low:
- Skip simulation entirely
- Skip endgame search
- Use static evaluation (move generation is always fast, ~10-50ms)
- The floor allocation ensures at least movegen + tiny margin

### Only One Legal Move

If there is only one legal move (or one non-pass move), play it immediately. Don't waste time on simulation.

### Time Forfeit Risk

Never allocate more than `time_remaining - safety_floor` to a single turn. The safety floor (e.g., 2 seconds) ensures the engine can always complete move generation even if something takes longer than expected.

```c
double safe_budget(double computed_budget, double time_remaining) {
    double safety_floor = 2.0;
    return min(computed_budget, time_remaining - safety_floor);
}
```

### First Turn Calibration

On the very first turn, there are no calibration measurements. Use conservative defaults:
- `mean_movegen_time = 0.1` (100ms)
- `mean_sim_time_per_sample = 0.001` (1ms)
- `mean_endgame_time_per_node = 0.0001` (0.1ms)

These are intentionally rough and will be corrected after the first turn.

## Data Structures

```c
// Expected equity loss predictor (small neural net)
typedef struct EELPredictor {
    int num_layers;
    int layer_sizes[MAX_EEL_LAYERS];  // e.g., {30, 64, 32, 1}
    float *weights;                    // All weights packed contiguously
    float *biases;                     // All biases packed contiguously
} EELPredictor;

// Top-level clock manager
typedef struct ClockManager {
    TimeControl time_control;
    PlayerClock clocks[2];         // One per player
    EELPredictor *eel_model;       // NULL before training (use heuristic fallback)
    // Phase-based time value estimates (trainable)
    double time_value_by_phase[NUM_PHASES];
} ClockManager;

// Per-turn allocation (computed at turn start, updated during turn)
typedef struct TurnAllocation {
    TurnBudget budget;
    SimConvergence sim_convergence;
    EndgameTimeControl endgame_tc;
    Timer turn_timer;
    int player_index;
} TurnAllocation;
```

## Integration Points in MAGPIE

### New Files

- `src/ent/clock_manager.h` — ClockManager, PlayerClock, TimeControl, SimSnapshot structs
- `src/ent/clock_manager.c` — Time allocation, calibration, convergence monitoring
- `src/ent/eel_predictor.h` — EELPredictor struct and inference API
- `src/ent/eel_predictor.c` — Neural net forward pass (no external dependencies)
- `data/eel_model.bin` — Trained model weights (generated by training pipeline)

### Modified Files

- **`src/impl/simmer.c`**: Pass time budget from ClockManager into BAIOptions. Add convergence monitoring callback.

- **`src/impl/endgame.c`**: Add time-aware per-ply callback. The existing `EndgamePerPlyCallback` mechanism requires no structural changes — just a new callback function that checks the time budget.

- **`src/impl/gameplay.c`**: After move generation, consult ClockManager for time allocation. Run simulation and/or endgame within the allocated budget. Select final move.

- **`src/impl/config.c`**: Add TimeControl parameters (initial time, overtime penalty, increment).

### No Changes to Hot Paths

The clock manager operates **between** phases (before simulation, between endgame plies), not during the inner loops. Move generation, BAI sampling, and negamax search are unchanged. The only additions are:
- BAI receives a time limit (already supported)
- Endgame receives a per-ply callback (already supported)
- A convergence check runs every N simulation samples (lightweight: neural net forward pass <1μs)

## Training

### EEL Predictor Neural Net

The expected equity loss predictor is the most novel component. Training it requires generating ground-truth equity loss data from extended simulation.

**Data generation pipeline**:

1. Collect a diverse set of positions from self-play (varying game phases, score differentials, rack compositions)
2. For each position:
   a. Run move generation to get candidate moves
   b. Run simulation for a large budget (e.g., 100K samples) to establish ground truth — the final move ranking and mean equities after 100K samples
   c. At multiple checkpoints during the simulation (100, 200, 500, 1K, 2K, 5K, 10K, 20K, 50K samples):
      - Record the SimSnapshot feature vector
      - Record which move would be chosen if stopping here
      - Record the equity of that move vs the ground-truth best move → realized equity loss
   d. Repeat with different random seeds (the draw order and simulation rollouts vary)
   e. Average the realized equity loss across seeds at each checkpoint → expected equity loss

3. This produces training examples: `(SimSnapshot features) → EEL in centipoints`

**Scale**: ~50K positions × 9 checkpoints × 5 seeds = ~2.25M training examples. Each requires only bookkeeping during a simulation run — the simulation work is shared across all checkpoints for a given position.

**Architecture**: Small feedforward network, 2-3 hidden layers of 32-64 neurons. Input is ~30 features (see SimSnapshot above). Output is a single scalar (EEL in centipoints). ReLU activations, trained with MSE loss. The network is tiny — inference is <1μs and the model file is <50KB.

**Validation**: Hold out 20% of positions. Verify that predicted EEL correlates well with actual equity loss. The model doesn't need to be perfectly calibrated — it just needs to rank "worth continuing" vs "safe to stop" correctly.

### Time Value Parameters

**From Self-Play Data**:

1. At each game phase, play positions with different time budgets (e.g., 0.5s, 1s, 2s, 5s, 10s, 30s)
2. Record the equity of the selected move at each budget
3. Compute marginal equity gain: `ΔE/Δt` at each phase
4. These become the `time_value_by_phase` parameters

**From Game Pairs**:

1. Play A vs B, where:
   - A uses clock management (allocates time dynamically)
   - B uses uniform allocation (equal time per turn)
   - Both have the same total time budget
2. Measure win rate differential
3. Iterate on the allocation policy parameters

**Across Hardware**:

Run the same training on different hardware. The calibration handles hardware differences automatically (via measured rates), but the time value parameters should be hardware-independent since they measure "equity per second of search" which scales naturally with hardware speed — faster hardware finds the same improvements in less wall time, but the allocation ratios remain the same.

## Implementation Phases

### Phase 1: Game Clock Infrastructure

- Add TimeControl and PlayerClock data structures
- Track time remaining per player
- Compute base time allocation per turn (proportional + reserve)
- Wire time limit into BAI via existing `time_limit_seconds`
- **Effort**: ~2 days

### Phase 2: Complexity and Phase Multipliers

- Compute equity gap and competitive move count after movegen
- Implement phase-based multiplier (tiles remaining)
- Implement turn budget splitting (simulation vs endgame)
- **Effort**: ~1-2 days

### Phase 3: Calibration

- Measure movegen time, sim sample rate, endgame node rate per turn
- Exponential moving average for stability
- Use calibrated rates for budget → sample/node count conversion
- **Effort**: ~1 day

### Phase 4: Diminishing Returns (Heuristic Fallback)

- Add SimConvergence monitoring and SimSnapshot extraction to BAI integration
- Implement heuristic EEL estimator (two-arm normal approximation)
- Implement should_stop_simulation() with future time value comparison
- Add time-aware endgame per-ply callback
- Unused simulation time rolls into endgame budget
- This provides a working system before the neural net is trained
- **Effort**: ~3 days

### Phase 5: EEL Training Data Generation

- Instrument simulation to record SimSnapshot features at multiple checkpoints
- Run extended simulation (100K samples) on ~50K diverse positions
- Record realized equity loss at each checkpoint × multiple random seeds
- Aggregate into training dataset: (SimSnapshot → EEL)
- **Effort**: ~3-5 days (mostly compute time)

### Phase 6: EEL Neural Net Training

- Train small feedforward network on EEL data
- Validate on held-out positions
- Export model weights for C inference (no framework dependency)
- Integrate neural net inference into should_stop_simulation()
- **Effort**: ~2-3 days

### Phase 7: Time Value Training

- Instrument self-play to record move quality vs time budget at each phase
- Compute marginal time value curves
- Fit time_value_by_phase parameters
- Validate via game pairs (managed vs uniform allocation)
- **Effort**: ~3-5 days

### Phase 8: Tournament Testing

- Test under standard tournament time controls (25 min, 10 pt/min penalty)
- Test under rapid time controls (10 min, 15 min)
- Test on different hardware (laptop, desktop, server)
- Verify no time forfeits, verify time is used productively
- **Effort**: ~2-3 days

## Relationship to Other Systems

### Dynamic Leaves (REQ)

Full KLV recomputation takes time. The clock manager should account for this:
- If dynamic leaves are enabled and time permits, recompute the KLV before simulation
- If time is short, use static KLV
- The recomputation cost (~1-100ms depending on bag size) is included in the movegen phase of the turn budget

### SPV (Positional Heuristic)

SPV adds negligible time to move evaluation (~8 table reads per move). No clock management interaction needed.

### Endgame + Simulation Interaction

In the pre-endgame (bag nearly empty), the optimal strategy may be:
1. Simulate top-N moves to narrow candidates
2. For each top candidate, attempt endgame search at the resulting position
3. Select the move whose resulting endgame is best for us

This "sim then solve" approach requires careful time splitting. The clock manager allocates a combined budget and lets each phase draw from it, with simulation going first and endgame getting whatever remains.
