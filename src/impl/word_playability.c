#include "word_playability.h"

#include "../def/board_defs.h"
#include "../def/config_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/board.h"
#include "../ent/dictionary_word.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/words.h"
#include "../impl/kwg_maker.h"
#include "../impl/move_gen.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A move forms the main word plus one cross word per fresh tile, so at most
// RACK_SIZE + 1 words. We only need to know whether the count of a move's words
// that are absent from the small set is 0, 1, or "2 or more", so the missing
// counter saturates at 2.
enum {
  PB_MAX_WORDS = RACK_SIZE + 1,
  PB_MISSING_CAP = 2,
  PB_INITIAL_MOVE_INFO_CAP = 4096,
  // Cap on moves-above-baseline written per position in a dump. The highest
  // equity moves matter most as the selected set's baseline rises.
  PB_DUMP_MOVE_CAP = 32,
};

// Per-thread dump files are opened during single-threaded worker creation, so
// a plain counter (no atomics) gives each thread a unique <prefix>.<n>.
static int pb_dump_file_counter = 0;

// Per-move scratch derived once per position and shared by all three metrics.
typedef struct PBMoveInfo {
  Equity equity;
  game_event_t type;
  int num_words;                   // formed words found in the full lexicon
  uint32_t word_idx[PB_MAX_WORDS]; // their full-lexicon indices
  int missing_count;               // formed words not in the small set, max 2
  uint32_t missing_word;           // the single missing word when count == 1
} PBMoveInfo;

struct WordPlayabilityContext {
  DictionaryWordList *words; // every full-lexicon word, sorted
  uint32_t num_words;
  const KWG *small_kwg; // borrowed; NULL disables the bonus metric
  const LetterDistribution *ld;
  char *out_path;    // owned
  char *dump_prefix; // owned; when set, each thread dumps its positions to
                     // <dump_prefix>.<n> for the offline greedy. NULL = off.
  word_playability_sort_t sort;
};

struct WordPlayabilityCounts {
  uint32_t num_words;
  uint64_t *count;
  int64_t *penalty;
  int64_t *bonus; // NULL when the context has no small set
  // Diagnostics: positions analyzed and how many had a true bingo (a best move
  // placing all RACK_SIZE tiles) as the best play. A bingo is 7 tiles from the
  // rack, distinct from merely forming a 7+ letter word through existing tiles.
  uint64_t total_positions;
  uint64_t total_bingos;
  // Per-thread position dump (NULL unless the context has a dump_prefix). Each
  // record: int32 seed_baseline_equity, uint16 num_moves, then per move
  // int32 equity, uint8 num_words, uint32 word_idx[num_words]. Only positions
  // with >=1 move above the seed baseline are written.
  FILE *dump_file;
  // Generation-stamped dedup so each word accrues at most one bonus per
  // position (the highest-equity move that adds exactly that word), without an
  // O(num_words) reset each position.
  uint32_t *bonus_stamp;
  uint32_t bonus_gen;
  MoveList *move_list;
  PBMoveInfo *move_info;
  int move_info_cap;
};

// memcmp-then-length ordering, matching dictionary_word_compare, over unblanked
// machine letters.
static int pb_word_cmp(const MachineLetter *a, int alen, const MachineLetter *b,
                       int blen) {
  const int min_len = alen < blen ? alen : blen;
  const int cmp = memcmp(a, b, (size_t)min_len);
  if (cmp != 0) {
    return cmp;
  }
  return alen - blen;
}

