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

// Reconstructs an autoplay position from its game seed and common-prefix move
// list, then prints a replayable CGP. This is analysis tooling for historical
// match logs that predate per-turn CGP capture.
//
// Env knobs:
//   TM_REPLAY_GAME_SEED — autoplay PCGAME seed.
//   TM_REPLAY_START     — starting player index (0 or 1).
//   TM_REPLAY_MOVES     — pipe-separated UCGI moves before the target root.
void test_time_manager_match_replay(void);

// Dumps the PEG greedy seed's ranking of the entire candidate field. Stage 0
// scores every generated move, so this shows the exact win% and rank the
// halving stages cut against -- the view needed when a strong play never
// reaches endgame fidelity.
//
// Env knobs:
//   PEG_GREEDY_CGP       — position (required).
//   PEG_GREEDY_LEX       — lexicon (default CSW24).
//   PEG_GREEDY_HIGHLIGHT — comma-separated substrings to flag in the dump.
//   PEG_GREEDY_THREADS   — worker threads (default 4).
void test_peg_greedy_candidate_dump(void);

#endif // PEG_ORACLE_TEST_H
