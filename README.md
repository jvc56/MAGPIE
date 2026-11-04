# MAGPIE

**Macondo Accordant Game Program and Inference Engine**

<table align="center"><tr>
<td align="center"><a href="#windows"><img alt="Windows" src="https://img.shields.io/badge/Windows-x64%20%7C%20ARM-0078D4?style=for-the-badge&logo=data:image/svg%2bxml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCI+PHBhdGggZmlsbD0id2hpdGUiIGQ9Ik0wIDBoMTEuNHYxMS40SDB6TTEyLjYgMEgyNHYxMS40SDEyLjZ6TTAgMTIuNmgxMS40VjI0SDB6TTEyLjYgMTIuNkgyNFYyNEgxMi42eiIvPjwvc3ZnPg=="></a><br><a href="https://github.com/jvc56/MAGPIE/actions/workflows/release-windows.yml"><img alt="Windows build" src="https://img.shields.io/github/actions/workflow/status/jvc56/MAGPIE/release-windows.yml?label=build&style=flat-square"></a></td>
<td align="center"><a href="#macos"><img alt="macOS" src="https://img.shields.io/badge/macOS-universal-000000?style=for-the-badge&logo=apple&logoColor=white"></a><br><a href="https://github.com/jvc56/MAGPIE/actions/workflows/release-macos.yml"><img alt="macOS build" src="https://img.shields.io/github/actions/workflow/status/jvc56/MAGPIE/release-macos.yml?label=build&style=flat-square"></a></td>
<td align="center"><a href="#linux"><img alt="Linux" src="https://img.shields.io/badge/Linux-x64%20%7C%20arm64-FCC624?style=for-the-badge&logo=linux&logoColor=black"></a><br><a href="https://github.com/jvc56/MAGPIE/actions/workflows/release-linux.yml"><img alt="Linux build" src="https://img.shields.io/github/actions/workflow/status/jvc56/MAGPIE/release-linux.yml?label=build&style=flat-square"></a></td>
<td align="center"><a href="https://magpie.birdtest.org"><img alt="Browser" src="https://img.shields.io/badge/Browser-magpie.birdtest.org-654FF0?style=for-the-badge&logo=webassembly&logoColor=white"></a><br><a href="https://github.com/jvc56/MAGPIE/actions/workflows/test-wasm.yml?query=branch%3Amain"><img alt="WASM tests" src="https://img.shields.io/github/actions/workflow/status/jvc56/MAGPIE/test-wasm.yml?label=wasm&branch=main&style=flat-square"></a></td>
</tr></table>

<p align="center"><a href="https://github.com/jvc56/MAGPIE/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/jvc56/MAGPIE?style=flat-square&label=latest"></a> <a href="https://github.com/jvc56/MAGPIE/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/jvc56/MAGPIE/total?style=flat-square"></a> <a href="https://github.com/jvc56/MAGPIE/actions/workflows/run-tests.yml?query=branch%3Amain"><img alt="CI" src="https://img.shields.io/github/actions/workflow/status/jvc56/MAGPIE/run-tests.yml?label=ci&branch=main&style=flat-square"></a> <a href="LICENSE.md"><img alt="License" src="https://img.shields.io/github/license/jvc56/MAGPIE?style=flat-square"></a></p>

MAGPIE is a crossword game playing and analysis program. It generates and
simulates moves, infers what an opponent kept, solves pre-endgames and
endgames, annotates and analyzes whole games, and plays itself to compare and tune
strategies.

