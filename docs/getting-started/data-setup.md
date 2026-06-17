# Data Setup

> **Status:** outline — prose to be written.
> **Sources:** `download_data.sh`, `convert_lexica.sh`, `setup.sh`
> **Related:** [Lexicon & Board Data](../formats/lexicon-data.md)

## Downloading the data

<!-- NOTE: `./download_data.sh` pulls data/ and testdata/ from the MAGPIE-DATA
     repo. Both dirs are gitignored. "Missing file" errors usually mean this
     wasn't run. -->

## Converting lexica

<!-- NOTE: `./convert_lexica.sh` builds MAGPIE then converts .kwg → .txt → .wmp.
     Explain why both .kwg and .wmp exist. -->

## The `data/` directory

<!-- NOTE: four subdirs. Always reference data by basename (no extension) on the
     command line. -->

| Subdirectory | Contents |
| ------------ | -------- |
| `layouts/` | Board layout files (start + bonus squares) |
| `letterdistributions/` | Tile frequency/score CSVs |
| `lexica/` | `.txt`, `.kwg`, `.klv`, `.wmp` |
| `strategy/` | Win-percentage tables for simulation |

## Default letter distributions by lexicon prefix

<!-- NOTE: table from the README — CSW/NWL/OSPD/... → english, RD → german,
     NSF → norwegian, DISC → catalan, FRA → french, OSPS → polish, DSW → dutch.
     Setting a lexicon auto-selects the matching distribution. -->
