#include "census_gen.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/ctime.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/board.h"
#include "../ent/dictionary_word.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/klv_csv.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "census.h"
#include "census_play_scores.h"
#include "gameplay.h"
#include "kwg_maker.h"
#include "move_gen.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // From a pool of this many unseen tiles, the superset sum (one sweep of the
  // lattice a letter, whatever the pool) apportions faster than pushing each
  // drawable rack into its subracks. wolges timed the crossover on English.
  CENSUS_SUPERSET_SUM_POOL_MIN = 36,
  // Up to this many multisets, scattering each word's score onto the racks
  // that spell it fills the racks' bests faster than searching every rack's
  // splits. wolges timed the 27 to 29 letter alphabets faster, the 33 letter
  // ones slower.
  CENSUS_SCATTER_MAX_LATTICE_SIZE = 12000000,
  // A board slot plays this many games looking for one that stops in the
  // window before giving up.
  CENSUS_MAX_TRIES_PER_BOARD = 100000,
};

// https://prng.di.unimi.it/splitmix64.c, as wolges's census_mix64
static uint64_t census_mix64(uint64_t value) {
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

// The recipe

static uint64_t *census_parse_recipe(const char *recipe, int *number_of_gens,
                                     ErrorStack *error_stack) {
  StringSplitter *parts = split_string(recipe, ',', false);
  const int number_of_parts = string_splitter_get_number_of_items(parts);
  int capacity = 0;
  int count = 0;
  uint64_t *board_counts = NULL;
  for (int part_idx = 0; part_idx < number_of_parts; part_idx++) {
    const char *part = string_splitter_get_item(parts, part_idx);
    uint64_t gens = 1;
    uint64_t boards = 0;
    const char *times = strchr(part, 'x');
    bool valid = true;
    if (times) {
      char *gens_str = string_duplicate(part);
      gens_str[times - part] = '\0';
      valid = is_all_digits_or_empty(gens_str) && gens_str[0] != '\0' &&
              is_all_digits_or_empty(times + 1) && times[1] != '\0';
      if (valid) {
        gens = string_to_uint64(gens_str, error_stack);
        boards = string_to_uint64(times + 1, error_stack);
      }
      free(gens_str);
    } else {
      valid = is_all_digits_or_empty(part) && part[0] != '\0';
      if (valid) {
        boards = string_to_uint64(part, error_stack);
      }
    }
    if (!valid || !error_stack_is_empty(error_stack) || gens == 0 ||
        boards == 0 || gens > 1000) {
      error_stack_reset(error_stack);
      error_stack_push(
          error_stack, ERROR_STATUS_CENSUS_MALFORMED_RECIPE,
          get_formatted_string("malformed census recipe '%s': expected board "
                               "counts like 3x256,2048",
                               recipe));
      break;
    }
    for (uint64_t gen_idx = 0; gen_idx < gens; gen_idx++) {
      if (count == capacity) {
        capacity = capacity ? capacity * 2 : 8;
        board_counts =
            realloc_or_die(board_counts, sizeof(uint64_t) * (size_t)capacity);
      }
      board_counts[count++] = boards;
    }
  }
  string_splitter_destroy(parts);
  if (!error_stack_is_empty(error_stack) || count == 0) {
    if (error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CENSUS_MALFORMED_RECIPE,
          get_formatted_string("empty census recipe '%s'", recipe));
    }
    free(board_counts);
    return NULL;
  }
  *number_of_gens = count;
  return board_counts;
}

// Shared state

// A board's play scores, kept for later generations as its nonzero entries
// only: most groups of tiles spell no word on a board.
typedef struct CensusPlayScoresCacheEntry {
  uint32_t *indexes;
  Equity *values;
  uint32_t number_of_entries;
  uint8_t unseen_tally[CENSUS_MAX_LETTERS];
} CensusPlayScoresCacheEntry;

static void play_scores_cache_entry_free(CensusPlayScoresCacheEntry *entry) {
  free(entry->indexes);
  free(entry->values);
  entry->indexes = NULL;
  entry->values = NULL;
  entry->number_of_entries = 0;
}