MAGPIE started as a C rewrite of [Macondo](https://github.com/domino14/macondo)
but has since incorporated a variety of new features, algorithms, and data
structures. See [Acknowledgements](#acknowledgements).

## Contents

- [Features](#features)
- [Installing MAGPIE](#installing-magpie)
- [Quick start](#quick-start)
- [Analyzing or Annotating a Game](#analyzing-or-annotating-a-game)
- [Self-play and training](#self-play-and-training)
- [Lookup tables](#lookup-tables)
- [Data](#data)
- [Scripting and embedding](#scripting-and-embedding)
- [Contributing](#contributing)
- [Acknowledgements](#acknowledgements)
- [Citing MAGPIE](#citing-magpie)
- [License](#license)

## Features

- **Move generation**: lists every play a rack can make for a given position.
- **Simulation**: plays moves forward over many possible draws.
- **Inference**: works out what the opponent probably kept, from the play
  they just made.
- **Pre-endgame solving**: with 1 to 4 tiles left in the bag, finds the play
  that wins against the most possible draws.
- **Endgame solving**: with the bag empty, finds the best sequence of plays
  for both sides.
- **Game annotation and analysis**: loads games from cross-tables, woogles.io
  or GCG files, steps through them, records plays and notes, and reviews every
  turn of a game.
- **Autoplay**: has MAGPIE play itself, to measure which of two strategies,
  leave values or lexica is stronger.
- **Leave generation**: works out how much each set of kept tiles is worth,
  for any lexicon.
- **A C library** for building MAGPIE into other programs.
- Boards of 15x15 and 21x21, and lexica in English, French, German, Catalan,
  Dutch, Polish, and more.

## Installing MAGPIE

Ready-made executables are published for Windows, macOS and Linux on the
[Releases](https://github.com/jvc56/MAGPIE/releases) page. They need nothing
else installed and download MAGPIE's data (about 195 MB) on first run.

**Building from source gives the best performance.** The published
executables are built for a fixed, widely supported instruction set so that
they run on any machine of their platform. `./scripts/setup.sh` instead builds
for the exact processor that compiles it and adds profile-guided optimization.
You can build from source natively on Linux (x86_64 or arm64) and macOS (Apple
silicon or Intel). There is no native Windows build: the Windows executables
are cross-compiled from Linux or macOS. On a Windows PC, you can build and run
the Linux version inside WSL. See [Compile from source](#compile-from-source).

### Windows

Windows 10 (version 1903 or later) and Windows 11, on x64 or ARM, run MAGPIE
either installed or as a single `.exe` with nothing else to install.

**To install it**, download `magpie-<version>-windows-setup.exe` from the
[Releases](https://github.com/jvc56/MAGPIE/releases) page and run it. Like the
executables, it is not code-signed, so Windows shows **Windows protected your
PC**: click **More info**, then **Run anyway**. It installs MAGPIE for you
alone, with no administrator needed, and picks the x64 or ARM build for your PC.
It adds MAGPIE to the Start menu and, if you tick the option, to your PATH, so
that any terminal can run it as `magpie`. On its first start from the Start
menu, MAGPIE offers to download its data into `%LOCALAPPDATA%\MAGPIE\data`, and
it finds the data there wherever you start it from afterwards. Uninstall it from
**Settings > Apps**. That asks whether to delete the data as well.

**To run it without installing:**

1. From the [Releases](https://github.com/jvc56/MAGPIE/releases) page, download
   `magpie-<version>-windows-x64.exe` (most PCs) or
   `magpie-<version>-windows-arm64.exe` (Windows on ARM, such as Snapdragon
   laptops), and put it in its own folder.
2. Double-click the `.exe`. The release is not code-signed, so the first time
   Windows shows **Windows protected your PC**: click **More info**, then
   **Run anyway**. (If your antivirus flags the file, please
   [open an issue](https://github.com/jvc56/MAGPIE/issues); unsigned programs
   that download files are sometimes misidentified.)
3. MAGPIE looks for its data in a `data` folder beside it and offers to
   download it: answer `y`.

To run it from a terminal, open PowerShell or Command Prompt in its folder and
run `.\magpie-<version>-windows-x64.exe`, with commands as arguments if you
like.

<details>
<summary>Checking the download (optional)</summary>

Checking the download is optional. Every file is published with a `.sha256`
file beside it. In PowerShell:

```
Get-FileHash .\magpie-<version>-windows-x64.exe -Algorithm SHA256
```

The hash printed must match the one in the `.sha256` file.

</details>

### macOS

macOS 11 or later, on Apple silicon or Intel, runs MAGPIE as one universal
executable, either installed or with nothing else to install. Either way,
MAGPIE downloads its data into `~/.magpie/data` and finds it there regardless
of which folder it starts in.

**To install it**, download `magpie-<version>-macos.pkg` from the
[Releases](https://github.com/jvc56/MAGPIE/releases) page and open it. The
installer is not signed or notarized, so macOS refuses to open it at first:
close the warning, then click **Open Anyway** in **System Settings > Privacy &
Security**. It puts `magpie` in `/usr/local/magpie` and links it into
`/usr/local/bin`, so a new Terminal window can run it as `magpie`. On its first
run, MAGPIE offers to download its data into `~/.magpie/data`.

**To run it without installing:**

1. From the [Releases](https://github.com/jvc56/MAGPIE/releases) page,
   download `magpie-<version>-macos-universal.zip` and open it. It unzips to a
   folder holding `magpie`. Move the folder wherever you like, say your home
   folder.
2. The executable is not signed or notarized, so macOS refuses to run it at
   first. To allow it, either run `xattr -d com.apple.quarantine magpie` in its
   folder once, or try to run it and then click **Open Anyway** in **System
   Settings > Privacy & Security**.
3. Double-click `magpie` in Finder, which opens it in Terminal, or run
   `./magpie` in its folder in Terminal. MAGPIE looks for its data in a `data`
   folder in the working directory, then in one beside the executable, then in
   `~/.magpie/data`. If it finds none, it offers to download it into
   `~/.magpie/data`: answer `y`.

<details>
<summary>Checking the download (optional), and uninstalling</summary>

Checking the download is optional. Every file is published with a `.sha256`
file beside it. In Terminal:

```
shasum -a 256 magpie-<version>-macos-universal.zip
```

The hash printed must match the one in the `.sha256` file.

To uninstall the `.pkg`, run `sudo rm -rf /usr/local/magpie /usr/local/bin/magpie`
and `sudo pkgutil --forget com.github.jvc56.magpie`, plus `rm -rf ~/.magpie` to
delete the data.

</details>

### Linux

Linux on x86_64 or arm64, with glibc 2.35 or later (Ubuntu 22.04, Debian 12,
Fedora 36 and newer), runs MAGPIE as one executable without building it.

1. From the [Releases](https://github.com/jvc56/MAGPIE/releases) page,
   download `magpie-<version>-linux-x64.tar.gz` (most PCs) or
   `magpie-<version>-linux-arm64.tar.gz` (ARM, such as a Raspberry Pi 4 or 5 on
   a 64-bit system), and unpack it with `tar -xzf`. It unpacks to a folder
   holding `magpie`. Move the folder wherever you like.
2. `cd` to its folder and run `./magpie`. MAGPIE looks for its data in a `data`
   folder there, then in one beside the executable, and offers to download it
   into the first: answer `y`. The download uses libcurl (`libcurl.so.4`), which
   most systems have; without it, run `./scripts/download_data.sh` from a clone
   of this repository instead.

<details>
<summary>Checking the download (optional)</summary>

Checking the download is optional. Every file is published with a `.sha256`
file beside it:

```
sha256sum -c magpie-<version>-linux-x64.tar.gz.sha256
```

</details>

### Compile from source

MAGPIE builds natively on Linux (x86_64 or arm64) and macOS (Apple silicon or
Intel). You need a C compiler (GCC or Clang), `make`, `bash`, `curl`, `tar` and `git`.
For the fastest build it also needs Clang 15 or newer and `llvm-profdata`,
for profile-guided optimization; without them it builds a plain optimized
MAGPIE.

Clone the repository and run the setup script, which switches the clone to
`main`, pulls, downloads the data, builds the wordmaps and builds
`bin/magpie`:

```
git clone https://github.com/jvc56/MAGPIE.git
cd MAGPIE
./scripts/setup.sh
./bin/magpie
```

To update, run `./scripts/setup.sh` again. Other builds (portable builds, 21x21
boards, the Windows and universal macOS executables, focused profiles for
long-running jobs) are covered in [docs/building.md](docs/building.md).

### In the browser

A browser version of MAGPIE runs at
[magpie.birdtest.org](https://magpie.birdtest.org).

### Data on first run

MAGPIE's data (lexica, leaves, board layouts, letter distributions and win
percentage tables) is a separate, versioned download of about 195 MB. The
released executables offer to download it on first run, and `data fetch`
downloads or updates it at any time:

```
magpie> data fetch
```

The first time you use
a lexicon, MAGPIE builds that lexicon's wordmap, which takes a few seconds and
makes move generation faster afterwards. See [Lookup tables](#lookup-tables).

## Quick start

Start MAGPIE with no arguments (`./bin/magpie` in a source build or `magpie` if it
was installed as an executable) and it waits for commands at a `magpie>` prompt. Here
are some example commands:

```
magpie> set -lex CSW24
magpie> new my_game
magpie> rgs AEINRST -tlim 10
magpie> t
magpie> help
```

This sets the lexicon, starts a game saved as `my_game.gcg`, generates the
moves for the rack AEINRST and simulates them for 10 seconds, commits the best
of them once the simulation finishes, and lists every command by task.

## Analyzing or Annotating a Game

This section explains how MAGPIE's commands work, then walks through the
commands for setting up, studying and recording a game.

### Commands, settings and help

MAGPIE reads commands at the `magpie>` prompt. Settings, which start with `-`, change how commands work:
any number of them can follow any command, and they persist between commands until
changed. `set` changes settings without doing anything else.

```
magpie> set -lex CSW24 -threads 8
magpie> gs -plies 3 -tlim 30
magpie> gs
```

Here the second `gs` also simulates 3 plies deep for 30 seconds, on 8 threads.
Settings ending in `1` or `2`, such as `-tl1`, apply to one player only.

Commands and settings can be written as any unambiguous prefix of their names,
so `generate` can be `generat`, `gen` and so on, and the most common commands
have shortcuts, shown in parentheses in the tables below: `g` for `generate`,
`s` for `shgame`.

`help` lists every command by task. `help <command>` gives a command's usage
and the settings it uses, and `help <setting>` shows what a setting does and which
commands use it. `help options` lists every setting, and `help setup`,
`help tables`, `help display` and `help session` the ones shared across
commands.

```
magpie> help
magpie> help simulate
magpie> help plies
```

In the default asynchronous mode, while a command runs, the prompt still
accepts `status` (`sta`), which prints its progress, `stop` (`sto`), which
ends it with the results so far, and `shgame` (`s`). In
[sync mode](#sync-mode), `status` and `stop` are ignored.

```
magpie> gs
sta
(... current sim results ...)
sto
(... final sim results ...)
magpie>
```

By default MAGPIE forgets its settings when it exits. Start it with
`-savesettings true` (for example `./bin/magpie set -savesettings true`) to
load them from `settings.txt` in the current directory on startup and save
them there after every successful command. MAGPIE ignores `settings.txt` on any startup
without `-savesettings true`.

### Setting up a game

| Command                    | What it does                                                               |
| -------------------------- | -------------------------------------------------------------------------- |
| `set -lex <lexicon>`       | Sets the lexicon, and with it the default letter distribution.             |
| `newgame` (`new`) `<name>` | Starts a new game, saved as `<name>.gcg`.                                  |
| `load` (`lo`) `<source>`   | Loads a game from cross-tables, woogles.io, a GCG URL or a local GCG file. |
| `cgp` (`cg`) `<cgp>`       | Loads a position written as a CGP.                                         |
| `p1`, `p2` `<name>`        | Names the first and second players.                                        |
| `switchnames` (`sw`)       | Swaps the players' names.                                                  |
| `rack` (`r`) `<tiles>`     | Sets the rack of the player on turn; `?` or `.` is a blank.                |
| `rrack` (`rr`)             | Gives the player on turn a random rack.                                    |

To annotate a game as you go, start a new one. By default a name is needed,
because MAGPIE saves the game after every change (see `help autosavegcg`):

```
magpie> set -lex CSW24
magpie> new logan_vs_richards
magpie> p1 Adam Logan
magpie> p2 Nigel Richards
magpie> r EEIJNP?
```

To let `newgame` start a game without a name, set `-fgrequired false`.

To study a game already played, load it. The source can be a cross-tables game
ID or URL, a woogles.io game ID or URL, the URL of a GCG file, or a local GCG
file. When the GCG names its lexicon, MAGPIE uses it; otherwise set `-lex`
first.

```
magpie> load 54938
magpie> load https://woogles.io/game/XuoAntzD
magpie> load my_game.gcg
```

To study a single position, load it as a CGP:

```
magpie> cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20
```

### Navigating

| Command                            | What it does                                                     |
| ---------------------------------- | ---------------------------------------------------------------- |
| `goto` (`go`) `<turn>\|start\|end` | Goes to the position after the given turn.                       |
| `next` (`n`)                       | Goes one move forward, skipping challenge bonus events.          |
| `previous` (`p`)                   | Goes one move back.                                              |
| `shgame` (`s`)                     | Shows the board, racks, scores, unseen tiles and the last event. |

```
magpie> goto start
magpie> goto 10
magpie> n
magpie> p
magpie> goto end
```

Going back and committing a different move replaces the rest of the game from
that point.

### Generating moves

| Command                                | What it does                                                       |
| -------------------------------------- | ------------------------------------------------------------------ |
| `generate` (`g`)                       | Generates the moves for the rack on turn, ranked by static equity. |
| `rg` `<rack>`                          | Sets the rack, then generates.                                     |
| `addmoves` (`a`) `<move>, <move>, ...` | Adds moves of your own to the list.                                |
| `shmoves` (`shm`)                      | Shows the move list, or the sim results when there are some.       |
| `threats` `[<rack>\|-] [<count>]`      | Lists the opponent's highest-scoring plays from the unseen tiles.  |

```
magpie> r EEIJNP?
magpie> g
magpie> a 8D JEEPs, 8H JEEP
magpie> shm 10
magpie> shm 8D
magpie> shm JEEP
```

`shmoves` takes a count, a coordinate, a word prefix, a coordinate and a
prefix together, or `-` for exchanges and passes. `-numplays` sets how many
moves generation keeps.

`threats` shows what the opponent could score next: the highest-scoring plays
the player not on turn could make from every tile the player on turn cannot
see, the opponent's rack and the bag, playing up to a rack of them. Like
`simulate`, it takes the opponent's known rack from the game unless given one;
it then lists only the plays a rack holding those tiles could make, and `-`
means none of their rack is known. It shows `-maxnumdplays` threats, or as many
as you ask for, up to 100, as `shmoves` shows moves but with only the `Move`
and `Sc` columns. It works with any number of tiles in the bag, and supports
only the classic game variant.

```
magpie> threats
magpie> threats 70
magpie> threats ABC
magpie> threats ABC 10
magpie> threats -
magpie> threats - 50
```

### Simulating

| Command                                          | What it does                                          |
| ------------------------------------------------ | ----------------------------------------------------- |
| `simulate` (`sim`) `[<opponent rack>]`           | Simulates the moves in the list.                      |
| `gsimulate` (`gs`)                               | Generates, then simulates.                            |
| `rgsimulate` (`rgs`) `<rack>`                    | Sets the rack, generates, then simulates.             |
| `snoprune` (`sn`) `<moves>`                      | Simulates without ever pruning the given moves.       |
| `heatmap` (`hea`) `<play> [<ply>] [all\|bingos]` | Shows where a simulated play's rollouts placed tiles. |

```
magpie> gs
magpie> sim -
magpie> rgs RETINAS -plies 3 -tlim 30
magpie> sn 8D.JEEPINg,8H.JEEPINg
```

A simulation plays each candidate forward for `-plies` turns (5 by default)
over many random draws, spending its iterations on the plays still in
contention, and stops at `-tlim` seconds (60 by default), at `-iterations`, or
when it is `-scondition` percent sure of the best play. It uses the opponent's
known rack from the game history unless given one; `-` forces a random one.
With `-sinfer true`, the default, it first infers what the opponent kept and
draws their racks from that.

In asynchronous mode, `status` and `stop` check on and end a running
simulation.

To record heat maps, set `-useheatmap true` before simulating. `heatmap` then
takes the play's rank in the sim results, optionally which rollout ply to show
(1 by default), and optionally the type: `all` (the default) counts every tile
placed on that ply, `bingos` only the tiles of bingos. The type can be shortened
to any prefix, such as `b`.

```
magpie> heatmap 1
magpie> heatmap 1 3
magpie> heatmap 1 bingos
magpie> heatmap 1 3 all
magpie> heatmap 2 4 b
```

### Inferring the opponent's rack

| Command               | What it does                                             |
| --------------------- | -------------------------------------------------------- |
| `infer` (`in`)        | Infers what the opponent kept after their previous play. |
| `shinference` (`shi`) | Shows the last inference again.                          |

```
magpie> load 54938
magpie> goto 3
magpie> infer
magpie> infer Seth EY 33
magpie> infer Emmett 3
```

With no arguments, `infer` uses the previous play in the game history: it
finds every rack the opponent could have held for which that play was within
`-imargin` equity (5 points by default) of their best, and reports how likely
each tile and leave is. The other forms name the player and describe a play
(its tiles and score) or an exchange (the number of tiles) by hand.

### Pre-endgames

The `peg` command solves a pre-endgame (1 to 4 tiles in the bag): for each
candidate move it enumerates the possible bag/opponent-rack orderings, solves
the resulting endgames, and reports each move's wins, draws, losses and win
percentage. `shpeg` (`shpe`) shows the last result again, and the untruncated
chart is written to `data/pegcharts/`.

```
magpie> load 54938
magpie> goto 21
magpie> peg -pegtlim 10
...
Best Move       A7 LODE  100.0%  (72 W, 0 T, 0 L)
Depth  Rank  Move         W  T   L   Win%  Spread  Time  Outcomes
6-ply     1  A7 LODE     72  0   0  100.0   +39.9  0.7s  always wins
6-ply     2  A7 LOTA     72  0   0  100.0   +31.2  0.9s  always wins
6-ply     3  A7 LADE     72  0   0  100.0   +29.2  0.8s  always wins
```

By default the `peg` command only cares about wins, ties, or losses: a win by 1 is as
good as a win by 100. Every candidate is solved, each endgame searched 3 plies
deep (`-pegplies`), and a candidate is cut once its losses and draws show it
cannot reach the best one. A line still going at that depth is finished by a
greedy playout, so the results are estimates; `-pegplies end` searches every
endgame to the end of the game, which makes the best move and its win% exact.
Cut candidates show the most they could have reached (`<=`), and spreads are
not shown. The opponent is **pessimistic**: wherever it replies, it plays as if
it could see your rack and the bag order. Tiles the opponent is known to hold,
on its rack in the position or returned there after a challenged-off phony, are
kept on its rack in every scenario. This means that by default, the `peg` will
only look for guaranteed wins.

The first argument chooses the candidates, and an optional second one lists
moves to add to them and keep through every stage. Moves are comma-separated
with no spaces -- coordinate and tiles joined by a period, pass as
`pass`:

```
magpie> peg                          # plays that leave at most 1 tile in the bag
magpie> peg all                      # every move
magpie> peg empty                    # every play that empties the bag
magpie> peg leaves2 13L.ONYX         # plays that leave at most 2 tiles, plus 13L ONYX
magpie> peg only 13L.ONYX,13L.OXY    # exactly these moves
```

A bag of 5 to 7 tiles is accepted when every candidate leaves at most 4 of
them.

The settings that matter most (`help peg` describes them all):

- `-pegplies <plies|end>` sets how deep the complete solve searches each
  endgame (default 3); `end` searches to the end of the game.
- `-pegtlim <seconds>` gives the solve a time limit, which switches it to the
  **staged cascade**: stage 0 greedy-evaluates every candidate, and each
  halving stage keeps the best of them and re-ranks them at one more ply of
  endgame depth. It returns the best answer it has when the limit hits, from
  the last completed stage; a play the limit left unscored shows `-`. An
  explicit `-pegtopk` also runs the cascade.
- `-pegtopk <count1>,<count2>,...` sets the cascade's per-stage counts
  (default `32,16,8,4,2`); `all` (or `0`) keeps every candidate in one deep
  stage, and `default` undoes an earlier `-pegtopk`.
- `-pegpess false` switches to a **rational** opponent, which chooses its
  reply from what it can see; its choice is then scored on the actual racks.
- `-pegndepth`, `-pegncaps` and `-pegnstrides` set how far, how wide and how
  densely the nested pre-endgames of later turns are searched. A complete
  solve nests until the game resolves, over every candidate and scenario.
- `-pegstride <n>` samples ~1/n of the scenarios for bag >= 3 in the cascade
  (faster, approximate).
- `-threads <n>` is a pure speedup at the same answer (only which
  candidates are cut, and the bounds they show, can vary).

### Endgames

| Command                                      | What it does                                                           |
| -------------------------------------------- | ---------------------------------------------------------------------- |
| `endgame` (`en`)                             | Solves the endgame on turn: the bag is empty and both racks are known. |
| `endgame <position> <tiles>`, `endgame pass` | Also evaluates the given move, even outside the top results.           |
| `shendgame` (`she`)                          | Shows the last endgame result again.                                   |

```
magpie> load 54938
magpie> goto 22
magpie> endgame -etopk 3
Endgame (depth: 6, time: 0.209s, status: finished)
Play        V   S   Turn 2
15D KYE 20  -23 -57 B4 TAPIR 17 (INQR +26)
15E QI 16   -25 -59 B4 TAPIR 17 (EKNRY +24)
B13 YEN 23  -28 -62 B4 TAPIR 17 (IKQR +34)
```

`-eplies` sets the search depth (6 by default), `-etlim` a time limit in
seconds, and `-etopk` how many moves are shown with their full principal
variations. The search is multithreaded across `-threads` and keeps a
transposition table of `-ttfraction` of the machine's memory (0.25 by
default).

### Recording plays, notes and challenges

| Command                                      | What it does                                                                                                  |
| -------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `commit` (`c`)                               | Commits a move: by its number in the list, by position and tiles, `ex <tiles>`, or `pass`.                    |
| `tcommit` (`t`) `[<rack>]`                   | Commits the best move: by win% when there are sim results, otherwise, or when given a rack, by static equity. |
| `note` (`no`) `<text>`                       | Adds a note to the most recently added move.                                                                  |
| `cnote` (`cn`) `<move> <text>`               | Commits a move and adds a note to it in one command.                                                          |
| `challenge` (`ch`)                           | Challenges the previous play.                                                                                 |
| `unchallenge` (`un`)                         | Removes the challenge bonus from the previous play.                                                           |
| `overtimepenalty` (`ov`) `<player> <points>` | Adds an overtime penalty, once the game is over.                                                              |
| `export` (`e`) `[<file>]`                    | Saves the game as a GCG file.                                                                                 |

```
magpie> c 1
magpie> c 11d ANTIQuES
magpie> c ex ABC
magpie> t
magpie> cnote 2 should have played $1
magpie> chal
magpie> unchal
magpie> e logan_vs_richards_annotated
```

Lowercase letters are blanks. A note can use interpolation to name moves from the current move list
by number: `$1` is the first, `$2` is the second, and so on. `challenge` removes the previous play if it
formed a phony and otherwise adds the challenge bonus. The `challenge` command preserves the subsequent game history and updates the scores accordingly. The `unchallenge` command takes the
bonus off again without affecting later moves. With `-autosavegcg true`, the
default, the game is saved after every change, so `export` is only needed to
save it under another name.

### Analyzing a whole game

`analyze` (`an`) evaluates every turn of a game and writes the results to
`<game>_report.txt`. A turn is simulated, solved with `peg` when 1 to
`-apegmaxbag` tiles are in the bag (1 by default), or solved as an endgame when
the bag is empty. With `-plies 0` every turn gets a static evaluation instead,
which is much faster.

```
magpie> analyze                       # the game loaded now
magpie> analyze my_game.gcg
magpie> analyze games/                # every GCG in a directory
magpie> analyze my_game.gcg Alice     # only Alice's turns
```

`-mistakes true` grades each turn as a small, medium or large mistake, and
`-aegspread true` solves endgame turns with a full window, so that spread, not
just the result, is exact.

## Self-play and training

These commands have MAGPIE play itself, to measure which of two strategies,
leave values or lexica is stronger, and to train new values it can use later. They
can run for hours or days.

### Autoplay

`autoplay` plays MAGPIE against itself and records the results. It is the way
to compare two strategies, two sets of leaves or two lexica:

```
magpie> set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1
magpie> autoplay games 1000 -gp true -threads 8 -seed 42 -hr true
```

Settings ending in `1` and `2` configure each player separately. With game
pairs (`-gp true`), each pair of games uses identical tile draws with the
players alternating who goes first, which greatly reduces statistical noise;
`games 1000` then plays 2000 games. See `help gp`.

#### Timed play with PlayChooser

Autoplay can use `PlayChooser` for either or both players. `-pc1` and `-pc2`
set each player's total per-game clock in milliseconds; `-1` disables it (the
default), and `0` enables it without a clock. For example, this gives both
players a 30-second clock:

```
magpie> autoplay games 100 -pc1 30000 -pc2 30000 -hr true
```

PlayChooser selects its evaluation mode from the position, using simulation,
pre-endgame, or endgame analysis as appropriate. Timed games deduct 10 points
per started minute of overtime by default. The penalty and period are
configurable independently, so blitz runs can deduct one point per started
second:

```
magpie> autoplay games 100 -pc1 5000 -pc2 5000 -otpenalty 1 -otperiod 1000 -hr true
```

When at least one player uses PlayChooser, the final report includes clock
usage, overtime, and deducted penalty points for each active player.

Games with a clocked player are **not reproducible from the seed**. How much
search fits in the budget varies with machine load, thread count, and build, so
the same `-seed` will not replay the same moves, and the overtime penalty itself
varies between runs. This is inherent to timed play rather than a defect, but it
is worth knowing because the rest of autoplay _is_ seed-deterministic. The
intended output is the aggregate: win percentage, spread, and penalty points
over enough games. Individual games are not meant to be replayed.

For the same reason, game pairs (`-gp`) lose their variance-reduction property
when combined with a clock. Pairs work by having both games see identical draws
so that only the players' decisions differ; with timing on, the two games can
diverge for reasons unrelated to strategy.

How much search a clock buys also depends on the threading mode, so the same
`-pc1` value is not comparable across `-mtmode` settings:

- Per-game parallelism (`-mtmode pgp`, the default) runs `-threads` games at
  once and gives each game's PlayChooser a single thread. Raising `-threads`
  finishes the run sooner but does not give any player more search per second
  of clock — until the thread count exceeds the machine's available cores, at
  which point games contend for CPU and the same budget buys measurably less.
- Intra-game parallelism (`-mtmode igp`) plays one game at a time and gives
  that game's PlayChooser every thread, so the same clock buys roughly
  `-threads` times as much search per move.

Compare timed runs only against other runs with the same `-mtmode` and
`-threads`.

#### Solving the end of the game

Without a clock, an autoplay player can still solve the end of the game.
`-eplies1`/`-eplies2` have that player solve the endgame to the given depth
once the bag is empty, and `-pegbag1`/`-pegbag2` have it solve the
pre-endgame while the bag holds 1 to that many tiles (at most 4). Every other
turn is played as before, statically or by simulation:

```
magpie> autoplay games 100 -eplies1 6 -pegbag1 2 -hr true
```

`-eplies1 0` (the default) turns both solvers off, since the pre-endgame
solver scores its emptier scenarios with endgame solves. The pre-endgame's
schedule is `-pegtopk1`, `-pegstride1`, `-pegpess1` and `-pegnested1` (and
their `2` forms), and `-pegncaps`, `-pegndepth` and `-pegnstrides` for its
nested lookahead, which set both players. Unlike `-eplies` and `-pegtopk`,
which configure the `endgame` and `peg` commands, these change how the
player plays.

No time limit applies: the depth and the schedule bound the work, so a player
is as strong on a slow machine as on a fast one. Every solve uses all of
`-threads`, whatever `-mtmode` gives a simulation. A solve is multithreaded,
so these games are not reproducible from the seed either. Cost grows steeply
with the bag: at a bag of 4 with the default schedule, a single game can take
minutes.

#### Comparing lexica

To play two lexica against each other to see which is stronger, create the
lexical data from text files and run autoplay. First, set the letter
distribution:

```
magpie> set -ld english
```

then convert the text files to the KWG and WMP. The following example assumes
text files called `CSW50.txt` and `CSW60.txt` are saved to `./data/lexica`:

```
magpie> convert text2kwg CSW50
magpie> convert text2wordmap CSW50
magpie> convert text2kwg CSW60
magpie> convert text2wordmap CSW60
```

Both text files must contain one word per line in all uppercase. The words do
not need to be in any particular order: the convert commands sort them before
building. Once converted, run autoplay:

```
magpie> autoplay games 10000 -l1 CSW50 -l2 CSW60 -leaves CSW21 -gp true -hr true -pfreq 10000
```

### Generating leaves

The leave values that static equity uses can be generated for any lexicon in
two ways, and blended:

- `leavegen` plays generations of self-play games and derives leave values from each of them. Depending on your hardware, this can take days or weeks.
- `census` values every leave exactly on each of many boards against the
  leaves of the generation before.
- `leaveblend` mixes two leave tables leave by leave. The census is about as
  strong as `leavegen` alone; the blend of the two is stronger than either.

Evaluate the result with autoplay and game pairs. `help leavegen`,
`help census` and `help leaveblend` give the details.

### Contributing compute to birdtest

`contribute` lends this machine to [birdtest](https://github.com/jvc56/birdtest)
by running its server's tasks: it claims tasks, runs them, and submits the
results until the server has no more work. Learn more at https://birdtest.org.

## Data

MAGPIE gets its data from the
[MAGPIE-DATA](https://github.com/jvc56/MAGPIE-DATA) repository. It lives in a
`data` directory, whose four subdirectories `data fetch` and
`scripts/download_data.sh` fill from MAGPIE-DATA; `peg` adds a fifth,
`pegcharts`. All lexical, board layout, and strategy data must be saved in
their respective directories for MAGPIE to find them. When specifying input data in MAGPIE, always use the basename without
the file extension. `-path` sets the data directories, in order of precedence.

### layouts

This directory contains the layout files which specify the start square and
bonus squares for the board. By default, it contains `standard15` and `standard21`. The start square is
denoted by the 0-indexed `row, column` integers on the first row, followed by the board
layout bonus squares. Only the following bonus squares are valid:

- ` ` (no bonus)
- `'` (double letter)
- `-` (double word)
- `"` (triple letter)
- `=` (triple word)
- `^` (quadruple letter)
- `~` (quadruple word)
- `#` (brick, an unplayable square)

The height and width of the board are the compile time constant `BOARD_DIM`;
see [docs/building.md](docs/building.md#board-and-rack-size).

### letterdistributions

This directory contains the letter distribution CSV files which specify the
frequency, score, and display of each tile. The format is:

```
<uppercase_letter>,<lowercase_letter>,<frequency>,<score>,<is_vowel>,[<fullwidth_uppercase_letter>,<full_width_lowercase_letter>]
```

The full width display characters can be optionally specified at the end of the
row. The `_super` distributions are for the 21x21 board. Setting a new lexicon
with the `lexicon` setting will set a default letter distribution if the
lexicon name has a known prefix:

- CSW, NWL, TWL, OSPD, OSW, America, CEL -> english
- RD -> german
- NSF -> norwegian
- DISC -> catalan
- FRA -> french
- OSPS -> polish
- DSW -> dutch

### lexica

This directory contains the following file types:

- `.kwg` (Kurnia Word Graph, the lexicon, courtesy of
  [wolges](https://github.com/andy-k/wolges))
- `.klv2` (KWG that stores leave values, courtesy of
  [wolges](https://github.com/andy-k/wolges)); the `_super21` leaves are for
  the 21x21 board
- `.txt` (plain text lexica, one word per line)
- `.wmp` (word maps), `.rit` (rack info tables) and `.wit` (word info tables),
  built from the above; see [Lookup tables](#lookup-tables)

The data set includes English (CSW, NWL, TWL), French (FRA20), German (RD29),
Catalan (DISC2), Dutch (DSW25) and Polish (OSPS49) lexica.

### strategy

This directory contains win percentage tables used in Monte Carlo simulations,
one per letter distribution, named `winpct_<letter_distribution>.csv` (for
example `winpct_english.csv`, `winpct_french.csv`, `winpct_english_super.csv`).
Simulations use the table for the current letter distribution unless the
`winpct` setting names another; `set -winpct default` returns to the
distribution's table. A table must cover the distribution's bag, so for example
the English table cannot be used with a 102-tile French bag.

Each table is keyed by the game state (the number of tiles in the bag and on
each rack) and records, for every state, how the on-turn player's spread
changed by the end of self-play games. That gives both the chance of winning
from any spread and the expected final margin, which the `smargin` setting can
use to project simulation results to the end of the game.

To make a table, record self-play games with the `winpct` autoplay recorder,
which writes `<lexicon>_winpct_record.csv` and adds to it on later runs, then
smooth the record into a table with the `winpct` convert command:

```
magpie> autoplay winpct 1000000 -lex CSW24 -threads 8
magpie> convert winpct CSW24_winpct_record winpct_english
```

The recorder splits games into two independent samples, and `convert winpct`
chooses how much to smooth by predicting each sample from the other; it prints
that comparison.

### pegcharts

`peg` writes each solve's untruncated chart here, as
`outcomes_<timestamp>.txt`.

## Scripting and embedding

### Script mode

Given a command other than `set` or `cgp` on the command line, MAGPIE runs it
and exits:

```
./bin/magpie analyze my_game.gcg -lex CSW24
```

A `set` or `cgp` command, or the `-mode` setting, starts the interactive
prompt instead.

### Sync mode

At the interactive prompt, MAGPIE runs in asynchronous mode by default, which
accepts `status` and `stop` while a command runs. With `-mode sync`, the
prompt instead waits for each command to finish before reading the next, and
ignores `status` and `stop`:

```
./bin/magpie set -mode sync
```

To send many commands from another program, use sync mode rather than
starting MAGPIE once per command in script mode, which pays the startup cost
every time, or use the C library.

### The C library

Programs can embed MAGPIE through a string -> string C API. Build the shared
library with

```
make libmagpie
```

which produces `bin/libmagpie.so` (`bin/libmagpie.dylib` on macOS) exporting
only the `magpie_*` functions declared in `src/impl/cmd_api.h` — that header,
its dependency `src/def/cmd_api_defs.h`, and the shared library form the
embedding surface. When packaging the headers, retain their `impl/` and `def/`
sibling directories so the relative include resolves.

The API is command-oriented: callers create an opaque `Magpie` handle, pass it
the same command strings the console accepts, and read the resulting output
back as a string. Output is machine readable by default; the
`*_human_readable` function variants format it for display instead. Commands
can run synchronously (`magpie_run_sync`) or on a background thread
(`magpie_run_async`), with `magpie_get_last_command_status_message` for mid-run
progress, `magpie_stop_current_command` to interrupt, and `magpie_await` to
collect the result. All returned strings are owned by the caller and freed with
`magpie_free_string`.

See `examples/generate_moves.c` for a minimal C client (build it with
`make examples`) and `examples/magpie.py` for a Python ctypes wrapper with an
interactive REPL.

## Contributing

Issues and pull requests are welcome! [AGENTS.md](AGENTS.md) describes the
code's conventions and every check CI runs (formatting, cppcheck, clang-tidy,
portability, the Windows cross-compile and the unit tests), with the commands
to run them locally. Build the tests with `make magpie_test` and run a suite
with `./bin/magpie_test <name>`, for example `./bin/magpie_test movegen`.

More documentation lives in [docs/](docs/): [building](docs/building.md),
[data formats](docs/data-formats.md) and the [XS codecs](docs/xs-codecs.md).

## Acknowledgements

MAGPIE started as a C rewrite of [Macondo](https://github.com/domino14/macondo)
by [César Del Solar](https://github.com/domino14). It uses several concepts
originally developed by [Andy Kurnia](https://github.com/andy-k) in
[wolges](https://github.com/andy-k/wolges): shadow playing, the KWG and KLV
data structures, and the leave census.

Thanks to:

- everyone who has [contributed](https://github.com/jvc56/MAGPIE/graphs/contributors);
- Marc Jourdan, who kindly provided the code for the best-arm identification
  paper below;
- [cross-tables](https://www.cross-tables.com) and
  [woogles.io](https://woogles.io), whose games `load` reads;
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), which builds the
  Windows executables.

### Research

MAGPIE implements or builds on the following work:

- Marc Jourdan, Rémy Degenne and Emilie Kaufmann. _Dealing with Unknown
  Variances in Best-Arm Identification._ ALT 2023.
  [arXiv:2210.00974](https://arxiv.org/abs/2210.00974). The simulation's
  best-arm identification.
- Wei You, Chao Qin, Zihao Wang and Shuoguang Yang. _Information-Directed
  Selection for Top-Two Algorithms._ COLT 2023.
  [arXiv:2205.12086](https://arxiv.org/abs/2205.12086), with code in
  [topk_colt23](https://github.com/zihaophys/topk_colt23). The simulation's
  top-two sampling rules.
- Steven A. Gordon. _A Faster Scrabble Move Generation Algorithm._ Software:
  Practice and Experience 24(2), 1994. The GADDAG, which move generation
  searches.
- Alejandro González Romero, René Alquézar Mancho, Arturo Ramírez Flores,
  Francisco González Acuña and Ian García Olmedo. _El método de anagramas:
  un rápido y novedoso algoritmo para generar jugadas de Scrabble._ Research
  in Computing Science 147(6), 2018, pp. 41–55.
  [Paper](https://rcs.cic.ipn.mx/2018_147_6/El%20metodo%20de%20anagramas_%20un%20rapido%20y%20novedoso%20algoritmo%20para%20generar%20jugadas%20de%20scrabble.pdf).
  The anagram-based move generation that inspired MAGPIE's word map (WMP).
- Jean-Christophe Weill. _The ABDADA Distributed Minimax Search Algorithm._
  ICCA Journal 19(1), 1996; also in the proceedings of ACM CSC '96,
  [doi:10.1145/228329.228345](https://doi.org/10.1145/228329.228345). The
  parallel endgame search.
- Albert L. Zobrist. _A New Hashing Method with Application for Game Playing._
  Technical Report 88, University of Wisconsin, 1970.
  [minds.wisconsin.edu](https://minds.wisconsin.edu/handle/1793/57624). The
  endgame and pre-endgame transposition tables' keys.
- David Blackman and Sebastiano Vigna. _Scrambled Linear Pseudorandom Number
  Generators._ ACM Transactions on Mathematical Software 47(4), 2021.
  [doi:10.1145/3460772](https://doi.org/10.1145/3460772),
  [arXiv:1805.01407](https://arxiv.org/abs/1805.01407). The xoshiro random
  number generator.
- Guy L. Steele Jr., Doug Lea and Christine H. Flood. _Fast Splittable
  Pseudorandom Number Generators._ OOPSLA 2014.
  [doi:10.1145/2714064.2660195](https://doi.org/10.1145/2714064.2660195).
  SplitMix, which seeds xoshiro.
- B. P. Welford. _Note on a Method for Calculating Corrected Sums of Squares
  and Products._ Technometrics 4(3), 1962. The running means and variances of
  simulation statistics.

## Citing MAGPIE

If you use MAGPIE in research, please cite it. [CITATION.cff](CITATION.cff)
holds the details, and GitHub's **Cite this repository** button formats them.

## License

MAGPIE is free software under the [GNU General Public License v3.0](LICENSE.md).
