# Worker API contract fixtures

The worker API is a cross-repo boundary: birdtest serves it, MAGPIE's
`contribute` command speaks it, and the two are released independently. Nothing
else pins the contract — it exists implicitly in
[`routes/worker.rs`](../backend/src/routes/worker.rs) and MAGPIE's
`src/impl/contribute.c` agreeing.

These files are the cheap version of fixing that: one committed example of each
message either side has to produce or read. Copy them into both repositories
and have each side's tests parse them. A field renamed on one side and not the
other then fails a test rather than a contributor's run.

| File | Direction | What it pins |
|---|---|---|
| `claim-request.json` | client → server | The required claim body: version, the build's board dimension and rack size, and unsupported set |
| `assignment-games.json` | server → client | A games assignment with `expected_data`, two lexicons, a player 1 with a rack info table and a word info table (all three derived files pinned), and a player 2 that solves its endgame and a nested pre-endgame (every solver key stated) |
| `assignment-opening-rack.json` | server → client | A rack batch for a simming player: the one job type whose request carries `racks` and a single `player` |
| `assignment-leave-generation.json` | server → client | Generation 1, reading the server-built zeroed KLV, and the static `player` the bot plays both seats as |
| `decline-missing-data.json` | client → server | A decline naming a missing file and a mismatched one |
| `decline-time-limit.json` | client → server | A task stopped at the assignment's `max_task_seconds`: the token and `"reason": "time_limit"`, no `missing` |
| `shutdown-data-out-of-date.json` | server → client | Every job unreachable because the data is stale |
| `shutdown-magpie-too-old.json` | server → client | Every job unreachable because the build is old |
| `shutdown-both.json` | server → client | Both, leading with the MAGPIE version |
| `shutdown-unsupported-build.json` | server → client | A build whose `BOARD_DIM` or `RACK_SIZE` is not the 15 and 7 every job plays with |
| `assignment-game-pairs.json` | server → client | A `game_pairs` assignment (told apart from games by its `job_type` alone) whose players ask for a wordmap, capturing only first divergences (`capture_first_divergence`) |
| `anon-uuid-assignment.json` | server → client | The first assignment of a worker with no identity: a games task carrying the minted `worker_uuid`, and a job that pins no derived file (`derived: []`) |
| `expected-data.json` | server → client | The `expected_data` digest list of an assignment, input files and a derived wordmap |
| `heartbeat.json` | client → server | A heartbeat |
| `result-games.json` | client → server | A games result with `capture_positions` on, from two players that solve their endgames and small pre-endgames: the tally and every captured position, each with its `analysis` (`static`, `peg`, `endgame`) and a solved one's spreads and depths |
| `result-games-inference.json` | client → server | A games result from simming players that infer: a first-turn position with no inference, and a later one with its `inference` (leaves found, draws, mean equity, the most drawn leaves), each move with its win% and per-ply statistics. Written by hand in MAGPIE's key layout; MAGPIE's test checks its own output carries every key |
| `result-game-pairs.json` | client → server | A pairs result: the tally, the pentanomial, the divergent subset, and each diverging pair's two positions at its first divergence |
| `result-opening-rack.json` | client → server | A simulating player's rack analyses, with win%, blended utility and per-ply statistics |
| `result-leave-generation.json` | client → server | Every rack a leave-generation task saw, on MAGPIE's two-letter test distribution |

The digests here are the real ones from `data-20260925.tgz`, so a fixture that
stops matching what an import produces is itself a signal.

## What every assignment states

Beside `claim_token`, `job_id`, `min_magpie_version`, `expected_data` and the
`task_request`, every assignment carries:

- `job_name` -- a string, never empty: the job's name, or for a job created
  without one its type and the start of its id (`"games job 1d4a7f60"`). What
  the worker calls the job when it says what it is running.
- `max_task_seconds` -- a whole number, 60 to 86,400: how long the worker may
  run this task. It is the server's setting (`/admin/settings`) as it stood
  when the claim was made, and the claim's deadline is its claim time plus
  this. A worker that reaches it stops the task, hands it back unfinished and
  declines it with `"reason": "time_limit"` (`decline-time-limit.json`). A
  minute past the deadline the server takes the claim back whether or not the
  worker still heartbeats, and answers a result for it `{"accepted": false}`.

A `games` or `game_pairs` request also states `threading_mode`, after
`sim_cutoff`: `"igp"` (the default) gives all of a task's threads to one game's
simulation at a time, which makes an iteration-bounded simulation
reproducible, and `"pgp"` plays the batch's games in parallel. It is the job's
setting, and matters only when a player simulates. Opening-rack and leave
requests state none.

A decline's `reason` is one of `missing_data`, `magpie_version`,
`unknown_job_type`, `derived_mismatch`, `task_failed` and `time_limit`;
anything else is a `400` listing them.

## Capturing

The first nine, `decline-time-limit.json` and `result-games-inference.json`
were written by hand. The rest were **captured from a real exchange**, though
`assignment-game-pairs.json` and `anon-uuid-assignment.json` were given
`job_name`, `max_task_seconds` and `threading_mode` by hand when the server
began sending them, until the next recapture: `scripts/capture_contract.py` is a recording proxy that sits
between `magpie contribute` and the backend, forwards everything unchanged, and
writes the first body of each message type here. Nothing is normalised --
tokens, ids and timings are the ones that crossed the wire, so a recapture
changes them -- and the JSON is pretty-printed with two-space indentation.

Recapture with a real stack and a real MAGPIE (a `portable_release` build and a
`download_data.sh` install):

    MAGPIE_ROOT=../MAGPIE scripts/e2e_magpie_native.sh --cases capture --capture-out contract-fixtures

or, against a compose stack already up, `python3 scripts/e2e_magpie.py --magpie
../MAGPIE/bin/magpie --magpie-root ../MAGPIE --cases capture --capture-out
contract-fixtures --github-fixture-port 8481`, with the backend's
`GITHUB_API_URL`/`GITHUB_RAW_URL` pointing at that port (the leave-generation
result runs on the small test distribution that script serves). The `capture`
case creates one job of each type, runs a contributor through the proxy, and
fails unless every fixture above was captured. Then copy the directory into
MAGPIE's `test/birdtest_contract/` in the same change.

`backend/src/routes/worker.rs` (`mod contract_fixtures`) decodes each
client → server fixture into the type that handles it and runs a result through
its job type's validation, as a submission would be; server → client fixtures
are compared with what the wire types serialize by field structure. MAGPIE's
`test/contribute_test.c` checks the other half: every key its client reads is
in the assignments, its result serializers produce every key in the results,
and the claim and decline bodies it builds match `claim-request.json` (with
this build's board and rack size) and `decline-missing-data.json` key for key.
