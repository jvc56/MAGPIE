#ifndef WASM_API_H
#define WASM_API_H

#include <stdint.h>

void wasm_destroy_configs(void);

// Some functions for our WASM API. Not all the WASM-accessible functions are
// defined here. See the Makefile-wasm for exports.

char *wasm_score_move(const char *cgpstr, const char *ucgi_move_str);

#endif