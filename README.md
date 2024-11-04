# MAGPIE

**Macondo Accordant Game Program and Inference Engine**

MAGPIE is a crossword game playing and analysis program that currently supports the following features:

- Static move generation
- Montecarlo simulation
- Exhaustive inferences
- Autoplay
- Superleave generation

MAGPIE started as a C rewrite of [Macondo](https://github.com/domino14/macondo) but has since incorporated a variety of new features, algorithms, and data strctures. It uses several concepts originally developed in [wolges](https://github.com/andy-k/wolges), including shadow playing and the KWG and KLV data structures.

## Setup
From this page, download and unzip the MAGPIE repo or use git clone:
```
git clone https://github.com/jvc56/MAGPIE.git
```
Navigate into the MAGPIE directory:
```
cd MAGPIE
```
Compile the MAGPIE executable:
```
make magpie BUILD=release
```
Download the required input data for common lexicons:
```
./download_data.sh
```
## Usage

MAGPIE has four different execution modes:

### Single one-off command

To run a single command and exit immediately, invoke the MAGPIE executable with the desired command and all necessary arguments. For example, the following command:

```
./bin/magpie autoplay games 100 -lex CSW21 -s1 equity -s2 equity -r1 best -r2 best -threads 4 -hr true
```

will run 100 autoplay games in the CSW21 lexicon, print the result in a human-readable format, and exit immediately.

#### Single one-off command conditions

MAGPIE will exit immediately after completing the command if the following conditions are met:

- A command is specified
- The specified command is not a CGP load
- No infile is specified
- No execution mode is specified

Otherwise, MAGPIE will not exit immediately.

### Console mode

To enter console mode, use the `console` argument:

```
./bin/magpie set -mode console
```

Console mode is also the default mode if the one-off command conditions are not met, so you can also enter console mode by running

```
./bin/magpie
```

In console mode, the arguments used to specify the configuration persist across commands, so the following commands

```
./bin/magpie
magpie>set -lex CSW21
magpie>set -s1 equity -s2 equity
magpie>set -r1 best -r2 best
magpie>set -threads 4
magpie>autoplay games 100
```

are equivalent to

```
./bin/magpie
magpie>autoplay games 100 -lex CSW21 -s1 equity -s2 equity -r1 best -r2 best -threads 4
```

### UCGI mode

UCGI mode operates exactly the same as console mode with the following exceptions:

- All commands are asynchronous. This means they will immediately start running in the background while MAGPIE continues to listen for user input.
- The `stop` command will halt any ongoing commands.

To enter UCGI mode, specify the `ucgi` argument:

```
./bin/magpie set -mode ucgi
```

### Command file mode

To execute commands in a file, you can specify the `infile` argument:

`./bin/magpie set -infile some_file_with_commands`

For example, if the contents of `some_file_with_commands` were:

```
autoplay games 100 -lex CSW21 -s1 equity -s2 equity -r1 best -r2 best -numplays 1 -threads 4
autoplay games 50 -lex NWL20 -s1 equity -s2 equity -r1 best -r2 best -numplays 1 -threads 4
```

MAGPIE would play autoplay 100 CSW21 games and then autoplay 20 NWL20 games.

## Commands and Options
All commands can be specified by the shortest unambiguous string. For example, the generate command can be specified by any of the following strings:
```
generat
genera
gen
```
Commands can take required positional arguments followed by optional positional arguments.

Options are stateful variables that persist between commands. Options can be specified for any command by listing them after the command's positional arguments in the form.
```
-<option> <value>
```
For example, autoplay takes 2 positional arguments. To specify the lexicon, use the lexicon option after the positional arguments:
```
autoplay games 100 -lex CSW21
```
### Commands
#### cgp
Loads the given CGP. The CGP must have the four required components:
```
cgp <board> <racks> <scores> <consecutive_zeros>
```
##### Examples
Load an empty CGP:
```
cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0
```
Load a game in progress while specifying the lexicon and letter distribution options:
```
cgp 4AUREOLED3/11O3/11Z3/10FY3/10A4/10C4/10I4/7THANX3/10GUV2/15/15/15/15/15/15 AHMPRTU/ 177/44 0 -lex CSW21; -ld english;
```
#### generate
Generates plays for the position. The generate command takes no arguments as its behavior is mediated by the options. Generating plays completely resets the move list, discarding any moves that may have be added manually.
##### Examples
Generate moves for the current position:
```
gen
```
Generate at most 17 moves for the current position:
```
gen -numplays 17
```
#### addmoves
Adds moves to the move list if they do not already exist. Moves added manually will be discarded by the move generation command. Multiple moves are delimited by a comma and do not have any whitespace. All added moves must adhere to the [UCGI format](https://github.com/woogles-io/open-protocols/blob/main/ucgi/ucgi.md).
##### Examples
Add a play:
```
addmoves 8D.REASON
```
Add an exchange:
```
addmoves ex.QV
```
Add a pass:
```
addmoves pass
```
Add multiple moves:
```
addmoves pass,8f.NIL,8F.LIN,8D.ZILLION,8F.ZILLION,ex.ILN
```
#### rack
Specifies the rack for one of the players. The rack command requires the player index followed by the rack:
```
rack <player_index> <rack>
```
If no rack is given, the rack will be set to the empty rack.
##### Examples
Set player one's rack to ABCD:
```
rack 1 ABCD
```
Set player two's rack to EFGHI?:
```
rack 2 EFGHI?
```
Set player one's rack to empty:
```
rack 1
```
#### simulate
Run a montecarlo simulation of the given position with the given on turn player rack. The simulate command takes an optional argument specifying the partially known tiles of the opponent.
##### Examples
Simulate the current position:
```
sim
```
Simulate the current position with a partially known oppopent rack of ES:
```
sim ES
```
#### infer
Run an inference of the given position. Inferences are exhaustive (iterate through all possible racks) and based on static equity. Inferences require the the target player (the player being inferred) index and either the number exchanged or the played tiles and the score.
##### Examples
Infer what player 2 had when they played the letters ABC for 20 points:
```
infer 2 ABC 20
```
Infer what player 1 had when they exchanged 3 tiles:
```
infer 1 3
```
#### autoplay
Run autoplay games where player one plays player two. When evaluating which player is better, it is recommended to use the game pairs option (specified by "-gp") to reduce the noise in the result.
##### Running autoplay with game pairs
The game pairs option removes some noise from the autoplay results by playing games in pairs using the same seed, with player one going first in one game and player two going first in the other game. Since MAGPIE games are deterministic for a given starting seed, if both players make the exact same decision for each corresponding play, the games will be identical. The final results are aggregated into two statistics, all games and divergent games. A pair of games is only added to the divergent games statistic if the players made a different play for the exact same position.

MAGPIE also implements double sided tile drawing to reduce the noise introduced by making a different play. At the start of each game, the bag is preshuffled, and each player draws from their own "end" of the bag. Player one would draw from the bag starting at index 0 and player two would draw from the bag starting at index 99 (assuming the bag started with 100 tiles). Exchanged tiles for both players are returned completely randomly. When a player makes a different decision and perhaps plays a different number of tiles, the difference between all of the tiles they will end up drawing is greatly reduced.

By using game pairs with double sided tile drawing, the noise of the final result will be much lower and a statsig signal can be derived from fewer games.
#### convert
Convert one data format to another.
##### Examples
Convert a text file to a dawg (stored as a KWG):
```
convert text2dawg CSW21 CSW21
```
Convert a KLV file to a human readable CSV:
```
convert klv2csv CSW21 CSW21
```
#### leavegen
Generates superleaves for the given lexicon. The leavegen command requires the minimum target leave count for each generation and the number of games to play before it starts forcing rare draws.
##### Examples
Generate leaves for the CSW21 lexicon with 4 generations with a minimum of 100, 200, 500, and 1000 minimum leave target counts, while only forcing rare leaves after playing 10,000,000 games:
```
leavegen 100,200,500,1000 10000000
```
#### setoptions
Sets the specified options. Note that options can be set together with any command.
```
setoptions -numplays 5 -it 6
```

### Options
#### bb

Specifies the bingo bonus. The default value is 50.

#### bdn

Specifies the board layout.

#### var

Specifies the game variant.

#### ld

Specifies the letter distribution.

#### lex

Specifies the lexicon for both players

#### p1, p2

Specifies the name for the given player.

#### l1, l2

Specifies the lexicon for the given player.

#### k1, k2

Specifies the leaves for the given player.

#### s1, s2

Specifies the move sort type for the given player. The valid sort options are `equity` and `score`.

#### r1, r2

Specifies the move record type for the given player. The valid record options are `best` and `all`.
The `best` option will only record the best move according to the sorting type and the `all` option will record the number of plays specified by `numplays`.

#### winpct

Specifies the win percentage filename.

#### plies

Specifies the number of plies the simulation runs.

#### numplays

Specifies the number of plays that the move generator will record.

#### iterations

Specifies the number of simulations or the number of games or game pairs.

#### scond

Specifies the stopping condition of the simulation. Valid options are `95`, `98`, and `99`.

#### eq

Specifies the equity margin for the player to infer. If the play made by the player to infer is within the equity margin, it is considered a possible rack.

#### gp

Specifies whether autoplay should run with game pairs or not.

#### seed

Specifies the random seed

#### threads

Specifies the number of threads with which to run the command.

#### pfreq

Specifies the interval of iterations to print the current info for the command.

#### cfreq

Specifies the interval of iterations to check the stopping condition for a simulation.

#### infile

Specifies the input file for the MAGPIE execution. This can be a regular file or a pipe. By default, infile is `stdin`.

#### outfile

Specifies the output file for the MAGPIE execution. Warnings and errors still get printed to `stderr`.

#### mode

Specifies that MAGPIE should run in console or UCGI mode.
