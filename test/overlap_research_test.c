// Scratch research (not for a PR): how much work repeats across a
// position's sampled racks in the blocking/setup teacher.
#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/blocking_setup.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t candidate_lanes(const Board *board, const Move *candidate);

enum {
  OR_TABLE_BITS = 23,
  OR_LINE = 65536,
  OR_LIST = 200000,
};

typedef struct ORSlot {
  uint64_t a;
  uint64_t b;
  uint32_t epoch;
} ORSlot;

static ORSlot *or_tables[2];
static uint32_t or_epoch = 1;
static bool or_enabled = false;
static long or_total[2];
static long or_distinct[2];

static uint64_t or_mix(uint64_t x) {
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  x *= UINT64_C(0xc4ceb9fe1a85ec53);
  return x ^ (x >> 33);
}

// Inserts (a, b) into table kind; returns whether it was new this epoch.
static bool or_insert(int kind, uint64_t a, uint64_t b) {
  const uint64_t mask = ((uint64_t)1 << OR_TABLE_BITS) - 1;
  uint64_t slot = or_mix(a ^ or_mix(b)) & mask;
  ORSlot *table = or_tables[kind];
  while (table[slot].epoch == or_epoch) {
    if (table[slot].a == a && table[slot].b == b) {
      return false;
    }
    slot = (slot + 1) & mask;
  }
  table[slot].epoch = or_epoch;
  table[slot].a = a;
  table[slot].b = b;
  return true;
}

// The wmp.h hook is removed for timing; kept for reference.
void overlap_note(int kind, const BitRack *bit_rack, int word_length);
void overlap_note(int kind, const BitRack *bit_rack, int word_length) {
  if (!or_enabled) {
    return;
  }
  or_total[kind]++;
  uint64_t words[2];
  memcpy(words, bit_rack, sizeof(words));
  if (or_insert(kind, words[0] ^ ((uint64_t)word_length << 58), words[1])) {
    or_distinct[kind]++;
  }
}

static uint64_t or_move_key(const Move *move, uint64_t *second) {
  uint64_t a = ((uint64_t)move_get_row_start(move) << 56) |
               ((uint64_t)move_get_col_start(move) << 48) |
               ((uint64_t)move_get_dir(move) << 40) |
               ((uint64_t)move_get_tiles_length(move) << 32);
  uint64_t b = 0;
  for (int idx = 0; idx < move_get_tiles_length(move); idx++) {
    const uint64_t tile = move_get_tile(move, idx);
    if (idx < 4) {
      a ^= tile << (8 * idx);
    } else {
      b ^= tile << (8 * ((idx - 4) % 8));
      b = or_mix(b);
    }
  }
  *second = b;
  return a;
}

static void or_gen_all(const Game *game, MoveList *list, uint64_t lanes) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .lane_mask = lanes,
      .skip_exchanges = true,
      .disable_pat = true,
  };
  generate_moves(&args);
}

// Counts the plays all racks make on game (in lanes), and how many distinct.
static void or_play_overlap(const Game *board_game, Game *scratch,
                            const BlockingSetupSamples *samples, int num_racks,
                            uint64_t lanes, MoveList *list, long *sum,
                            long *distinct, double *ms) {
  game_copy(scratch, board_game);
  const int on_turn = game_get_player_on_turn_index(scratch);
  *sum = 0;
  *distinct = 0;
  or_epoch++;
  const int64_t start = ctimer_monotonic_ns();
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    rack_copy(player_get_rack(game_get_player(scratch, on_turn)),
              &samples->opponent_racks[rack_idx]);
    or_gen_all(scratch, list, lanes);
    for (int move_idx = 0; move_idx < move_list_get_count(list); move_idx++) {
      const Move *move = move_list_get_move(list, move_idx);
      if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      (*sum)++;
      uint64_t second = 0;
      const uint64_t first = or_move_key(move, &second);
      if (or_insert(1, first, second)) {
        (*distinct)++;
      }
    }
  }
  *ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
}

