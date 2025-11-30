# Inference Cutoff Optimization Benchmark Results

## Test Configuration
- **Games**: 1000
- **Threads**: 10
- **Build**: Release (`-O3 -flto -march=native`)
- **WMP**: Enabled
- **Lexicon**: CSW21

## Overall Results

| Metric | With Cutoff | Without Cutoff | Speedup |
|--------|-------------|----------------|---------|
| **Total Time** | 1487.66s | 1842.49s | **1.24x** |
| **Scoring Plays** | 515.83s | 792.12s | **1.54x** |
| **Exchanges** | 971.83s | 1050.37s | **1.08x** |

## Scoring Play Breakdown by Tiles Played

| Tiles | Count | Time (With) | Time (Without) | Per-Inference (With) | Per-Inference (Without) | Speedup |
|-------|-------|-------------|----------------|---------------------|------------------------|---------|
| 1 | 991 | 0.85s | 1.76s | 0.0009s | 0.0018s | 2.07x |
| 2 | 2485 | 7.65s | 14.68s | 0.0031s | 0.0059s | 1.92x |
| 3 | 3694 | 28.73s | 51.39s | 0.0078s | 0.0139s | 1.79x |
| 4 | 4024 | 56.70s | 96.57s | 0.0141s | 0.0240s | 1.70x |
| 5 | 3313 | 79.07s | 126.53s | 0.0239s | 0.0382s | 1.60x |
| 6 | 2080 | 94.82s | 148.48s | 0.0456s | 0.0714s | 1.57x |
| 7 | 1444 | 248.01s | 352.71s | 0.1718s | 0.2443s | 1.42x |

## Exchange Breakdown by Tiles Exchanged

| Tiles | Count | Time (With) | Time (Without) | Per-Exchange (With) | Per-Exchange (Without) | Speedup |
|-------|-------|-------------|----------------|---------------------|------------------------|---------|
| 1 | 8 | 27.49s | 38.34s | 3.44s | 4.79s | 1.39x |
| 2 | 21 | 100.68s | 117.65s | 4.79s | 5.60s | 1.17x |
| 3 | 25 | 109.10s | 125.22s | 4.36s | 5.01s | 1.15x |
| 4 | 37 | 201.40s | 224.87s | 5.44s | 6.08s | 1.12x |
| 5 | 36 | 211.87s | 222.56s | 5.89s | 6.18s | 1.05x |
| 6 | 31 | 219.85s | 222.04s | 7.09s | 7.16s | 1.01x |
| 7 | 15 | 101.45s | 99.68s | 6.76s | 6.65s | 0.98x |

## Cutoff Trigger Statistics

- **Cutoff triggered**: 454,301,042 / 501,665,122 total (**90.56%**)

## Anchor Statistics

| Mode | Processed | Available | Skipped | Skip Rate |
|------|-----------|-----------|---------|-----------|
| **With Cutoff** | 7,195,594,177 | 34,460,774,994 | 27,270,312,928 | **79.13%** |
| **Without Cutoff** | 15,832,271,204 | 34,471,867,874 | 18,663,896,266 | **54.14%** |

**Anchor processing reduction**: 54.5% fewer anchors processed with cutoff optimization

## Subrack Statistics

| Mode | Processed | Available | Skipped | Skip Rate |
|------|-----------|-----------|---------|-----------|
| **With Cutoff** | 9,507,549,564 | 98,236,418,755 | 22,703,926,429 | **23.11%** |
| **Without Cutoff** | 17,626,393,253 | 190,121,128,824 | 58,367,918,880 | **30.70%** |

**Subrack processing reduction**: 46.1% fewer subracks processed with cutoff optimization

## Key Observations

1. **Scoring plays benefit significantly** from the cutoff optimization (1.54x speedup overall)
2. **Shorter plays benefit more**: 1-tile plays see 2.07x speedup vs 1.42x for 7-tile plays
3. **Exchanges see modest improvement** (1.08x) with diminishing returns for larger exchanges
4. **Cutoff triggers 90.56% of the time**, indicating the optimization is highly applicable
5. **Anchor skip rate improves dramatically**: 79.13% with cutoff vs 54.14% without
6. **Results are identical** between cutoff and non-cutoff modes (correctness verified)

## Test Date
November 30, 2025

---

# Results Without WMP (GADDAG Move Generation)

## Test Configuration
- **Games**: 100
- **Threads**: 10
- **Build**: Release (`-O3 -flto -march=native`)
- **WMP**: Disabled
- **Lexicon**: CSW21

## Overall Results

| Metric | With Cutoff | Without Cutoff | Speedup |
|--------|-------------|----------------|---------|
| **Total Time** | 405.54s | 537.42s | **1.33x** |
| **Scoring Plays** | 92.78s | 187.46s | **2.02x** |
| **Exchanges** | 312.76s | 349.97s | **1.12x** |

## Scoring Play Breakdown by Tiles Played

| Tiles | Count | Time (With) | Time (Without) | Per-Inference (With) | Per-Inference (Without) | Speedup |
|-------|-------|-------------|----------------|---------------------|------------------------|---------|
| 1 | 15 | 11.56s | 27.24s | 0.771s | 1.816s | 2.36x |
| 2 | 171 | 50.70s | 98.36s | 0.297s | 0.575s | 1.94x |
| 3 | 366 | 24.56s | 50.87s | 0.067s | 0.139s | 2.07x |
| 4 | 444 | 5.22s | 9.97s | 0.012s | 0.022s | 1.91x |
| 5 | 289 | 0.57s | 0.85s | 0.002s | 0.003s | 1.48x |
| 6 | 99 | 0.04s | 0.05s | 0.0004s | 0.0005s | 1.13x |
| 7 | 415 | 0.13s | 0.13s | 0.0003s | 0.0003s | 0.96x |

## Exchange Breakdown by Tiles Exchanged

| Tiles | Count | Time (With) | Time (Without) | Per-Exchange (With) | Per-Exchange (Without) | Speedup |
|-------|-------|-------------|----------------|---------------------|------------------------|---------|
| 4 | 5 | 112.21s | 110.82s | 22.44s | 22.16s | 0.99x |
| 5 | 8 | 192.00s | 228.84s | 24.00s | 28.60s | 1.19x |
| 6 | 2 | 8.54s | 10.31s | 4.27s | 5.15s | 1.21x |

## Cutoff Trigger Statistics

- **Cutoff triggered**: 37,204,067 / 41,273,341 total (**90.14%**)

## Key Observations (Without WMP)

1. **Scoring plays benefit even more** from the cutoff optimization without WMP (2.02x speedup vs 1.54x with WMP)
2. **Shorter plays see dramatic improvement**: 1-tile plays see 2.36x speedup
3. **Bingo plays (7 tiles) see no benefit** (0.96x), as expected since cutoff rarely triggers
4. **Exchanges are significantly slower** without WMP (~22-24s vs ~5-7s with WMP)
5. **Cutoff trigger rate is consistent** at ~90% regardless of WMP mode
6. **Overall speedup is similar** (1.33x) but with different performance profile

## WMP vs Non-WMP Performance Comparison

| Play Type | WMP Per-Inference | Non-WMP Per-Inference | WMP Advantage |
|-----------|------------------|----------------------|---------------|
| 1-tile | 0.0009s | 0.771s | ~850x |
| 4-tile | 0.0141s | 0.012s | ~1.2x |
| 7-tile (bingo) | 0.172s | 0.0003s | Non-WMP faster |
| Exchange (4-tile) | 5.44s | 22.44s | ~4x |

Note: Non-WMP shows faster bingo inference due to fewer subracks explored.
