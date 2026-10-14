#ifndef XS_GAME_H
#define XS_GAME_H

#include "../util/io_util.h"

typedef struct XSGameStats {
  unsigned ranked;
  unsigned explicit_moves;
  unsigned ranked_draws;
} XSGameStats;

char *xs_game_encode(const char *gcg, const char *paths, unsigned profile,
                     XSGameStats *stats, ErrorStack *errors);
char *xs_game_decode(const char *token, const char *paths, ErrorStack *errors);

#endif
