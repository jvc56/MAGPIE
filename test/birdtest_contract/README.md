# birdtest contract fixtures

Copied verbatim from birdtest's `contract-fixtures/` -- every JSON file, byte
for byte (this README is MAGPIE's own); birdtest's CI replaces this
directory's JSON with its current set before running `magpie_test
contribute`. The worker API is a cross-repo boundary, and these are the
server's half of it: `test/contribute_test.c` asserts that

- every key `config_contribute_*` and `contribute_claim_task` read is present
  in each assignment fixture (`assignment-*.json`, `anon-uuid-assignment.json`,
  `expected-data.json`, `heartbeat.json`),
- the serializers a task's result is built with still produce every key the
  result fixtures carry (`result-*.json`),
- the claim body has the keys of `claim-request.json`, its `board_dim` and
  `rack_size` this build's `BOARD_DIM` and `RACK_SIZE`,
- the decline body, built for the files `decline-missing-data.json` names,
  has its keys and values (a file not found leaves `actual` out, which the
  server reads as the fixture's `null`), and
- each `shutdown-*.json` reason is waited out or obeyed as intended,

so a key renamed on either side fails a test here rather than a
contributor's run.

The `result-*`, `heartbeat`, `expected-data`, `anon-uuid-assignment` and
`assignment-game-pairs` fixtures were captured from a real exchange between
this client and a birdtest server by birdtest's `scripts/capture_contract.py`
(see birdtest's `contract-fixtures/README.md`), not written by hand.

When birdtest changes a fixture, copy the new file here in the same change.
