#ifndef JSON_API_H
#define JSON_API_H

#include "config.h"

// JSON serialization of MAGPIE state for UI consumers (e.g. the WASM web UI).
//
// Every function returns a newly heap-allocated, NUL-terminated JSON string
// that the caller must free(). None of them ever return NULL: when the
// requested data is unavailable they return a small JSON object describing the
// situation (e.g. {"ok":false,...} or {"ok":true,"hasMoves":false,...}).
//
// These are read-only views of the current Config state; run the relevant
// command (cgp / generate / simulate / endgame) first to populate it.

// Board grid, premium squares, player racks/scores, turn, bag, the active
// lexicon / letter distribution / board layout, and the position's CGP string.
char *json_api_get_state(const Config *config);

// The most recently generated move list. When valid simulation results exist
// for the current position, each simmed play also carries its win% and mean
// equity. Moves are emitted in the order generate produced them (best first).
char *json_api_get_moves(const Config *config);

// A summary of the most recent endgame solve: value, depth, elapsed seconds,
// principal-variation count, and the best move (with placed-tile geometry for
// board highlighting). Full PV detail is available via the "shendgame" command.
char *json_api_get_endgame(const Config *config);

#endif
