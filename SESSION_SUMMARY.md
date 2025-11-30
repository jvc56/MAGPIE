# Inference Cutoff Optimization Benchmarking Session Summary

## The Goal
Run benchmarks comparing inference performance with and without the cutoff optimization, for both WMP (Word Move Prune) and non-WMP (GADDAG) move generation modes.

## What Went Well

### 1000-game WMP benchmark completed successfully
Results saved to `INFERENCE_BENCHMARK_RESULTS.md`:
- 1.24x overall speedup
- 1.54x speedup for scoring plays
- 90.56% cutoff trigger rate

### 100-game non-WMP benchmark completed
Added to the markdown:
- 1.33x overall speedup
- 2.02x speedup for scoring plays
- Exchanges are ~4x slower without WMP

## The Difficulties

### 1. Initial confusion about test speed (non-WMP)
- Started 300-game non-WMP test, seemed impossibly slow
- You suspected debug build, but it was actually release (`-O3 -flto -march=native`)
- Real issue: exchanges without WMP are extremely slow (~22-24 seconds each vs ~5-7s with WMP)
- Also: progress logging only happens every 100 games, so it looked stuck

### 2. Reduced to 5 games, then 100 games
- 5 games worked, validated the test was functional
- 100 games took ~7 minutes total, gave us good non-WMP baseline data

### 3. Scaling to 1000 games non-WMP
- You requested 1000 games for statistical significance
- Estimated 70-90 minutes runtime

### 4. Auto-commit setup complications
- You wanted results committed and pushed so you could see them while AFK
- Initial script had HEREDOC issues (wouldn't expand file contents properly)
- Rewrote as proper shell script in `/private/tmp/commit_results.sh`
- Had to verify git permissions would work from background shell
- Added `git push` after you reminded me you need remote access

## Current State
- **Test running**: 1000-game non-WMP benchmark (ID: ece454)
- **Auto-commit+push script running**: (ID: 1d67ea) - waits for "Overall:" in output, then commits and pushes
- **Output file**: `/private/tmp/infercmp_1000games_nowmp.txt`
- **Branch**: `infer-cutoff-test-fix`

## How to Check Results Later
```bash
# On GitHub: check the latest commit on infer-cutoff-test-fix branch
# Or locally:
git fetch && git log origin/infer-cutoff-test-fix -1 --format="%B"
```

## Date
November 30, 2025
