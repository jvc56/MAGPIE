# Data & Formats

> **Status:** outline — prose to be written.

<!-- NOTE: precise file-format specs are a hallmark of authoritative engine
     docs (cf. wolges' KWG/KLV docs, Macondo's CGP spec). Each page should give:
     purpose, on-disk/in-memory layout (byte fields, a table or diagram), how
     it's produced (which maker/converter), and how it's consumed. -->

## The data MAGPIE loads

<!-- NOTE: map the `data/` subdirs to formats:
       lexica/            → .txt, .kwg, .klv, .wmp
       layouts/           → board layout files
       letterdistributions/ → CSV tile distributions
       strategy/          → win% lookup tables for simulation
     When specifying data on the command line, use the basename (no extension). -->

| Format | File | Purpose | Page |
| ------ | ---- | ------- | ---- |
| KWG | `.kwg` | Word graph (DAWG + GADDAG) | [KWG](kwg.md) |
| KLV | `.klv` | Leave values | [KLV](klv.md) |
| WMP | `.wmp` | Word map (anagram lookup) | [WMP](wmp.md) |
| GCG | `.gcg` | Recorded game | [GCG](gcg.md) |
| CGP | *(string)* | Single position | [CGP](cgp.md) |
| LD / layout | `.csv` / layout | Tiles & board | [Lexicon & Board Data](lexicon-data.md) |

## The conversion pipeline

<!-- NOTE: text → kwg → wmp (and klv). The `convert` command (text2kwg,
     text2wordmap) and convert_lexica.sh. Detail lives on the format pages and
     in formats/lexicon-data.md. -->
