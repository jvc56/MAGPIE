// Aggregator: computes the full set of position features for the
// outcome_model. Decoupled from the game's bag — caller passes a
// reconstructed pre-draw pool so the function can be invoked at any
// point in autoplay's per-move loop.

#include "outcome_features.h"

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "bingo_prob.h"
#include "single_tile_play.h"
#include <stdint.h>
#include <string.h>

void outcome_features_compute(Game *game, MoveList *mv_list, int thread_index,
                              int us_player_index, const Rack *us_leave,
                              const uint8_t *pool_counts, int pool_size,
                              int bingo_samples, XoshiroPRNG *prng,
                              OutcomeFeatures *out) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const int us_idx = us_player_index;
  const int opp_idx = 1 - us_idx;

  uint8_t us_leave_counts[MAX_ALPHABET_SIZE] = {0};
  int us_leave_size = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    const int n = rack_get_letter(us_leave, (MachineLetter)ml);
    us_leave_counts[ml] = (uint8_t)n;
    us_leave_size += n;
  }
  const int us_draw_size = RACK_SIZE - us_leave_size;
  uint8_t empty_leave[MAX_ALPHABET_SIZE] = {0};

  const int saved_on_turn = game_get_player_on_turn_index(game);

  // ---- us features ----
  game_set_player_on_turn_index(game, us_idx);
  SingleTileScan scan_us;
  single_tile_scan(game, &scan_us);
  SingleTileFeatures st_us;
  single_tile_features(&scan_us, ld, us_leave_counts, pool_counts, pool_size,
                       us_draw_size, RACK_SIZE, &st_us);
  out->us_st_frac_playable = st_us.frac_playable;
  out->us_st_top1 = st_us.e_top1;
  out->us_st_top2 = st_us.e_top2;
  // bingo_prob_sampled saves and restores us's rack; we just need
  // on-turn = us so cross_set_index follows us's KWG when not shared.
  out->us_bingo_prob = bingo_prob_sampled(
      game, mv_list, thread_index, us_leave_counts, pool_counts, pool_size,
      us_draw_size, bingo_samples, prng);

  // ---- opp features ----
  game_set_player_on_turn_index(game, opp_idx);
  SingleTileScan scan_opp;
  single_tile_scan(game, &scan_opp);
  SingleTileFeatures st_opp;
  single_tile_features(&scan_opp, ld, empty_leave, pool_counts, pool_size,
                       RACK_SIZE, RACK_SIZE, &st_opp);
  out->opp_st_frac_playable = st_opp.frac_playable;
  out->opp_st_top1 = st_opp.e_top1;
  out->opp_st_top2 = st_opp.e_top2;
  out->opp_bingo_prob =
      bingo_prob_sampled(game, mv_list, thread_index, empty_leave, pool_counts,
                         pool_size, RACK_SIZE, bingo_samples, prng);

  game_set_player_on_turn_index(game, saved_on_turn);

  out->unplayed_blanks = pool_counts[BLANK_MACHINE_LETTER];
  out->tiles_unseen = pool_size;
  const Player *us_player = game_get_player(game, us_idx);
  const Player *opp_player = game_get_player(game, opp_idx);
  out->score_diff =
      (int)player_get_score(us_player) - (int)player_get_score(opp_player);
  // KLV value of us's leave (Equity millipoints). 0 if leave is empty
  // (full bingo) or no KLV is loaded for us.
  const KLV *us_klv = player_get_klv(us_player);
  out->us_leave_value = (int)klv_get_leave_value(us_klv, us_leave);
}