// Index of an (already unblanked) word in the sorted full-lexicon table, or
// UINT32_MAX if absent (defensive; generated moves only form valid words).
static uint32_t pb_word_index(const WordPlayabilityContext *ctx,
                              const MachineLetter *word, int len) {
  int lo = 0;
  int hi = (int)ctx->num_words - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const DictionaryWord *dw = dictionary_word_list_get_word(ctx->words, mid);
    const int cmp = pb_word_cmp(dictionary_word_get_word(dw),
                                dictionary_word_get_length(dw), word, len);
    if (cmp == 0) {
      return (uint32_t)mid;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return UINT32_MAX;
}

// Membership of an (already unblanked) word in a KWG's DAWG.
static bool pb_kwg_contains(const KWG *kwg, const MachineLetter *word,
                            int len) {
  if (len <= 0) {
    return false;
  }
  uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
  for (int i = 0; i < len - 1; i++) {
    node_index = kwg_get_next_node_index(kwg, node_index, word[i]);
    if (node_index == 0) {
      return false;
    }
  }
  return kwg_in_letter_set(kwg, word[len - 1], node_index);
}

WordPlayabilityContext *
word_playability_context_create(const KWG *full_kwg, const KWG *small_kwg,
                                const LetterDistribution *ld, char *out_path,
                                word_playability_sort_t sort) {
  WordPlayabilityContext *ctx = malloc_or_die(sizeof(WordPlayabilityContext));
  ctx->words = dictionary_word_list_create();
  kwg_write_words(full_kwg, kwg_get_dawg_root_node_index(full_kwg), ctx->words,
                  NULL);
  dictionary_word_list_sort(ctx->words);
  ctx->num_words = (uint32_t)dictionary_word_list_get_count(ctx->words);
  ctx->small_kwg = small_kwg;
  ctx->ld = ld;
  ctx->out_path = out_path;
  ctx->dump_prefix = NULL;
  ctx->sort = sort;
  return ctx;
}

void word_playability_context_set_dump_prefix(WordPlayabilityContext *ctx,
                                              const char *dump_prefix) {
  free(ctx->dump_prefix);
  ctx->dump_prefix = dump_prefix ? string_duplicate(dump_prefix) : NULL;
}

// Writes every full-lexicon word, one per line in index order, so the offline
// greedy can map the dump's word ids back to words (and their lengths).
void word_playability_write_word_list(const WordPlayabilityContext *ctx,
                                      const char *path,
                                      ErrorStack *error_stack) {
  StringBuilder *sb = string_builder_create();
  for (uint32_t i = 0; i < ctx->num_words; i++) {
    const DictionaryWord *dw =
        dictionary_word_list_get_word(ctx->words, (int)i);
    const MachineLetter *word = dictionary_word_get_word(dw);
    const int len = dictionary_word_get_length(dw);
    for (int li = 0; li < len; li++) {
      char *hl = ld_ml_to_hl(ctx->ld, word[li]);
      string_builder_add_string(sb, hl);
      free(hl);
    }
    string_builder_add_string(sb, "\n");
  }
  write_string_to_file(path, "w", string_builder_peek(sb), error_stack);
  string_builder_destroy(sb);
}

void word_playability_context_destroy(WordPlayabilityContext *ctx) {
  if (!ctx) {
    return;
  }
  dictionary_word_list_destroy(ctx->words);
  free(ctx->out_path);
  free(ctx->dump_prefix);
  free(ctx);
}

bool word_playability_context_has_bonus(const WordPlayabilityContext *ctx) {
  return ctx->small_kwg != NULL;
}

uint32_t word_playability_context_num_words(const WordPlayabilityContext *ctx) {
  return ctx->num_words;
}

uint32_t word_playability_context_word_index(const WordPlayabilityContext *ctx,
                                             const MachineLetter *word,
                                             int len) {
  MachineLetter unblanked[BOARD_DIM] = {0};
  for (int i = 0; i < len; i++) {
    unblanked[i] = get_unblanked_machine_letter(word[i]);
  }
  return pb_word_index(ctx, unblanked, len);
}

WordPlayabilityCounts *
word_playability_counts_create(const WordPlayabilityContext *ctx) {
  WordPlayabilityCounts *counts = malloc_or_die(sizeof(WordPlayabilityCounts));
  counts->num_words = ctx->num_words;
  counts->count = calloc_or_die(ctx->num_words, sizeof(uint64_t));
  counts->penalty = calloc_or_die(ctx->num_words, sizeof(int64_t));
  if (ctx->small_kwg) {
    counts->bonus = calloc_or_die(ctx->num_words, sizeof(int64_t));
    counts->bonus_stamp = calloc_or_die(ctx->num_words, sizeof(uint32_t));
  } else {
    counts->bonus = NULL;
    counts->bonus_stamp = NULL;
  }
  counts->bonus_gen = 0;
  counts->total_positions = 0;
  counts->total_bingos = 0;
  counts->dump_file = NULL;
  if (ctx->dump_prefix) {
    char *fname =
        get_formatted_string("%s.%d", ctx->dump_prefix, pb_dump_file_counter++);
    counts->dump_file = fopen(fname, "wb");
    if (!counts->dump_file) {
      log_fatal("could not open playability dump file: %s", fname);
    }
    free(fname);
  }
  counts->move_list = move_list_create(DEFAULT_SMALL_MOVE_LIST_CAPACITY);
  counts->move_info_cap = PB_INITIAL_MOVE_INFO_CAP;
  counts->move_info =
      malloc_or_die((size_t)counts->move_info_cap * sizeof(PBMoveInfo));
  return counts;
}

void word_playability_counts_destroy(WordPlayabilityCounts *counts) {
  if (!counts) {
    return;
  }
  free(counts->count);
  free(counts->penalty);
  free(counts->bonus);
  free(counts->bonus_stamp);
  move_list_destroy(counts->move_list);
  free(counts->move_info);
  if (counts->dump_file) {
    fclose(counts->dump_file);
  }
  free(counts);
}

uint64_t word_playability_counts_get_count(const WordPlayabilityCounts *counts,
                                           uint32_t word_index) {
  return counts->count[word_index];
}

int64_t word_playability_counts_get_penalty(const WordPlayabilityCounts *counts,
                                            uint32_t word_index) {
  return counts->penalty[word_index];
}

int64_t word_playability_counts_get_bonus(const WordPlayabilityCounts *counts,
                                          uint32_t word_index) {
  return counts->bonus ? counts->bonus[word_index] : 0;
}

// True if move `info` forms the full-lexicon word `word_idx`.
static bool pb_move_forms(const PBMoveInfo *info, uint32_t word_idx) {
  for (int i = 0; i < info->num_words; i++) {
    if (info->word_idx[i] == word_idx) {
      return true;
    }
  }
  return false;
}

// Fills counts->move_info[0..n) from the generated move list, computing each
// move's formed-word indices and (if a small set exists) how many of its words
// are absent from it.
static void pb_fill_move_info(WordPlayabilityCounts *counts,
                              const WordPlayabilityContext *ctx, Board *board,
                              int n) {
  if (n > counts->move_info_cap) {
    free(counts->move_info);
    counts->move_info_cap = n;
    counts->move_info =
        malloc_or_die((size_t)counts->move_info_cap * sizeof(PBMoveInfo));
  }
  MachineLetter unblanked[BOARD_DIM];
  for (int i = 0; i < n; i++) {
    const Move *move = move_list_get_move(counts->move_list, i);
    PBMoveInfo *info = &counts->move_info[i];
    info->equity = move_get_equity(move);
    info->type = move_get_type(move);
    info->num_words = 0;
    info->missing_count = 0;
    info->missing_word = UINT32_MAX;
    if (info->type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue; // exchanges and passes form no words
    }
    FormedWords *fw = formed_words_create(board, move);
    const int num_formed = formed_words_get_num_words(fw);
    for (int wi = 0; wi < num_formed; wi++) {
      const MachineLetter *word = formed_words_get_word(fw, wi);
      const int len = formed_words_get_word_length(fw, wi);
      for (int li = 0; li < len; li++) {
        unblanked[li] = get_unblanked_machine_letter(word[li]);
      }
      const uint32_t idx = pb_word_index(ctx, unblanked, len);
      if (idx == UINT32_MAX) {
        continue; // defensive: generated moves only form valid words
      }
      if (info->num_words < PB_MAX_WORDS) {
        info->word_idx[info->num_words++] = idx;
      }
      if (ctx->small_kwg && info->missing_count < PB_MISSING_CAP &&
          !pb_kwg_contains(ctx->small_kwg, unblanked, len)) {
        info->missing_word = idx; // only used when missing_count ends at 1
        info->missing_count++;
      }
    }
    formed_words_destroy(fw);
  }
}

void word_playability_counts_add_position(WordPlayabilityCounts *counts,
                                          const WordPlayabilityContext *ctx,
                                          const Game *game) {
  MoveList *move_list = counts->move_list;
  move_list_reset(move_list);
  const MoveGenArgs args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .move_list = move_list,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&args);
  const int n = move_list_get_count(move_list);
  if (n <= 0) {
    return;
  }
  // MOVE_RECORD_ALL leaves the list as a min-heap; sort to equity-descending so
  // move 0 is the best and the metric scans run from highest to lowest equity.
  move_list_sort_moves(move_list);
  counts->total_positions++;
  const Move *played = move_list_get_move(move_list, 0);
  if (move_get_type(played) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
      move_get_tiles_played(played) == RACK_SIZE) {
    counts->total_bingos++; // a true bingo: all RACK_SIZE tiles placed
  }
  Board *board = game_get_board(game);
  pb_fill_move_info(counts, ctx, board, n);

  const PBMoveInfo *best = &counts->move_info[0];

  // Mode 1 (count) and Mode 2 (removal penalty) apply only when the best move
  // is a tile placement (exchanges/passes form no words).
  if (best->type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    for (int k = 0; k < best->num_words; k++) {
      counts->count[best->word_idx[k]]++;
    }
    for (int k = 0; k < best->num_words; k++) {
      const uint32_t word = best->word_idx[k];
      bool seen_earlier = false;
      for (int p = 0; p < k; p++) {
        if (best->word_idx[p] == word) {
          seen_earlier = true;
          break;
        }
      }
      if (seen_earlier) {
        continue;
      }
      // Highest-equity move forming no `word`; the pass sentinel equity is not
      // a real equity, so passing falls back to an alternative value of 0.
      int64_t alt_equity = 0;
      for (int i = 0; i < n; i++) {
        const PBMoveInfo *info = &counts->move_info[i];
        if (info->type == GAME_EVENT_PASS) {
          continue;
        }
        if (!pb_move_forms(info, word)) {
          alt_equity = (int64_t)info->equity;
          break;
        }
      }
      const int64_t loss = (int64_t)best->equity - alt_equity;
      if (loss > 0) {
        counts->penalty[word] += loss;
      }
    }
  }

  // Mode 3 (small-set + this word bonus). Baseline is the highest-equity move
  // playable with only the small set (no missing words). Moves above it that
  // become playable by adding exactly one word credit that word; moves at or
  // below the baseline cannot beat it, so the bonus is skipped there.
  if (counts->bonus) {
    int64_t baseline = 0;
    int baseline_index = n;
    for (int i = 0; i < n; i++) {
      const PBMoveInfo *info = &counts->move_info[i];
      if (info->type == GAME_EVENT_PASS) {
        continue;
      }
      if (info->missing_count == 0) {
        baseline = (int64_t)info->equity;
        baseline_index = i;
        break;
      }
    }

    // Dump the position for the offline greedy: the seed baseline and every
    // move above it (capped), each with its full formed-word-id set. The
    // offline greedy re-derives the per-set baseline and per-word bonus from
    // this, so no autoplay re-run is needed when the selected set changes.
    if (counts->dump_file && baseline_index > 0) {
      int num =
          baseline_index < PB_DUMP_MOVE_CAP ? baseline_index : PB_DUMP_MOVE_CAP;
      const int32_t baseline32 = (int32_t)baseline;
      const uint16_t num16 = (uint16_t)num;
      fwrite(&baseline32, sizeof(int32_t), 1, counts->dump_file);
      fwrite(&num16, sizeof(uint16_t), 1, counts->dump_file);
      for (int i = 0; i < num; i++) {
        const PBMoveInfo *info = &counts->move_info[i];
        const int32_t equity32 = (int32_t)info->equity;
        const uint8_t nwords = (uint8_t)info->num_words;
        fwrite(&equity32, sizeof(int32_t), 1, counts->dump_file);
        fwrite(&nwords, sizeof(uint8_t), 1, counts->dump_file);
        fwrite(info->word_idx, sizeof(uint32_t), info->num_words,
               counts->dump_file);
      }
    }

    counts->bonus_gen++;
    for (int i = 0; i < baseline_index; i++) {
      const PBMoveInfo *info = &counts->move_info[i];
      if (info->type == GAME_EVENT_PASS || info->missing_count != 1) {
        continue;
      }
      const uint32_t word = info->missing_word;
      if (counts->bonus_stamp[word] == counts->bonus_gen) {
        continue; // a higher-equity move already credited this word
      }
      counts->bonus_stamp[word] = counts->bonus_gen;
      const int64_t gain = (int64_t)info->equity - baseline;
      if (gain > 0) {
        counts->bonus[word] += gain;
      }
    }
  }
}

