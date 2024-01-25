#ifndef WASM_API_H
#define WASM_API_H

#include <stdint.h>

void destroy_wasm_exec_states();

// Some functions for our WASM API. Not all the WASM-accessible functions are
// defined here. See the Makefile-wasm for exports.

// FIXME: get a better name for this function
char *score_move_from_strings(const char *cgpstr, const char *ucgi_move_str);

#endif