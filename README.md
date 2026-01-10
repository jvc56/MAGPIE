# MAGPIE

**Macondo Accordant Game Program and Inference Engine**

MAGPIE is a crossword game playing and analysis program that supports the following features:

- Static move generation
- Montecarlo simulation
- Exhaustive inferences
- Autoplay
- Superleave generation
- Exhaustive endgame

MAGPIE started as a C rewrite of [Macondo](https://github.com/domino14/macondo) but has since incorporated a variety of new features, algorithms, and data strctures. It uses several concepts originally developed in [wolges](https://github.com/andy-k/wolges), including shadow playing and the KWG and KLV data structures.

## Getting Started

From this page, download and unzip the MAGPIE repo or use git clone:

```
git clone https://github.com/jvc56/MAGPIE.git
```

then navigate into the MAGPIE directory

```
cd MAGPIE
```

and run the setup command

```
./setup.sh
```

You should now be able to run the compiled MAGPIE executable:

```
./bin/magpie
```

This will start MAGPIE in async interactive mode by default. For more details on different ways to run MAGPIE, see [Execution Modes](#execution-modes).

## Usage

### Commands and Settings

MAGPIE accepts two different kinds of arguments: commands and settings.

Commands perform a specific action and settings affect how the actions are performed. Some commands can take positional arguments that could be required or optional. Command arguments only apply to the given command. Settings persist between commands and only change when overwritten by a new specified value. Settings are always denoted by a `-` character. Any number of settings can be specified for any command.

For example, in the following command:

```
magpie> autoplay games 100 -lex CSW21 -threads 4 -hr true
```

The `autoplay` text is the command and the `games` and `100` text are the positional arguments. Everything else is a setting which will apply to the next command, so running the subsequent command:

```
magpie> autoplay games 50 -lex CSW21 -threads 4 -hr true
```

will play 50 games in the CSW21 lexicon with 4 threads and print the results in a human readable format.

All commands and settings can be specified by the shortest unambiguous string. For example, the generate command can be specified by any of the following strings:

```
magpie> generat
magpie> genera
magpie> gen
```

Some commands have one character shortcuts such as the `generate` and `shgame` commands:

```
magpie> g
magpie> s
```

To print more details about commands and settings, run the `help` command:

```
magpie> help
```

To see details for a specific command or setting, provide the command or setting when invoking the help command:

```
magpie> help autoplay
magpie> help lex
```

### Load and Save Settings

On startup, MAGPIE will check for a `settings.txt` file in the current directory and will load the settings saved in that file if it exists. By default, after every successful command that is not invoked in script mode, MAGPIE will save the current settings to `settings.txt` so they do not have to be reentered on the next startup. To disable this feature, set the "savesettings" setting to false.

### Execution Modes

MAGPIE can operate in multiple modes, which are described below:

#### Script Mode

Script mode executes a single command and then exits immediately. To run in script mode, the following conditions must be met:

- A command which is not `set` or `cgp` is given
- No execution mode is specified with the `mode` setting.

If the conditions above are not met, MAGPIE will run in interative mode and wait for user input.

#### Interactive Modes (REPL)

The interactive modes of MAGPIE implement a Read-Evaluate-Print-Loop which continuously listens for and executes user input.

##### Asynchronous Interactive Mode

Asynchronous mode allows the user to either stop or query the status of the currently running command, with the `stop` and `status` asynchronous commands respectively. This mode is enabled by default and can be set with the `-mode async` setting.

In async mode a long running sim and be checked for progress and stopped at anytime during execution:

```
magpie> new
(... game output ...)
magpie> r AEEINNR
magpie> gs
sta
(... current sim results ...)

sto
(... final sim results ...)

magpie>
```

The asynchronous commands can also be specified by their shortest unambiguous strings.

##### Synchronous Interactive Mode

Synchronous mode blocks while a command is running and does will not accept new commands until the previous command has completed. This mode can be set with the `-mode sync` setting. It is not recommended for human users to run in sync mode.

For running many commands programatically from another process, it is recommended to use sync mode instead of script mode to avoid the overhead startup costs.

### API Library

For programs embedding magpie as a library, all commands have a corresponding `str_api_*` function. The `str_api_*` functions all have the signature `char* func(Config*, ErrorStack*, char* cmd)` and use the same parser as the `execute` commands, but return the output as a string instead of printing the output to `stdout`.

Currently, the API library has no clients, so if you are interested in this feature, please feel free to reach out to the developers if you run into any issues.

## Data

The `setup.sh` command will download the necessary lexical data for several common lexica into the `./data` directory organized into 4 subdirectories. All lexical, board layout, and strategy data must be saved in their respective directories for MAGPIE to find them. When specifying input data in MAGPIE, always use the basename without the file extension.

### layouts

This directory contains the layout files which specify the start square and bonus squares for the board. The start square is denoted by the `row, column` integers on the first row, followed by the board layout bonus square. Only the following bonus squares are valid:

- ` ` (no bonus)
- `'` (double letter)
- `-` (double word)
- `"` (triple letter)
- `=` (triple word)
- `^` (quadruple letter)
- `~` (quadruple word)
- `#` (brick, an unplayable square)

The height and width of the board are denoted by the compile time constant `BOARD_DIM` which can be overwritten during compilation. For example, compiling with:

```
make magpie BUILD=release BOARD_DIM=21
```

will compile a MAGPIE executable that only accepts layouts of 21x21.

### letterdistributions

This directory contains the letter distribution CSV files which specify the frequency, score, and display of each tile. The format is:

```
<uppercase_letter>,<lowercase_letter>,<frequency>,<score>,<is_vowel>,[<fullwidth_uppercase_letter>,<full_width_lowercase_letter>]

```

The full width display characters can be optionally specified at the end of the row. Setting a new lexicon with the `lexicon` setting will set a default letter distribution if the lexicon name has a known prefix. Below is a list of lexicon prefixes and their default letter distributions:

- CSW, NWL, OSPD, OSW, America, CEL -> english
- RD -> german
- NSF -> norwegian
- DISC -> catalan
- FRA -> french
- OSPS -> polish
- DSW -> dutch

### lexica

This directory contains the following file types:

- .txt (plain text lexica files)
- .kwg (Kurnia Word Graph (KWG), courtesy of [wolges](https://github.com/andy-k/wolges))
- .klv (KWG that stores leave values, , courtesy of [wolges](https://github.com/andy-k/wolges))
- .wmp (Word Maps)

### strategy

This directory contains win percentage lookup tables used in Monte Carlo simulations.

## Examples

### Annotating a game

The following example demonstrates some of the more common game annotation commands. To see all of the available commands, invoke the `help` command.

First, set the lexicon:

```
magpie> set -lex CSW24
```

then start a new game:

```
magpie> new
```

player names can be specified with the `p1` and `p2` commands:

```
magpie> p1 Adam Logan
magpie> p2 Nigel Richards
```

at any point in the game, player names can be switched with the `switchnames` command:

```
magpie> sw
```

specify the rack for the player on turn:

```
magpie> r EEIJNP?
```

generate moves with the given rack:

```
magpie> g
```

sim the generated moves:

```
magpie> sim
```

to generate moves and sim in a single command, use the `gsim` command:

```
magpie> gsim
```

to set the rack, generate moves, and then sim in a single command, use the `rgsimulate` command:

```
magpie> rgs RETINAS
```

to commit a play, use the commit command:

```
magpie> c 1
```

alternatively, the current best move can be commited with the `tcommit` command:

```
magpie> t
```

which commits the top simming move if there are sim results available, otherwise it will commit the top static move. To challenge the previous play, use the `challenge` command:

```
magpie> chal
```

this will either remove the previous play if it formed a phony word or add a challenge bonus if not. Challenge bonuses for any play can be removed at any point in the game without affecting the subsequent move with the `unchallenge` command:

```
magpie> unchal
```

To save the game as a GCG file, use the `export` command:

```
magpie> e
```

The export command will give the file a default name if no name is provided.

### Analyzing a game from xtables or woogles.io

First, set the lexicon:

```
magpie> set -lex CSW24
```

then import the game

```
magpie> load 54515
```

the `load` command can take several different types of input which are explained in further detail in the `help` command.

To navigate through the game, use `goto`

```
magpie> goto end
magpie> gsim
magpie> goto start
magpie> gsim
magpie> goto 10
magpie> gsim
magpie> goto 3
magpie> infer
```

### Comparing lexica

To play two lexica against each other to see which is stronger, you can create the required lexical data from text files and run the autoplay command. First, set the letter distribution:

```
magpie> set -ld english
```

then convert the text files to the KWG and WMP. The following example assumes a text file called `CSW50.txt` is saved to `./data/lexica`:

```
magpie> convert text2kwg CSW50
magpie> convert text2wordmap CSW50
magpie> convert text2kwg CSW60
magpie> convert text2wordmap CSW60
```

both text file must contain one word per line in all uppercase. Once converted, you can run the `autoplay` command

```
magpie> autoplay games 10000 -l1 CSW50 -l2 CSW60 -leaves CSW21 -gp true -hr true -pfreq 10000
```

It is highly recommended to run with game pairs to reduce statistical noise. To see more details about game pairs, use `help gp`.