void word_playability_counts_merge(WordPlayabilityCounts *dst,
                                   const WordPlayabilityCounts *src) {
  for (uint32_t i = 0; i < dst->num_words; i++) {
    dst->count[i] += src->count[i];
    dst->penalty[i] += src->penalty[i];
  }
  if (dst->bonus && src->bonus) {
    for (uint32_t i = 0; i < dst->num_words; i++) {
      dst->bonus[i] += src->bonus[i];
    }
  }
  dst->total_positions += src->total_positions;
  dst->total_bingos += src->total_bingos;
}

uint64_t
word_playability_counts_get_positions(const WordPlayabilityCounts *counts) {
  return counts->total_positions;
}

uint64_t
word_playability_counts_get_bingos(const WordPlayabilityCounts *counts) {
  return counts->total_bingos;
}

typedef struct PBSortEntry {
  int64_t key;
  uint32_t word_index;
} PBSortEntry;

// Descending key; ties broken by ascending word index, which is alphabetical
// because the word table is sorted.
static int pb_sort_entry_cmp(const void *a, const void *b) {
  const PBSortEntry *ea = (const PBSortEntry *)a;
  const PBSortEntry *eb = (const PBSortEntry *)b;
  if (ea->key != eb->key) {
    return ea->key > eb->key ? -1 : 1;
  }
  if (ea->word_index != eb->word_index) {
    return ea->word_index < eb->word_index ? -1 : 1;
  }
  return 0;
}

