# Opening-rack static-evaluation sweep (CSW21, MAGPIE)

Every distinct 7-tile opening rack is enumerated; the top static-eval move is recorded (score + superleave value + MAGPIE's opening placement adjustment), plus the equity of the next-best move and the best exchange-1 play that keeps the J in the leave (for J-containing racks).

Equity is expressed in millipoints in the TSV; this report renders it in points.

- Total distinct 7-tile opening racks: **3,199,724**
- Top-move breakdown:
  - `tile`: 2,456,013
  - `exch1`: 522
  - `exch`: 743,189
  - `pass`: 0
- Racks containing a J: **611,253**; of those, racks where the top static play is an exchange-1 keeping the J: **0**.

## Letter coverage: 6-tile leaves that contain each letter

For every letter, how many opening racks have a top static play that is an **exchange-1** whose resulting 6-tile leave contains that letter. The Discord claim under investigation is that the J is the only letter with zero such racks.

| letter | racks where the letter is kept by the optimal exchange-1 | best margin (pts) | best rack | best leave |
|---|---|---|---|---|
| A | 208 | 12.585 | ACEHQRS | ACEHRS |
| B | 36 | 3.676 | ABELQRS | ABELRS |
| C | 133 | 12.585 | ACEHQRS | ACEHRS |
| D | 143 | 5.889 | DEEPQRS | DEEPRS |
| E | 447 | 12.585 | ACEHQRS | ACEHRS |
| F | 10 | 5.310 | EFFQRS? | EFFRS? |
| G | 38 | 4.879 | AEGLNQS | AEGLNS |
| H | 74 | 12.585 | ACEHQRS | ACEHRS |
| I | 67 | 5.942 | CEHIJRS | CEHIRS |
| J | 0 |  |  |  |
| K | 27 | 4.606 | AEKNQRS | AEKNRS |
| L | 163 | 7.080 | ACELPQS | ACELPS |
| M | 80 | 5.821 | ADEMNQS | ADEMNS |
| N | 161 | 5.821 | ADEMNQS | ADEMNS |
| O | 176 | 8.197 | EHOQRST | EHORST |
| P | 121 | 7.080 | ACELPQS | ACELPS |
| Q | 11 | 4.844 | CHQSTUV | CHQSTU |
| R | 248 | 12.585 | ACEHQRS | ACEHRS |
| S | 397 | 12.585 | ACEHQRS | ACEHRS |
| T | 114 | 8.197 | EHOQRST | EHORST |
| U | 16 | 4.844 | CHQSTUV | CHQSTU |
| V | 10 | 5.716 | EEOQRSV | EEORSV |
| W | 13 | 4.363 | DEOQRSW | DEORSW |
| X | 13 | 6.374 | EEPQSX? | EEPSX? |
| Y | 14 | 2.264 | LOPQRY? | LOPRY? |
| Z | 8 | 3.030 | ABOQRSZ | ABORSZ |
| ? | 153 | 6.374 | EEPQSX? | EEPSX? |

## Top 6-tile leaves ranked by number of racks that pick them

Each row is a 6-tile leave that is the optimal exchange-1 leave on at least one opening rack. `racks` is how many opening racks choose it. `margin` columns are the gap above the next-best static move.

| leave (kept) | tile exchanged | racks | min margin (pts) | mean margin (pts) | max margin (pts) | rack achieving max margin |
|---|---|---|---|---|---|---|
| EORSST | T | 3 | 0.205 | 1.538 | 4.205 | EOQRSST |
| AGINRS | S | 3 | 2.163 | 2.514 | 2.689 | AGINRSS |
| AEILNT | T | 3 | 0.247 | 0.409 | 0.506 | AEILNNT |
| AEILNS | N | 3 | 0.190 | 0.639 | 0.890 | AEIILNS |
| AEGILN | O | 3 | 0.149 | 0.512 | 0.849 | AAEGILN |
| EFFRS? | V | 3 | 0.295 | 2.679 | 5.310 | EFFQRS? |
| EOPSST | Q | 2 | 1.185 | 3.324 | 5.463 | EOPQSST |
| ENORSS | Q | 2 | 0.181 | 1.628 | 3.075 | ENOQRSS |
| EIPRSS | J | 2 | 0.176 | 2.278 | 4.380 | EIJPRSS |
| EERSST | U | 2 | 0.763 | 2.763 | 4.763 | EEQRSST |
| EELRST | R | 2 | 0.376 | 0.376 | 0.376 | EELRRST |
| CHQSTU | V | 2 | 1.255 | 3.050 | 4.844 | CHQSTUV |
| AINRST | R | 2 | 0.682 | 0.682 | 0.682 | AINRRST |
| AGINST | S | 2 | 0.178 | 0.796 | 1.414 | AGINSST |
| AGILNS | O | 2 | 0.273 | 1.450 | 2.628 | AGILLNS |
| AELRSS | Q | 2 | 0.925 | 1.948 | 2.971 | AELQRSS |
| AEGINR | U | 2 | 2.798 | 3.188 | 3.578 | AEGINRU |
| AEGINS | I | 2 | 0.190 | 0.232 | 0.274 | AEGIINS |
| ADERST | U | 2 | 0.434 | 0.602 | 0.771 | ADERSTU |
| ADENRS | Q | 2 | 0.297 | 0.634 | 0.970 | ADENQRS |
| ADEINS | U | 2 | 0.312 | 1.234 | 2.155 | ADEINSU |
| ABELMS | V | 2 | 0.849 | 2.105 | 3.361 | ABELMQS |
| EIQSU? | W | 2 | 0.228 | 0.528 | 0.827 | EIJQSU? |
| CEMPS? | V | 2 | 1.014 | 1.014 | 1.014 | CEMPSV? |
| BELMS? | V | 2 | 0.211 | 1.391 | 2.571 | BELMQS? |
| OPRSST | Q | 1 | 0.258 | 0.258 | 0.258 | OPQRSST |
| GINORS | R | 1 | 0.158 | 0.158 | 0.158 | GINORRS |
| GIILNS | I | 1 | 1.511 | 1.511 | 1.511 | GIIILNS |
| GHILNS | V | 1 | 0.243 | 0.243 | 0.243 | GHILNSV |
| EPRSST | Q | 1 | 1.520 | 1.520 | 1.520 | EPQRSST |
| EOPSSS | Q | 1 | 0.033 | 0.033 | 0.033 | EOPQSSS |
| EOPRST | Q | 1 | 6.406 | 6.406 | 6.406 | EOPQRST |
| EOPRSS | Q | 1 | 6.207 | 6.207 | 6.207 | EOPQRSS |
| EOPPRS | Q | 1 | 4.263 | 4.263 | 4.263 | EOPPQRS |
| ENOSST | Q | 1 | 3.641 | 3.641 | 3.641 | ENOQSST |
| ENORST | Q | 1 | 2.604 | 2.604 | 2.604 | ENOQRST |
| ENOPST | Q | 1 | 0.191 | 0.191 | 0.191 | ENOPQST |
| ENOPRS | Q | 1 | 1.575 | 1.575 | 1.575 | ENOPQRS |
| EMOSST | Q | 1 | 2.592 | 2.592 | 2.592 | EMOQSST |
| EMORST | Q | 1 | 3.261 | 3.261 | 3.261 | EMOQRST |
| EMORSS | Q | 1 | 1.595 | 1.595 | 1.595 | EMOQRSS |
| EMNOST | Q | 1 | 5.532 | 5.532 | 5.532 | EMNOQST |
| EMNORS | Q | 1 | 3.071 | 3.071 | 3.071 | EMNOQRS |
| ELOSST | Q | 1 | 2.359 | 2.359 | 2.359 | ELOQSST |
| ELORSS | Q | 1 | 2.906 | 2.906 | 2.906 | ELOQRSS |
| ELOPST | Q | 1 | 0.233 | 0.233 | 0.233 | ELOPQST |
| ELOPSS | Q | 1 | 1.987 | 1.987 | 1.987 | ELOPQSS |
| ELOPRS | Q | 1 | 1.381 | 1.381 | 1.381 | ELOPQRS |
| ELOPPS | Q | 1 | 0.396 | 0.396 | 0.396 | ELOPPQS |
| ELNOSS | Q | 1 | 0.551 | 0.551 | 0.551 | ELNOQSS |

_(showing top 50 of 491 distinct leaves)_

## J nearest miss

Racks containing a J, sorted by the smallest equity gap between the top static play and the best **exchange-1** play that trades a non-J tile (keeping J in the 6-tile leave). Lower `miss` = closer the J-keeping exchange-1 comes to being optimal.

| rack | top move | top eq (pts) | best exch1 keeping J | exch1 leave | exch1 eq (pts) | miss (pts) |
|---|---|---|---|---|---|---|
| CCEJR?? | (exch CJ) | 44.670 | (exch C) | CEJR?? | 43.517 | 1.153 |
| CEJNOQ? | (exch JQ) | 35.358 | (exch Q) | CEJNO? | 34.133 | 1.225 |
| CDEJQ?? | (exch JQ) | 44.760 | (exch Q) | CDEJ?? | 43.188 | 1.572 |
| CCDEJ?? | (exch CJ) | 44.760 | (exch C) | CDEJ?? | 43.188 | 1.572 |
| CJQRS?? | (exch JQ) | 43.578 | (exch Q) | CJRS?? | 41.840 | 1.738 |
| CCJRS?? | (exch CJ) | 43.578 | (exch C) | CJRS?? | 41.840 | 1.738 |
| CCIJR?? | (exch CJ) | 43.052 | (exch C) | CIJR?? | 41.230 | 1.822 |
| FJQRS?? | (exch JQ) | 42.425 | (exch Q) | FJRS?? | 40.313 | 2.112 |
| EJQTV?? | (exch JV) | 42.523 | (exch V) | EJQT?? | 40.354 | 2.169 |
| CCJST?? | (exch CJT) | 43.273 | (exch C) | CJST?? | 41.035 | 2.238 |
| CEJQY?? | 8E QuEY | 45.740 | (exch Q) | CEJY?? | 43.452 | 2.288 |
| GJNQR?? | (exch GJNQ) | 37.753 | (exch Q) | GJNR?? | 35.456 | 2.297 |
| CJMSV?? | (exch CJV) | 43.711 | (exch V) | CJMS?? | 41.328 | 2.383 |
| CJSTV?? | 8D JiVeST | 43.521 | (exch V) | CJST?? | 41.035 | 2.486 |
| CEJMQ?? | (exch JQ) | 43.356 | (exch Q) | CEJM?? | 40.789 | 2.567 |
| CEJPQ?? | (exch JQ) | 44.003 | (exch Q) | CEJP?? | 41.405 | 2.598 |
| EGJNQ?? | (exch GQ) | 42.110 | (exch Q) | EGJN?? | 39.500 | 2.610 |
| CCEJS?? | 8D JuiCE | 47.935 | (exch C) | CEJS?? | 45.305 | 2.630 |
| CEJQST? | 8D QuEST | 40.058 | (exch Q) | CEJST? | 37.424 | 2.634 |
| CJQST?? | 8G QaT | 43.685 | (exch Q) | CJST?? | 41.035 | 2.650 |
| FJSTV?? | (exch FTV) | 41.394 | (exch V) | FJST?? | 38.718 | 2.676 |
| CJNSV?? | (exch JNV) | 43.273 | (exch V) | CJNS?? | 40.553 | 2.720 |
| CFJNS?? | (exch FJN) | 43.273 | (exch F) | CJNS?? | 40.553 | 2.720 |
| CFJRS?? | 8H SCaRF | 44.681 | (exch F) | CJRS?? | 41.840 | 2.841 |
| DGJNQ?? | (exch GJNQ) | 37.693 | (exch Q) | DGJN?? | 34.851 | 2.842 |
| JLQSY?? | (exch JLQ) | 44.278 | (exch Q) | JLSY?? | 41.267 | 3.011 |
| EJMNQ?? | 8D QuEMe | 44.269 | (exch Q) | EJMN?? | 41.234 | 3.035 |
| AJNQSX? | (exch JQX) | 34.766 | (exch Q) | AJNSX? | 31.710 | 3.056 |
| JNOQSX? | (exch JQX) | 33.267 | (exch Q) | JNOSX? | 30.184 | 3.083 |
| GJNQW?? | (exch GJNQW) | 37.559 | (exch Q) | GJNW?? | 34.460 | 3.099 |
