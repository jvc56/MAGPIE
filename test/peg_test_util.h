#ifndef PEG_TEST_UTIL_H
#define PEG_TEST_UTIL_H

#include "../src/def/game_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/peg.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/string_util.h"

#include <stdbool.h>
#include <stdio.h>

static inline void peg_test_print_game_position(const Game *game) {
  StringBuilder *sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, sb);
  printf("\n%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);
  game_string_options_destroy(gso);
}

static inline void peg_test_progress_callback(
    int pass, int num_evaluated, const Move *top_moves,
    const double *top_values, const double *top_win_pcts,
    const bool *top_pruned, const bool *top_spread_known,
    const double *top_eval_seconds,
    int num_top, const Game *game, double elapsed,
    double stage_seconds, void *user_data) {
  (void)user_data;
  if (pass == 0) {
    printf("[PEG greedy] %d candidates evaluated in %.3fs\n", num_evaluated,
           stage_seconds);
  } else {
    printf("[PEG %d-ply endgame] %d candidates evaluated in %.3fs (%.3fs "
           "cumulative)\n",
           pass, num_evaluated, stage_seconds, elapsed);
  }
  const LetterDistribution *ld = game_get_ld(game);
  int mover_idx = game_get_player_on_turn_index(game);
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  for (int i = 0; i < num_top; i++) {
    Move m = top_moves[i];
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &m, ld, false);
    bool sk = top_spread_known[i];
    if (move_get_type(&m) == GAME_EVENT_PASS) {
      if (top_pruned[i]) {
        printf("  %d. %s  win%%≤%.1f%%  [%.3fs]\n", i + 1,
               string_builder_peek(sb), top_win_pcts[i] * 100.0,
               top_eval_seconds[i]);
      } else if (!sk) {
        printf("  %d. %s  win%%=%.1f%%  [%.3fs]\n", i + 1,
               string_builder_peek(sb), top_win_pcts[i] * 100.0,
               top_eval_seconds[i]);
      } else {
        printf("  %d. %s  win%%=%.1f%%  spread=%+.2f  [%.3fs]\n", i + 1,
               string_builder_peek(sb), top_win_pcts[i] * 100.0, top_values[i],
               top_eval_seconds[i]);
      }
      string_builder_destroy(sb);
      continue;
    }
    int score = equity_to_int(move_get_score(&top_moves[i]));
    double equity = equity_to_double(move_get_equity(&top_moves[i]));
    Rack leave;
    rack_copy(&leave, mover_rack);
    for (int j = 0; j < m.tiles_length; j++) {
      MachineLetter ml = m.tiles[j];
      if (ml != PLAYED_THROUGH_MARKER)
        rack_take_letter(&leave, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
    string_builder_add_formatted_string(sb, " %d  eq=%.1f  leave=", score,
                                        equity);
    string_builder_add_rack(sb, &leave, ld, false);
    if (top_pruned[i]) {
      printf("  %d. %s  win%%≤%.1f%%  [%.3fs]\n", i + 1,
             string_builder_peek(sb), top_win_pcts[i] * 100.0,
             top_eval_seconds[i]);
    } else if (!sk) {
      printf("  %d. %s  win%%=%.1f%%  [%.3fs]\n", i + 1,
             string_builder_peek(sb), top_win_pcts[i] * 100.0,
             top_eval_seconds[i]);
    } else {
      printf("  %d. %s  win%%=%.1f%%  spread=%+.2f  [%.3fs]\n", i + 1,
             string_builder_peek(sb), top_win_pcts[i] * 100.0, top_values[i],
             top_eval_seconds[i]);
    }
    string_builder_destroy(sb);
  }
}

static inline void peg_test_print_result(const PegResult *result,
                                         const Game *game) {
  Move m = result->best_move;
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), &m, game_get_ld(game),
                          false);
  const char *depth_label =
      result->stages_completed <= 1 ? "greedy" : "endgame";
  if (result->spread_known) {
    printf("  Best move: %s  win%%=%.1f%%  spread=%.2f  depth=%d (%s)\n",
           string_builder_peek(sb), result->best_win_pct * 100.0,
           result->best_expected_spread, result->stages_completed, depth_label);
  } else {
    printf("  Best move: %s  win%%=%.1f%%  depth=%d (%s)\n",
           string_builder_peek(sb), result->best_win_pct * 100.0,
           result->stages_completed, depth_label);
  }
  string_builder_destroy(sb);
}

#endif
