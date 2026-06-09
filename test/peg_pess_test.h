#ifndef PEG_PESS_TEST_H
#define PEG_PESS_TEST_H

// Independent pessimistic ("guaranteed win", macondo -E style) PEG reference
// solver. This is deliberately a from-scratch implementation sharing no code
// with the production solver in src/impl/peg.c: it recursively solves both
// sides over perfect-information ordered draws, so it can chase macondo's
// exact guaranteed-win numbers at depth, where the production
// PEG_OPP_PESSIMISTIC model is only a 1-ply adversarial playout. Used to
// cross-validate production results and study macondo parity.

// Full pessimistic evaluation of one candidate move over every ordered draw
// of the bag tiles. Env knobs (PASSPEG_PESSFULL_*): CGP and MOVE (required),
// PLIES, MAX_OPP_K, TT_MB, TT_SHARED, OPP_SORT, MOVER_SORT, SUBPERM_SORT,
// SKIP_WORD_PRUNING, THREADS, SPLIT_OPP, RECURSIVE_SPLIT, FORCE_NESTED_PERM,
// FIRST_WIN, EG_THREADS, NESTED (+_DEPTH/_K), CACHE, NESTED_CACHE,
// SLOW_SOLVE_LOG_S, IDLE_PROBE_S, RUNG4_PROBE_S, GROUP_BY_FIRST, WANT_DRAW.
void test_pass_peg_pessimistic_full_eval(void);

// Sanity probe: a single endgame_solve on one pessimistic scenario.
// Env knobs: PASSPEG_ENDGAME_CGP (required), _MOVE, _DRAW, _PLIES, _THREADS,
// _FIRST_WIN.
void test_pass_peg_endgame_one(void);

#endif // PEG_PESS_TEST_H
