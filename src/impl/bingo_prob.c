// Sample-based bingo probability for outcome_model feature extraction.

#include "bingo_prob.h"

#include "../def/equity_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../ent/letter_distribution.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "move_gen.h"
#include <stdint.h>
#include <string.h>

static void sample_one_rack(XoshiroPRNG *prng, const uint8_t *pool_counts,
                            int pool_size, int draw_size, uint8_t *drawn,
                            uint16_t *scratch) {
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    scratch[ml] = pool_counts[ml];
    drawn[ml] = 0;
  }
  int remaining = pool_size;
  for (int i = 0; i < draw_size; i++) {
    const uint64_t pick = prng_get_random_number(prng, (uint64_t)remaining);
    uint64_t cum = 0;
    for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
      cum += scratch[ml];
      if (pick < cum) {
        drawn[ml]++;
        scratch[ml]--;
        remaining--;
        break;
      }
    }
  }
}

double bingo_prob_sampled(Game *game, MoveList *mv_list, int thread_index,
                          const uint8_t *leave_counts,
                          const uint8_t *pool_counts, int pool_size,
                          int draw_size, int num_samples, XoshiroPRNG *prng) {
  if (num_samples <= 0) {
    return 0.0;
  }
  if (draw_size > 0 && pool_size < draw_size) {
    return 0.0;
  }

  const int player_idx = game_get_player_on_turn_index(game);
  Player *player = game_get_player(game, player_idx);
  Rack *rack = player_get_rack(player);
  const int ld_size = ld_get_size(game_get_ld(game));

  Rack saved_rack;
  rack_set_dist_size(&saved_rack, ld_size);
  rack_copy(&saved_rack, rack);

  uint8_t drawn[MAX_ALPHABET_SIZE];
  uint16_t scratch[MAX_ALPHABET_SIZE];

  const MoveGenArgs args = {
      .game = game,
      .move_list = mv_list,
      .move_record_type = MOVE_RECORD_BINGO_EXISTS,
      .move_sort_type = MOVE_SORT_SCORE,
      .thread_index = thread_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  int bingo_count = 0;
  for (int s = 0; s < num_samples; s++) {
    if (draw_size > 0) {
      sample_one_rack(prng, pool_counts, pool_size, draw_size, drawn, scratch);
    } else {
      memset(drawn, 0, sizeof(drawn));
    }
    rack_reset(rack);
    for (int ml = 0; ml < ld_size; ml++) {
      const int n = (int)leave_counts[ml] + (int)drawn[ml];
      if (n > 0) {
        rack_add_letters(rack, (MachineLetter)ml, n);
      }
    }
    if (bingo_exists(&args)) {
      bingo_count++;
    }
  }

  rack_copy(rack, &saved_rack);
  return (double)bingo_count / (double)num_samples;
}
