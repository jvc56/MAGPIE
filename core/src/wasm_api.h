#ifndef WASM_API_H
#define WASM_API_H
#include <stdint.h>

// Some functions for our WASM API. Not all the WASM-accessible functions are
// defined here. See the Makefile-wasm for exports.

char *score_play(char *cgpstr, int move_type, int row, int col, int vertical,
                 uint8_t *tiles, uint8_t *leave, int ntiles, int nleave);

#endif