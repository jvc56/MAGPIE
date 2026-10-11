#ifndef BROWSER_GAME_H
#define BROWSER_GAME_H

typedef enum {
  BROWSER_GAME_IMPORT,
  BROWSER_GAME_COMMIT,
  BROWSER_GAME_NOTE,
  BROWSER_GAME_CHALLENGE,
  BROWSER_GAME_DEAL,
  BROWSER_GAME_LIVE_MOVE,
} BrowserGameAction;

char *browser_game_action(const char *text, const char *lexicon,
                          int event_index, const char *rack, const char *move,
                          const char *note, BrowserGameAction action,
                          const char *current_cgp, unsigned int seed,
                          int current_player, int allow_phonies);
char *browser_game_import_gcg(const char *text, const char *lexicon);
void browser_game_cache_destroy(void);
#endif
