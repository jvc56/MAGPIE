#ifndef XS_RANK_H
#define XS_RANK_H

#include "../ent/board_layout.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/players_data.h"
#include "../util/io_util.h"
#include <stdint.h>

typedef struct XSMove {
  unsigned start;
  unsigned down;
  char word[16];
  int64_t equity;
} XSMove;

typedef struct XSRanker {
  PlayersData *players;
  BoardLayout *layout;
  LetterDistribution *distribution;
  Game *game;
} XSRanker;

XSRanker *xs_ranker_create(const char *paths, unsigned profile,
                           ErrorStack *errors);
void xs_ranker_destroy(XSRanker *ranker);
XSMove *xs_rank_moves(XSRanker *ranker, const char board[225], const char *rack,
                      int *count, ErrorStack *errors);

#endif
