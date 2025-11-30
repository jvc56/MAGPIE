# Gemini Advice and Instructions for Claude

## Current Status: Debugging Release-Mode Divergence

We are investigating a regression in the **Inference Exchange Cutoff Optimization** that manifests **only in Release builds** (`BUILD=release`). The optimization (specifically the fallback path for blank tiles) is finding *more* valid racks than the baseline, implying it is missing a refutation (likely the matching Exchange move) that the baseline finds.

### The Problem
*   **Symptom:** `bin/magpie_test infercmp` fails with `num_samples mismatch: 8004811 vs 7991815` (Optimized > Baseline).
*   **Context:** Occurs in `Release` build. Passes in `Debug` (implied).
*   **Isolated Case:** We isolated a deterministic failure: Game 1, Seed 13345.
    *   **Rack:** `CDNVVZ?` (Has blank).
    *   **Move:** Exchange 5 tiles.
    *   **Baseline:** Says INVALID (correctly identifying the exchange matches target or equity logic).
    *   **Optimized (Fallback):** Says VALID (incorrectly preferring a scoring play?).

### File System State
*   **`src/impl/gameplay.c`**:
    *   I added a `printf` in the `has_blank` fallback block of `get_top_equity_move_with_exchange_cutoff` to inspect the `top_move` it finds.
    *   I changed `target_leave_size_for_exchange_cutoff` to `-1` in this block to match Baseline defaults perfectly. **This did not fix the issue.**
*   **`test/infer_test.c`**:
    *   Added `test_infer_cutoff_repro` which runs the failing Game 1 case isolated.
    *   Reduced `test_infer_cutoff_optimization_comparison` to 5 games for speed.

### Immediate Action Required
The last `make` command was cancelled. You need to run it to see the debug output.

1.  **Run the Test:**
    ```bash
    make magpie_test BUILD=release && ./bin/magpie_test infercmp
    ```
2.  **Analyze Output:**
    *   Look for `Fallback Debug:` output.
    *   See what `TopType`, `TopEq`, and `Match` status the Optimized path sees.
    *   If `TopType` is NOT Exchange, but Baseline finds Exchange (inferred from mismatch), why is Optimized missing it?
    *   If `TopEq` differs from what Baseline would see (requires assumption or further instrumentation), why?

### Hypotheses
1.  **Sort Instability:** `generate_moves` might produce ties between the Exchange and a Scoring Play. In Release mode, the sort order might differ or `move_list_get_move(0)` might return a different move if ties aren't broken deterministically.
2.  **Uninitialized Memory:** `MoveGen` reuse or struct initialization might have a field that is uninitialized in Release mode, affecting logic.
3.  **Optimization/UB:** The compiler might be optimizing away something in `generate_moves` or `gameplay.c` due to Undefined Behavior (e.g., strict aliasing) that we haven't spotted.

### Next Steps
1.  Run the debug command.
2.  If Optimized sees a Scoring Play as top move, check why Baseline sees Exchange (you might need to add printfs to `get_top_equity_move` in `gameplay.c` too).
3.  Fix the divergence (likely by enforcing deterministic sort or fixing initialization).