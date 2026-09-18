# birdtest contract fixtures

Copied verbatim from birdtest's `contract-fixtures/`. The worker API is a
cross-repo boundary, and these are the server's half of it: `test/contribute_test.c`
asserts that every key `config_contribute_*` reads is present in them, so a key
renamed on either side fails a test here rather than a contributor's run.

When birdtest changes a fixture, copy the new file here in the same change.
