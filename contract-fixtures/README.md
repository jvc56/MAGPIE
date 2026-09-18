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
| `claim-request.json` | client → server | The required claim body: version and unsupported set |
| `assignment-games.json` | server → client | A games assignment with `expected_data`, two lexicons |
| `assignment-leave-generation.json` | server → client | Generation 1, reading the server-built zeroed KLV |
| `decline-missing-data.json` | client → server | A decline naming a missing file and a mismatched one |
| `shutdown-data-out-of-date.json` | server → client | Every job unreachable because the data is stale |
| `shutdown-magpie-too-old.json` | server → client | Every job unreachable because the build is old |
| `shutdown-both.json` | server → client | Both, leading with the MAGPIE version |

The digests here are the real ones from `data-20251004.tgz`, so a fixture that
stops matching what an import produces is itself a signal.