void word_playability_write_csv(const WordPlayabilityContext *ctx,
                                const WordPlayabilityCounts *totals,
                                ErrorStack *error_stack) {
  const uint32_t num_words = ctx->num_words;
  PBSortEntry *entries = malloc_or_die((size_t)num_words * sizeof(PBSortEntry));
  for (uint32_t i = 0; i < num_words; i++) {
    entries[i].word_index = i;
    switch (ctx->sort) {
    case WORD_PLAYABILITY_SORT_PENALTY:
      entries[i].key = totals->penalty[i];
      break;
    case WORD_PLAYABILITY_SORT_BONUS:
      entries[i].key = totals->bonus ? totals->bonus[i] : 0;
      break;
    case WORD_PLAYABILITY_SORT_COUNT:
    default:
      entries[i].key = (int64_t)totals->count[i];
      break;
    }
  }
  qsort(entries, num_words, sizeof(PBSortEntry), pb_sort_entry_cmp);

  StringBuilder *sb = string_builder_create();
  if (totals->bonus) {
    string_builder_add_string(sb, "word,count,penalty_millipoints,"
                                  "bonus_millipoints\n");
  } else {
    string_builder_add_string(sb, "word,count,penalty_millipoints\n");
  }
  for (uint32_t e = 0; e < num_words; e++) {
    const uint32_t i = entries[e].word_index;
    const DictionaryWord *dw =
        dictionary_word_list_get_word(ctx->words, (int)i);
    const MachineLetter *word = dictionary_word_get_word(dw);
    const int len = dictionary_word_get_length(dw);
    for (int li = 0; li < len; li++) {
      char *hl = ld_ml_to_hl(ctx->ld, word[li]);
      string_builder_add_string(sb, hl);
      free(hl);
    }
    if (totals->bonus) {
      string_builder_add_formatted_string(
          sb, ",%llu,%lld,%lld\n", (unsigned long long)totals->count[i],
          (long long)totals->penalty[i], (long long)totals->bonus[i]);
    } else {
      string_builder_add_formatted_string(sb, ",%llu,%lld\n",
                                          (unsigned long long)totals->count[i],
                                          (long long)totals->penalty[i]);
    }
  }
  write_string_to_file(ctx->out_path, "w", string_builder_peek(sb),
                       error_stack);
  string_builder_destroy(sb);
  free(entries);
}
