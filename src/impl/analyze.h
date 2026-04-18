#ifndef ANALYZE_H
#define ANALYZE_H

#include "../ent/game_history.h"
#include "../ent/sim_args.h"
#include "../impl/endgame.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct AnalyzeCtx AnalyzeCtx;

// All inputs required by analyze_game. Config.c is responsible for filling
// each field before calling analyze_game; analyze_with_sim and
// analyze_with_endgame set the remaining per-turn fields (game, move_list,
// known_opp_rack) on sim_args and endgame_args before forwarding to simulate
// and endgame_solve.
//
// sim_args.num_plies == 0 selects static analysis (move generation only,
// no simulation or endgame solving).
typedef struct AnalyzeArgs {
  GameHistory *game_history;
  uint64_t player_mask;    // 0 = all players; pre-resolved by the caller
  const char *report_path; // output file path

  SimArgs sim_args;
  EndgameArgs endgame_args;

  bool human_readable;
  int max_num_display_plays;
  const char *config_settings_str; // non-owning; set by caller, freed by caller
} AnalyzeArgs;

AnalyzeCtx *analyze_ctx_create(void);
void analyze_ctx_destroy(AnalyzeCtx *ctx);

// Analyzes all scorable turns in game_history. Creates *analyze_ctx if NULL
// on entry; the caller is responsible for calling analyze_ctx_destroy after
// this returns. Reusing the same ctx across multiple calls (directory mode)
// preserves internal allocations for efficiency.
void analyze_game(AnalyzeArgs *analyze_args, AnalyzeCtx **analyze_ctx,
                  ErrorStack *error_stack);

#endif