static void play_scores_cache_entry_store(CensusPlayScoresCacheEntry *entry,
                                          const Equity *play_scores,
                                          uint32_t lattice_size) {
  play_scores_cache_entry_free(entry);
  uint32_t number_of_entries = 0;
  for (uint32_t index = 0; index < lattice_size; index++) {
    number_of_entries += play_scores[index] != 0;
  }
  entry->indexes = malloc_or_die(sizeof(uint32_t) *
                                 (number_of_entries ? number_of_entries : 1));
  entry->values = malloc_or_die(sizeof(Equity) *
                                (number_of_entries ? number_of_entries : 1));
  uint32_t entry_idx = 0;
  for (uint32_t index = 0; index < lattice_size; index++) {
    if (play_scores[index] != 0) {
      entry->indexes[entry_idx] = index;
      entry->values[entry_idx] = play_scores[index];
      entry_idx++;
    }
  }
  entry->number_of_entries = number_of_entries;
}

static void
play_scores_cache_entry_load(const CensusPlayScoresCacheEntry *entry,
                             Equity *play_scores, uint32_t lattice_size) {
  memset(play_scores, 0, sizeof(Equity) * lattice_size);
  for (uint32_t entry_idx = 0; entry_idx < entry->number_of_entries;
       entry_idx++) {
    play_scores[entry->indexes[entry_idx]] = entry->values[entry_idx];
  }
}

typedef struct CensusShared {
  const CensusGenArgs *args;
  const CensusLattice *lattice;
  const CensusAddTable *add_table;
  uint32_t lattice_size;
  int low_tiles;
  int high_tiles;
  bool scatter;

  // this generation's
  const Equity *leave;
  bool null_leave;
  // the max over subsets of leave, when the boards scatter onto it
  Equity *leave_max_over_subsets;
  uint64_t gen_boards;
  // boards built in earlier generations, whose play scores are cached
  uint64_t prior_max_boards;
  // boards of this generation whose play scores later generations reuse
  uint64_t live_after;
  bool play_scores_reuse;
  CensusPlayScoresCacheEntry *play_scores_cache;

  cpthread_mutex_t mutex;
  uint64_t next_board;
  uint64_t completed_boards;
  bool failed;
  char *failure;
  double *sum;
  uint32_t *count;
} CensusShared;

typedef struct CensusWorker {
  CensusShared *shared;
  Game *game;
  MoveList *move_list;
  Equity *play_scores;
  Equity *rack_best;
  CensusSums *sums;
  uint8_t unseen_tally[CENSUS_MAX_LETTERS];
} CensusWorker;

// Plays fresh games until one stops with between target and high_tiles tiles
// on the board. Returns false if none does.
static bool census_play_board(CensusWorker *worker, uint64_t board_index,
                              int target) {
  const CensusShared *shared = worker->shared;
  Game *game = worker->game;
  const uint64_t base_seed =
      census_mix64(shared->args->seed + census_mix64(board_index));
  for (uint64_t try_idx = 0; try_idx < CENSUS_MAX_TRIES_PER_BOARD; try_idx++) {
    game_reset(game);
    game_seed(game,
              try_idx == 0 ? base_seed : census_mix64(base_seed + try_idx));
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int fill = board_get_tiles_played(game_get_board(game));
      if (fill >= target) {
        if (fill <= shared->high_tiles) {
          return true;
        }
        break;
      }
      const MoveGenArgs move_gen_args = {
          .game = game,
          .move_list = worker->move_list,
          .move_record_type = MOVE_RECORD_BEST,
          .move_sort_type =
              shared->args->null_start ? MOVE_SORT_SCORE : MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
      };
      generate_moves(&move_gen_args);
      play_move(move_list_get_move(worker->move_list, 0), game, NULL);
    }
  }
  return false;
}