void overlap_research_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  const char *positions_path = string_splitter_get_item(fields, 0);
  const int num_racks =
      (int)strtol(string_splitter_get_item(fields, 1), NULL, 10);
  const long max_positions =
      strtol(string_splitter_get_item(fields, 2), NULL, 10);
  const int big_racks =
      (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
  for (int kind = 0; kind < 2; kind++) {
    or_tables[kind] = calloc_or_die((size_t)1 << OR_TABLE_BITS, sizeof(ORSlot));
  }
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  MoveList *list = move_list_create(OR_LIST);
  MoveList *all = move_list_create(OR_LIST);
  const int max_racks = num_racks > big_racks ? num_racks : big_racks;
  BlockingSetupSamples *samples =
      blocking_setup_samples_create(max_racks, 1024);
  BlockingSetupChecker *checker = blocking_setup_checker_create();
  FILE *in = fopen_or_die(positions_path, "r");
  char *line = malloc_or_die(OR_LINE);
  long positions = 0;
  long lookups = 0;
  long lookups_distinct = 0;
  long writes = 0;
  long writes_distinct = 0;
  long pass_sum = 0;
  long pass_distinct = 0;
  long pass_big_distinct = 0;
  long lane_sum = 0;
  long lane_distinct = 0;
  long lane_boards = 0;
  double pass_ms = 0;
  double lane_ms = 0;
  double teacher_ms = 0;
  Game *scratch = NULL;
  while (positions < max_positions && fgets(line, OR_LINE, in) != NULL) {
    char *end = NULL;
    const long game_idx = strtol(line, &end, 10);
    if (end == line) {
      continue;
    }
    char *cgp = line;
    for (int field_idx = 0; field_idx < 5 && cgp != NULL; field_idx++) {
      cgp = strchr(cgp, ',');
      if (cgp != NULL) {
        cgp++;
      }
    }
    cgp[strcspn(cgp, "\r\n")] = '\0';
    char *command = get_formatted_string("cgp %s", cgp);
    load_and_exec_config_or_die(config, command);
    free(command);
    const Game *game = config_get_game(config);
    if (bag_get_letters(game_get_bag(game)) == 0) {
      continue;
    }
    if (scratch == NULL) {
      scratch = game_duplicate(game);
    }
    // Candidates: the top 60 static placements.
    move_list_reset(all);
    const MoveGenArgs args = {
        .game = game,
        .move_list = all,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .disable_pat = true,
    };
    generate_moves(&args);
    move_list_sort_moves(all);
    const Move *cands[60];
    int count = 0;
    for (int move_idx = 0; move_idx < move_list_get_count(all) && count < 60;
         move_idx++) {
      if (move_get_type(move_list_get_move(all, move_idx)) ==
          GAME_EVENT_TILE_PLACEMENT_MOVE) {
        cands[count++] = move_list_get_move(all, move_idx);
      }
    }
    blocking_setup_samples_deal(samples, game, max_racks, true, true,
                                (uint64_t)game_idx * UINT64_C(7919) + 1);
    // 1. Word-map lookups during one full exact teacher run.
    BlockingSetupSamples teacher_samples = *samples;
    teacher_samples.num_racks = num_racks;
    or_epoch++;
    or_enabled = true;
    const long before_lookups = or_total[0];
    const long before_writes = or_total[1];
    const long before_distinct_lookups = or_distinct[0];
    const long before_distinct_writes = or_distinct[1];
    int64_t start = ctimer_monotonic_ns();
    blocking_setup_checker_load(checker, game, &teacher_samples, 1);
    for (int cand_idx = 0; cand_idx < count; cand_idx++) {
      BlockingSetupResult result;
      blocking_setup_checker_measure(checker, cands[cand_idx], &result);
    }
    teacher_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
    or_enabled = false;
    lookups += or_total[0] - before_lookups;
    writes += or_total[1] - before_writes;
    lookups_distinct += or_distinct[0] - before_distinct_lookups;
    writes_distinct += or_distinct[1] - before_distinct_writes;
    // 2. Plays all racks can make after our pass (opponent on turn).
    Game *passed = game_duplicate(game);
    Move *pass = move_create();
    move_set_as_pass(pass);
    play_move(pass, passed, NULL);
    long sum = 0;
    long distinct = 0;
    double ms = 0;
    or_play_overlap(passed, scratch, samples, num_racks, 0, list, &sum,
                    &distinct, &ms);
    pass_sum += sum;
    pass_distinct += distinct;
    pass_ms += ms;
    or_play_overlap(passed, scratch, samples, big_racks, 0, list, &sum,
                    &distinct, &ms);
    pass_big_distinct += distinct;
    // 3. The same in candidate lanes for the top 10 candidates.
    for (int cand_idx = 0; cand_idx < 10 && cand_idx < count; cand_idx++) {
      Game *after = game_duplicate(game);
      play_move(cands[cand_idx], after, NULL);
      const uint64_t lanes =
          candidate_lanes(game_get_board(after), cands[cand_idx]);
      or_play_overlap(after, scratch, samples, num_racks, lanes, list, &sum,
                      &distinct, &ms);
      lane_sum += sum;
      lane_distinct += distinct;
      lane_ms += ms;
      lane_boards++;
      game_destroy(after);
    }
    move_destroy(pass);
    game_destroy(passed);
    positions++;
  }
  printf("overlap positions=%ld racks=%d\n", positions, num_racks);
  printf("  teacher ms/position %.1f\n", teacher_ms / positions);
  printf("  wmp lookups/position %.0f distinct %.0f (repeat factor %.2f)\n",
         (double)lookups / positions, (double)lookups_distinct / positions,
         (double)lookups / (double)lookups_distinct);
  printf("  wmp word writes/position %.0f distinct %.0f (repeat %.2f)\n",
         (double)writes / positions, (double)writes_distinct / positions,
         (double)writes / (double)writes_distinct);
  printf("  pass board: plays summed over %d racks %.0f, distinct %.0f "
         "(sharing factor %.2f); distinct over %d racks %.0f; all-moves gen "
         "%.2f ms per rack\n",
         num_racks, (double)pass_sum / positions,
         (double)pass_distinct / positions,
         (double)pass_sum / (double)pass_distinct, big_racks,
         (double)pass_big_distinct / positions,
         pass_ms / positions / num_racks);
  printf("  candidate lanes: plays summed %.0f distinct %.0f per board "
         "(sharing %.2f); all-moves gen %.3f ms per rack per board\n",
         (double)lane_sum / lane_boards, (double)lane_distinct / lane_boards,
         (double)lane_sum / (double)lane_distinct,
         lane_ms / lane_boards / num_racks);
  free(line);
  (void)fclose(in);
  if (scratch != NULL) {
    game_destroy(scratch);
  }
  blocking_setup_checker_destroy(checker);
  blocking_setup_samples_destroy(samples);
  move_list_destroy(list);
  move_list_destroy(all);
  config_destroy(config);
  string_splitter_destroy(fields);
}

#include "../src/ent/wmp.h"
WMPMemoSlot *wmp_memo = NULL;
const WMPEntry *wmp_get_word_entry_uncached(const WMP *wmp,
                                            const BitRack *bit_rack,
                                            int word_length) {
  return wmp_get_word_entry_uncached_inline(wmp, bit_rack, word_length);
}
void overlap_memo_enable(void);
void overlap_memo_enable(void) {
  wmp_memo = calloc_or_die((size_t)1 << 20, sizeof(WMPMemoSlot));
}
