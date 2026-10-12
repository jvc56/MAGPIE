#ifndef BROWSER_GAME_H
#define BROWSER_GAME_H

typedef enum {
  BROWSER_GAME_IMPORT,
  BROWSER_GAME_COMMIT,
  BROWSER_GAME_NOTE,
  BROWSER_GAME_CHALLENGE,
  BROWSER_GAME_DEAL,
  BROWSER_GAME_LIVE_MOVE,
  BROWSER_GAME_LIVE_CHALLENGE,
} BrowserGameAction;

typedef enum {
  BROWSER_CHALLENGE_VOID,
  BROWSER_CHALLENGE_SINGLE,
  BROWSER_CHALLENGE_DOUBLE,
  BROWSER_CHALLENGE_FIVE_PER_PLAY,
  BROWSER_CHALLENGE_TEN_PER_PLAY,
  BROWSER_CHALLENGE_FIVE_PER_WORD,
  BROWSER_CHALLENGE_TEN_PER_WORD,
} BrowserChallengeRule;

char *browser_game_action(const char *text, const char *lexicon,
                          int event_index, const char *rack, const char *move,
                          const char *note, BrowserGameAction action,
                          const char *current_cgp, unsigned int seed,
                          int current_player, int challenge_rule);
char *browser_game_import_gcg(const char *text, const char *lexicon);
void browser_game_cache_destroy(void);
#endif