static void census_value_board(CensusWorker *worker, uint64_t board_index) {
  CensusShared *shared = worker->shared;
  const uint32_t lattice_size = shared->lattice_size;
  const bool reuse_board =
      shared->play_scores_reuse && board_index < shared->prior_max_boards;
  CensusPlayScoresCacheEntry *cache_entry = NULL;
  if (shared->play_scores_reuse &&
      (reuse_board || board_index < shared->live_after)) {
    cache_entry = &shared->play_scores_cache[board_index];
  }
  if (reuse_board) {
    play_scores_cache_entry_load(cache_entry, worker->play_scores,
                                 lattice_size);
    memcpy(worker->unseen_tally, cache_entry->unseen_tally,
           sizeof(worker->unseen_tally));
  } else {
    const int window = shared->high_tiles - shared->low_tiles + 1;
    const int target =
        shared->low_tiles + (int)(board_index % (uint64_t)window);
    if (!census_play_board(worker, board_index, target)) {
      cpthread_mutex_lock(&shared->mutex);
      if (!shared->failed) {
        shared->failed = true;
        shared->failure = get_formatted_string(
            "census board %lu: no game stopped with %d to %d tiles on the "
            "board",
            (unsigned long)board_index, target, shared->high_tiles);
      }
      cpthread_mutex_unlock(&shared->mutex);
      return;
    }
    census_get_unseen_tally(worker->game, worker->unseen_tally);
    // A word only raises an entry: anything can be exchanged away for 0.
    memset(worker->play_scores, 0, sizeof(Equity) * lattice_size);
    census_play_scores_build(worker->game, shared->lattice,
                             worker->unseen_tally, worker->play_scores);
    if (cache_entry) {
      play_scores_cache_entry_store(cache_entry, worker->play_scores,
                                    lattice_size);
      memcpy(cache_entry->unseen_tally, worker->unseen_tally,
             sizeof(cache_entry->unseen_tally));
    }
  }

  int pool = 0;
  for (int ml = 0; ml < census_lattice_get_num_letters(shared->lattice); ml++) {
    pool += worker->unseen_tally[ml];
  }
  memset(worker->sums, 0, sizeof(CensusSums) * lattice_size);
  const CensusApportionArgs apportion_args = {
      .lattice = shared->lattice,
      .add_table = shared->add_table,
      .play_scores = worker->play_scores,
      .leave = shared->leave,
      .leave_max_over_subsets = shared->scatter && !shared->null_leave
                                    ? shared->leave_max_over_subsets
                                    : NULL,
      .unseen_tally = worker->unseen_tally,
      .null_leave = shared->null_leave,
      .superset_sum = pool >= CENSUS_SUPERSET_SUM_POOL_MIN,
      .scatter = shared->scatter,
  };
  census_apportion(&apportion_args, worker->sums, worker->rack_best);
  // The board's value of each leave, rounded to a millipoint, outside the
  // lock.
  CensusSums *sums = worker->sums;
  for (uint32_t index = 0; index < lattice_size; index++) {
    if (sums[index].den > 0.0) {
      sums[index].num = round(sums[index].num / sums[index].den);
    }
  }
  cpthread_mutex_lock(&shared->mutex);
  for (uint32_t index = 0; index < lattice_size; index++) {
    if (sums[index].den > 0.0) {
      shared->sum[index] += sums[index].num;
      shared->count[index]++;
    }
  }
  shared->completed_boards++;
  cpthread_mutex_unlock(&shared->mutex);
}

static void *census_worker_run(void *arg) {
  CensusWorker *worker = (CensusWorker *)arg;
  CensusShared *shared = worker->shared;
  while (true) {
    if (thread_control_get_status(shared->args->thread_control) ==
        THREAD_CONTROL_STATUS_USER_INTERRUPT) {
      break;
    }
    cpthread_mutex_lock(&shared->mutex);
    const bool stop =
        shared->failed || shared->next_board >= shared->gen_boards;
    const uint64_t board_index = shared->next_board++;
    cpthread_mutex_unlock(&shared->mutex);
    if (stop) {
      break;
    }
    census_value_board(worker, board_index);
  }
  return NULL;
}

// Output

typedef struct CensusLeaveWriter {
  const CensusLattice *lattice;
  const bool *ever_valued;
  DictionaryWordList *dwl;
  uint8_t tally[CENSUS_MAX_LETTERS];
  MachineLetter word[RACK_SIZE];
} CensusLeaveWriter;

// Adds every valued leave of 1 to RACK_SIZE - 1 tiles, in alphabetical order,
// from letter ml on.
static void census_add_valued_leaves(CensusLeaveWriter *writer, int length,
                                     int ml, const uint8_t *available) {
  const int num_letters = census_lattice_get_num_letters(writer->lattice);
  if (length > 0) {
    const uint32_t index = census_lattice_rank(writer->lattice, writer->tally);
    if (writer->ever_valued[index]) {
      dictionary_word_list_add_word(writer->dwl, writer->word, length);
    }
  }
  if (length == RACK_SIZE - 1) {
    return;
  }
  for (int next_ml = ml; next_ml < num_letters; next_ml++) {
    if (writer->tally[next_ml] < available[next_ml]) {
      writer->tally[next_ml]++;
      writer->word[length] = (MachineLetter)next_ml;
      census_add_valued_leaves(writer, length + 1, next_ml, available);
      writer->tally[next_ml]--;
    }
  }
}

