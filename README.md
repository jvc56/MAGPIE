# MAGPIE

**Macondo Accordant Game Program and Inference Engine**

MAGPIE is a crossword game playing and analysis program that currently supports the following features:

- Static move generation
- Montecarlo simulation
- Exhaustive inferences
- Autoplay

MAGPIE is basically a C rewrite of [Macondo](https://github.com/domino14/macondo) and uses several algorithms and data structures originally developed in [wolges](https://github.com/andy-k/wolges), including shadow playing and the KWG and KLV data structures.

## Installation

From this page, download and unzip the MAGPIE repo or use git clone:

```
git clone https://github.com/jvc56/MAGPIE.git
```

## Compilation

To compile the MAGPIE executable, run:

```
make magpie BUILD=release
```

## Usage

Once MAGPIE is compiled, you can execute commands.

MAGPIE has four different execution modes:

### Single one-off command

To run a single command and exit immediately, invoke the MAGPIE executable with the desired command and all necessary arguments. For example, the following command:

```
./bin/magpie go autoplay lex CSW21 s1 equity s2 equity r1 best r2 best i 100 numplays 1 threads 4
```

will run 100 autoplay games in the CSW21 lexicon, print the result, and exit immediately.

#### Single one-off command conditions

MAGPIE will exit immediately after completing the command if the following conditions are met:

- A 'go' subcommand is specified
- No infile is specified
- No execution mode is specified

Otherwise, MAGPIE will not exit immediately.

### Console mode

To enter console mode, use the `console` argument:

```
./bin/magpie console
```

Console mode is also the default mode if the one-off command conditions are not met, so you can also enter console mode by running

```
./bin/magpie
```

In console mode, the arguments used to specify the configuration persist across commands, so the following commands

```
./bin/magpie
magpie>lex CSW21
magpie>s1 equity s2 equity
magpie>r1 best r2 best
magpie>i 100 numplays 1 threads 4
magpie>go autoplay
```

are equivalent to

```
./bin/magpie
magpie>go autoplay lex CSW21 s1 equity s2 equity r1 best r2 best i 100 numplays 1 threads 4
```

### UCGI mode

UCGI mode implements the [UCGI Protocol](https://docs.google.com/document/d/175zdbEiS37XLG600fDafDyeOcLwHQ804xN4UNdU8h1Q), the crossword game equivalent to the [UCI Protocol](https://www.wbec-ridderkerk.nl/html/UCIProtocol.html). It operates exactly the same as console mode with the following exceptions:

- All commands are asynchronous. This means they will immediately start running in the background while MAGPIE continues to listen for user input.
- The `stop` command will halt any ongoing commands.

To enter UCGI mode, specify the `ucgi` argument:

```
./bin/magpie ucgi
```

### Command file mode

To execute commands in a file, you can specify the `infile` argument:

`./bin/magpie infile some_file_with_commands`

For example, if the contents of `some_file_with_commands` were:

```
go autoplay lex CSW21 s1 equity s2 equity r1 best r2 best i 100 numplays 1 threads 4
go autoplay lex NWL20 s1 equity s2 equity r1 best r2 best i 50 numplays 1 threads 4
```

MAGPIE would play autoplay 100 CSW21 games and then autoplay 20 NWL20 games.

## Development

### Formatting

If you are using vscode to develop, using the following attributes in your `.vscode/settings.json` file:

```
"editor.formatOnSave": true,
"C_Cpp.clang_format_fallbackStyle": "{ ColumnLimit: 80 }",
```

### Testing

When creating a new module or functionality that is sufficiently complex, add a test for it in `MAGPIE/test/`.

## Commands and Arguments

### Subcommands

#### position cgp

Load the given CGP. The CGP must have the four required components:

```
position cgp <board> <racks> <scores> <consecutive_zeros>
```

#### go sim

Run a montecarlo simulation of the given position.

#### go infer

Run an inference of the given position. Inferences are exhaustive (iterate through all possible racks) and based on static equity.

#### go autoplay

Run autoplay games where player one plays player two.

#### setoptions

Sets the specified options. The `setoptions` string is optional for this command, so

```
setoptions numplays 5 i 6
```

and

```
numplays 5 i 6
```

are equivalent.

### Arguments

#### bb

Specifies the bingo bonus. The default value is 50.

#### bdn

Specifies the board layout. For now, only the `crossword` layout is available.

#### var

Specifies the game variant. For now, only the `classic` variant is available.

#### ld

Specifies the letter distribution.

#### lex

Specifies the lexicon for both players

#### p1, p2

Specifies the name for the given player.

#### l1, l2

Specifies the lexicon for the given player.

#### s1, s2

Specifies the move sort type for the given player. The valid sort options are `equity` and `score`.

#### r1, r2

Specifies the move record type for the given player. The valid record options are `best` and `all`.
The `best` option will only record the best move according to the sorting type and the `all` option will record the number of plays specified by `numplays`.

#### rack

Specifies the rack for the opponent. For simulation, this specifies the known tiles. For inferences, this specifies the played tiles.

#### winpct

Specifies the win percentage filename.

#### plies

Specifies the number of plies the simulation runs.

#### numplays

Specifies the number of plays that the move generator will record.

#### i

Specifies the number of simulations or the number of games or game pairs.

#### cond

Specifies the stopping condition of the simulation. Valid options are `95`, `98`, and `99`.

#### static, nostatic

Specifies whether the sim command should generate static moves.

#### pindex

Specifies the player index to infer for the inference command.

#### score

Specifies the score made by the player to infer.

#### eq

Specifies the equity margin for the player to infer. If the play made by the player to infer is within the equity margin, it is considered a possible rack.

#### exch

Specifies the number of tiles exchanged.

#### gp, nogp

Specifies whether autoplay should run with game pairs or not.

#### rs

Specifies the random seed

#### threads

Specifies the number of threads with which to run the command.

#### info

Specifies the interval of iterations to print the current info for the command.

#### check

Specifies the interval of iterations to check the stopping condition for a simulation.

#### infile

Specifies the input file for the MAGPIE execution. This can be a regular file or a pipe. By default, infile is `stdin`.

#### outfile

Specifies the output file for the MAGPIE execution. Warnings and errors still get printed to `stderr`.

#### console

Specifies that MAGPIE should run in console mode.

#### ucgi

Specifies that MAGPIE should run in ucgi mode.

## Examples

Run a Montecarlo simulation as a one-off:

```
./bin/magpie go sim threads 7 numplays 5 cond 99 i 10000 info 100 check 500 plies 5 cgp C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ 336/298 0 lex NWL20;
```

Run a static simulation in console mode:

```
 ./bin/magpie console
magpie>go sim static plies 1 threads 1 numplays 15 cgp C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ 336/298 0 lex NWL20;
```

Run an inference:

```
 ./bin/magpie go infer rack HUJA pindex 0 score 20 exch 0 numplays 20 info 1000000 threads 4 lex OSPS44
```

Run autoplay games in ucgi mode and then stop:

```
 ./bin/magpie ucgi
go autoplay lex CSW21 s1 equity s2 equity r1 best r2 best i 1000000 numplays 1 threads 4
stop
```
