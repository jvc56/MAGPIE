#ifndef WASM_API_H
#define WASM_API_H

#include <stdint.h>

void destroy_wasm_exec_states();

// Some functions for our WASM API. Not all the WASM-accessible functions are
// defined here. See the Makefile-wasm for exports.

char *score_move(const char *cgpstr, int move_type, int row, int col, int dir,
                 uint8_t *tiles, uint8_t *leave, int ntiles, int nleave);

#endif