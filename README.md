# MAGPIE

Crossword game move generator and endgame solver

## Examples

After compiling magpie executable (`make magpie BUILD=release`) copy it to the directory where the `data` folder is. Then execute it. It talks in a format called UCGI (spec coming).

### Montecarlo Simming


`position cgp C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ 336/298 0 lex NWL20;`

then

`go sim threads 7 plays 5 stopcondition 99 i 10000 info 100 checkstop 500 depth 5`

- threads: The number of threads to use (7 in this case)
- plays: How many plays to sim (The top 5 in this case)
- stopcondition: Stop after 95, 98, or 99 percent sureness level. We must be this percent sure that the top play is the best one before stopping.
- i: The number of iterations to stop after if we don't hit the stop condition.
- info: Print out information every this many iterations
- checkstop: Check the stopping condition every this many iterations
- depth: How deep to search (number of plies)