#include "peg_string.h"

#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../impl/peg.h"
#include "../util/string_util.h"
#include "move_string.h"

char *peg_result_get_string(const PegResult *result, const Game *game) {
  StringBuilder *sb = string_builder_create();
  if (result->last_completed_stage < 0 || result->n_top_cands == 0) {
    string_builder_add_string(sb, "no PEG results.\n");
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  string_builder_add_formatted_string(
      sb, "PEG (last completed stage %d): %d candidates, %.2fs\n",
      result->last_completed_stage, result->n_top_cands,
      result->elapsed_seconds);
  string_builder_add_string(sb, "rank  move                  win%    spread\n");
  for (int i = 0; i < result->n_top_cands; i++) {
    const PegRankedCand *cand = &result->top_cands[i];
    string_builder_add_formatted_string(sb, "%4d  ", i + 1);
    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, &cand->move, ld, false);
    const char *move_text = string_builder_peek(move_sb);
    string_builder_add_formatted_string(sb, "%-20s %6.1f  %+7.1f\n", move_text,
                                        100.0 * cand->win_pct,
                                        cand->mean_spread);
    string_builder_destroy(move_sb);
  }
  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}
