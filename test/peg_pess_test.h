#ifndef PEG_PESS_TEST_H
#define PEG_PESS_TEST_H

// Independent pessimistic ("guaranteed win", macondo -E style) PEG reference
// solver. This is deliberately a from-scratch implementation sharing no code
// with the production solver in src/impl/peg.c: it recursively solves both
// sides over perfect-information ordered draws, so it can chase macondo's
// exact guaranteed-win numbers at depth, where the production
// PEG_OPP_PESSIMISTIC model is only a 1-ply adversarial playout. Retained as
// the engine behind the draw-fix CI regression below.

// Fast CI regression test for the pre-endgame drawing fix (one tiny
// pessimistic case, hardcoded). Asserts mover WIN (1/0/0).
void test_peg_pessfull_draw_regression(void);

#endif // PEG_PESS_TEST_H
