#ifndef PEG_ORACLE_TEST_H
#define PEG_ORACLE_TEST_H

// Direct-endgame oracle for a single (position, move): evaluates a fixed
// candidate move on a 1-in-bag PEG by scenario-by-scenario endgame_solve,
// bypassing the PEG search entirely. Gives the ground-truth win%/spread for
// the chosen move at the requested endgame depth — useful for debugging a
// solver disagreement on one move.
//
// Env knobs:
//   PASSPEG_ORACLE_CGP    — position (default: the lone macondo-disagreement
//                           board from the pass-PEG study).
//   PASSPEG_ORACLE_MOVE   — UCGI move, '.' for in-move separator (default
//                           C6.REEST).
//   PASSPEG_ORACLE_PLIES  — endgame plies (default 12).
//   PASSPEG_ORACLE_TIME   — per-solve soft/hard time limit seconds
//                           (default 30).
void test_pass_peg_oracle_eval_move(void);

#endif // PEG_ORACLE_TEST_H
