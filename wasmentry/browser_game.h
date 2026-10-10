#ifndef WASM_BROWSER_GAME_H
#define WASM_BROWSER_GAME_H
char *wasm_game_action(const char *text, const char *lexicon, int event_index,
                       const char *rack, const char *move, const char *note,
                       int action, const char *current_cgp, unsigned int seed,
                       int current_player, int allow_phonies);
char *wasm_import_gcg(const char *text, const char *lexicon);
void wasm_game_cache_destroy(void);
#endif