// Writes the valued leaves to data/leaves as klv_name, and as a CSV beside it
// if write_csv; a leave never valued is left out, and so reads as 0.
static void census_write_klv(const CensusShared *shared,
                             const LetterDistribution *ld, const double *values,
                             const bool *ever_valued, const char *klv_name,
                             bool write_csv, ErrorStack *error_stack) {
  const int num_letters = census_lattice_get_num_letters(shared->lattice);
  uint8_t available[CENSUS_MAX_LETTERS] = {0};
  for (int ml = 0; ml < num_letters; ml++) {
    available[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  CensusLeaveWriter writer = {.lattice = shared->lattice,
                              .ever_valued = ever_valued,
                              .dwl = dictionary_word_list_create()};
  memset(writer.tally, 0, sizeof(writer.tally));
  census_add_valued_leaves(&writer, 0, 0, available);
  const int number_of_leaves = dictionary_word_list_get_count(writer.dwl);
  KWG *kwg = make_kwg_from_words(writer.dwl, KWG_MAKER_OUTPUT_DAWG,
                                 KWG_MAKER_MERGE_EXACT);
  KLV *klv = klv_create_zeroed_from_kwg(kwg, number_of_leaves, klv_name);
  Rack leave;
  rack_set_dist_size_and_reset(&leave, num_letters);
  for (int word_idx = 0; word_idx < number_of_leaves; word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(writer.dwl, word_idx);
    const MachineLetter *letters = dictionary_word_get_word(word);
    const int length = dictionary_word_get_length(word);
    uint8_t tally[CENSUS_MAX_LETTERS];
    memset(tally, 0, sizeof(tally));
    rack_reset(&leave);
    for (int letter_idx = 0; letter_idx < length; letter_idx++) {
      tally[letters[letter_idx]]++;
      rack_add_letter(&leave, letters[letter_idx]);
    }
    const uint32_t index = census_lattice_rank(shared->lattice, tally);
    klv_set_indexed_leave_value(klv, klv_get_word_index(klv, &leave),
                                (Equity)llround(values[index]));
  }
  dictionary_word_list_destroy(writer.dwl);
  klv_write(klv, shared->args->data_paths, klv_name, error_stack);
  if (write_csv && error_stack_is_empty(error_stack)) {
    klv_write_to_csv(klv, ld, shared->args->data_paths, klv_name, NULL,
                     error_stack);
  }
  klv_destroy(klv);
}

// Checks what the census does not model.
static void census_check_game(const Game *game, ErrorStack *error_stack) {
  if (game_get_variant(game) != GAME_VARIANT_CLASSIC) {
    error_stack_push(
        error_stack, ERROR_STATUS_CENSUS_UNSUPPORTED_GAME,
        string_duplicate("the census supports only the classic game variant"));
    return;
  }
  if (!game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG)) {
    error_stack_push(error_stack, ERROR_STATUS_CENSUS_UNSUPPORTED_GAME,
                     string_duplicate("cannot run a census with different "
                                      "lexica for the players"));
    return;
  }
  const int ld_size = ld_get_size(game_get_ld(game));
  if (ld_size > CENSUS_MAX_LETTERS) {
    error_stack_push(
        error_stack, ERROR_STATUS_CENSUS_UNSUPPORTED_GAME,
        get_formatted_string("the census supports at most %d letters, not %d",
                             CENSUS_MAX_LETTERS, ld_size));
  }
}

void census_gen(const CensusGenArgs *args, ErrorStack *error_stack) {
  census_check_game(args->game, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  int number_of_gens = 0;
  uint64_t *board_counts =
      census_parse_recipe(args->recipe, &number_of_gens, error_stack);
  if (!board_counts) {
    return;
  }
  const LetterDistribution *ld = game_get_ld(args->game);
  const int num_letters = ld_get_size(ld);
  int num_tiles = 0;
  for (int ml = 0; ml < num_letters; ml++) {
    num_tiles += ld_get_dist(ld, ml);
  }
  // The window keeps a tile in the bag: below it the leave table is not
  // consulted, and nothing is left to draw or exchange for.
  const int racks_tiles = 2 * RACK_SIZE;
  const int low_tiles = racks_tiles;
  const int high_tiles = num_tiles - racks_tiles - 1;
  if (high_tiles < low_tiles) {
    error_stack_push(
        error_stack, ERROR_STATUS_CENSUS_UNSUPPORTED_GAME,
        get_formatted_string("%d tiles are too few for a census", num_tiles));
    free(board_counts);
    return;
  }

  CensusLattice *lattice =
      census_lattice_create(num_letters, RACK_SIZE, error_stack);
  if (!lattice) {
    free(board_counts);
    return;
  }
  const uint32_t lattice_size = census_lattice_get_size(lattice);
  Timer timer;
  ctimer_start(&timer);
  CensusAddTable *add_table =
      census_add_table_create(lattice, args->number_of_threads);

  // Which boards' play scores later generations reuse (wolges's
  // census_sheet_reuse_plan).
  uint64_t *live_after =
      calloc_or_die((size_t)number_of_gens, sizeof(uint64_t));
  for (int gen_idx = number_of_gens - 2; gen_idx >= 0; gen_idx--) {
    live_after[gen_idx] = board_counts[gen_idx + 1] > live_after[gen_idx + 1]
                              ? board_counts[gen_idx + 1]
                              : live_after[gen_idx + 1];
  }
  uint64_t play_scores_cache_length = 0;
  uint64_t max_boards = 0;
  for (int gen_idx = 0; gen_idx < number_of_gens; gen_idx++) {
    const uint64_t cached = board_counts[gen_idx] < live_after[gen_idx]
                                ? board_counts[gen_idx]
                                : live_after[gen_idx];
    if (cached > play_scores_cache_length) {
      play_scores_cache_length = cached;
    }
    if (board_counts[gen_idx] > max_boards) {
      max_boards = board_counts[gen_idx];
    }
  }
  const bool play_scores_reuse = number_of_gens > 1;

  // The first generation's leaves: zero, or the game's.
  Equity *leave = calloc_or_die(lattice_size, sizeof(Equity));
  if (!args->null_start) {
    const KLV *klv = player_get_klv(game_get_player(args->game, 0));
    uint8_t tally[CENSUS_MAX_LETTERS];
    Rack leave_rack;
    rack_set_dist_size_and_reset(&leave_rack, num_letters);
    for (uint32_t index = census_lattice_get_size_start(lattice, 1);
         index < census_lattice_get_full_rack_start(lattice); index++) {
      census_lattice_unrank(lattice, index, tally);
      rack_reset(&leave_rack);
      for (int ml = 0; ml < num_letters; ml++) {
        rack_add_letters(&leave_rack, (MachineLetter)ml, tally[ml]);
      }
      leave[index] = klv_get_leave_value(klv, &leave_rack);
    }
  }
  // The leaves as the means found them, before the rounding the next
  // generation plays with: what the census writes.
  double *exact = calloc_or_die(lattice_size, sizeof(double));
  for (uint32_t index = 0; index < lattice_size; index++) {
    exact[index] = (double)leave[index];
  }
  bool *ever_valued = calloc_or_die(lattice_size, sizeof(bool));

  CensusShared shared = {
      .args = args,
      .lattice = lattice,
      .add_table = add_table,
      .lattice_size = lattice_size,
      .low_tiles = low_tiles,
      .high_tiles = high_tiles,
      .scatter = lattice_size <= CENSUS_SCATTER_MAX_LATTICE_SIZE,
      .leave = leave,
      .leave_max_over_subsets = malloc_or_die(sizeof(Equity) * lattice_size),
      .play_scores_reuse = play_scores_reuse,
      .play_scores_cache =
          play_scores_reuse
              ? calloc_or_die(play_scores_cache_length > 0
                                  ? (size_t)play_scores_cache_length
                                  : 1,
                              sizeof(CensusPlayScoresCacheEntry))
              : NULL,
      .sum = calloc_or_die(lattice_size, sizeof(double)),
      .count = calloc_or_die(lattice_size, sizeof(uint32_t)),
  };
  cpthread_mutex_init(&shared.mutex);

  int number_of_threads =
      args->number_of_threads < 1 ? 1 : args->number_of_threads;
  if ((uint64_t)number_of_threads > max_boards) {
    number_of_threads = (int)max_boards;
  }
  CensusWorker *workers =
      calloc_or_die((size_t)number_of_threads, sizeof(CensusWorker));
  for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
    CensusWorker *worker = &workers[thread_idx];
    worker->shared = &shared;
    worker->game = game_duplicate(args->game);
    worker->move_list = move_list_create(1);
    worker->play_scores = malloc_or_die(sizeof(Equity) * lattice_size);
    worker->rack_best = malloc_or_die(sizeof(Equity) * lattice_size);
    worker->sums = malloc_or_die(sizeof(CensusSums) * lattice_size);
  }
  cpthread_t *threads =
      malloc_or_die(sizeof(cpthread_t) * (size_t)number_of_threads);

  thread_control_print_formatted(
      args->thread_control,
      "census: %u multisets of %d letters and %d tiles, boards of %d to %d "
      "tiles, %d generations on %d threads, add table built in %.1fs\n",
      lattice_size, num_letters, RACK_SIZE, low_tiles, high_tiles,
      number_of_gens, number_of_threads, ctimer_elapsed_seconds(&timer));

  const uint32_t empty_index = 0;
  bool interrupted = false;
  for (int gen_idx = 0; gen_idx < number_of_gens; gen_idx++) {
    bool null_leave = true;
    for (uint32_t index = 0; index < lattice_size; index++) {
      if (leave[index] != 0) {
        null_leave = false;
        break;
      }
    }
    shared.null_leave = null_leave;
    if (shared.scatter && !null_leave) {
      census_max_over_subsets(lattice, add_table, leave,
                              shared.leave_max_over_subsets);
    }
    shared.gen_boards = board_counts[gen_idx];
    shared.live_after = live_after[gen_idx];
    shared.next_board = 0;
    shared.completed_boards = 0;
    memset(shared.sum, 0, sizeof(double) * lattice_size);
    memset(shared.count, 0, sizeof(uint32_t) * lattice_size);

    for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
      cpthread_create(&threads[thread_idx], census_worker_run,
                      &workers[thread_idx]);
    }
    for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
      cpthread_join(threads[thread_idx]);
    }
    if (shared.failed) {
      error_stack_push(error_stack, ERROR_STATUS_CENSUS_UNSUPPORTED_GAME,
                       shared.failure);
      shared.failure = NULL;
      break;
    }
    if (shared.completed_boards < shared.gen_boards) {
      interrupted = true;
      break;
    }
    if (shared.prior_max_boards < shared.gen_boards) {
      shared.prior_max_boards = shared.gen_boards;
    }

    // The empty leave's mean is the average best full-rack equity: the
    // baseline every leave is measured from. A leave this generation did not
    // value keeps its value from the one before.
    const double base =
        shared.count[empty_index] > 0
            ? shared.sum[empty_index] / (double)shared.count[empty_index]
            : 0.0;
    uint32_t number_valued = 0;
    for (uint32_t index = 0; index < lattice_size; index++) {
      if (shared.count[index] > 0) {
        ever_valued[index] = true;
        exact[index] = shared.sum[index] / (double)shared.count[index] - base;
        leave[index] = (Equity)llround(exact[index]);
        number_valued++;
      }
    }
    // Play scores no later generation reads.
    if (shared.play_scores_cache) {
      for (uint64_t board_idx = live_after[gen_idx];
           board_idx < play_scores_cache_length; board_idx++) {
        play_scores_cache_entry_free(&shared.play_scores_cache[board_idx]);
      }
    }
    char *gen_name =
        get_formatted_string("%s_gen_%d", args->output_name, gen_idx + 1);
    census_write_klv(&shared, ld, exact, ever_valued, gen_name, false,
                     error_stack);
    thread_control_print_formatted(
        args->thread_control,
        "census: generation %d of %d, %lu boards, %u multisets valued, "
        "baseline %.3f, wrote %s (%.1fs)\n",
        gen_idx + 1, number_of_gens, (unsigned long)shared.gen_boards,
        number_valued, base / 1000.0, gen_name, ctimer_elapsed_seconds(&timer));
    free(gen_name);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }
  }

  if (error_stack_is_empty(error_stack) && !interrupted) {
    census_write_klv(&shared, ld, exact, ever_valued, args->output_name, true,
                     error_stack);
    if (error_stack_is_empty(error_stack)) {
      thread_control_print_formatted(
          args->thread_control, "census: wrote %s (%.1fs)\n", args->output_name,
          ctimer_elapsed_seconds(&timer));
    }
  }

  free(threads);
  for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
    CensusWorker *worker = &workers[thread_idx];
    game_destroy(worker->game);
    move_list_destroy(worker->move_list);
    free(worker->play_scores);
    free(worker->rack_best);
    free(worker->sums);
  }
  free(workers);
  free(shared.leave_max_over_subsets);
  if (shared.play_scores_cache) {
    for (uint64_t board_idx = 0; board_idx < play_scores_cache_length;
         board_idx++) {
      play_scores_cache_entry_free(&shared.play_scores_cache[board_idx]);
    }
    free(shared.play_scores_cache);
  }
  free(shared.failure);
  free(shared.sum);
  free(shared.count);
  free(ever_valued);
  free(exact);
  free(leave);
  free(live_after);
  census_add_table_destroy(add_table);
  census_lattice_destroy(lattice);
  free(board_counts);
}
