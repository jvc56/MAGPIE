# Data formats and conversions

The [README](../README.md#data) describes MAGPIE's data directory. This page
covers building its derived files by hand, and the compact codecs.

`help convert` lists every conversion.

## Rack info tables and rack-equity KLVs

A rack info table is built from a KLV and a wordmap, and stores each full
rack's precomputed leave values. It therefore belongs to a *(lexicon, leaves)*
pair rather than to a lexicon: CSW24 played with `CSW_quackle_leaves` and CSW24
played with its own leaves need different tables. `klvwmp2rit` takes the KLV's
and the wordmap's names separately so the output does not have to borrow one of
theirs:

```
magpie> convert klvwmp2rit CSW24.CSW_quackle_leaves english CSW_quackle_leaves CSW24
```

Both extra names are optional and default to the output's, so the short form
still works where a table really is named after its lexicon:

```
magpie> convert klvwmp2rit CSW24
```

`rackequity2klv` turns a generation's aggregated full-rack results into the KLV
they imply, which is the other half of a leave-generation run when the games
were played somewhere else. It reads `lexica/<name>.csv`, one
`rack,count,equity_sum` row per full rack:

```
magpie> convert rackequity2klv gen1 english
```

Every full rack the distribution draws must appear exactly once. A rack with a
count of zero is not the same as a missing rack — it was enumerated and never
drawn, and contributes a mean of zero at full draw weight, exactly as it does
inside a `leavegen` run — so a file that omits one is refused rather than having
its gaps valued at zero. This is what birdtest's server uses to build each
generation's KLV.

## Lexica from text files

`text2kwg` and `text2wordmap` build a lexicon's KWG and wordmap from
`lexica/<name>.txt`, one uppercase word per line. The words do not need to be in
any particular order: the convert commands sort them before building. That
matters because the DAWG builder requires lexicographic order and does not check
for it, and unordered input silently produces a much larger graph encoding the
same words. The README's [Comparing lexica](../README.md#comparing-lexica)
example uses them.

## Compact position and game codecs

The codecs and word-frequency trainer are C-only and add no build dependencies.
Use `make word_frequencies BUILD=no_pgo_release` to build the training tool.
Use `conv counts2xsw` to create word codebooks, `conv cgp2xsp` / `xsp2cgp` for
positions, and `conv gcg2xsg` / `xsg2gcg` for games. See
[XS codecs](xs-codecs.md) for commands, immutable data installation, wire
formats, and compatibility tests.
