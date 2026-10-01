#include "blocking_setup_gen_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_eval.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/blocking_setup.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "blocking_setup_options.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Label generation for per-lexicon blocking/setup parameters. Three stages,
// each an on-demand test "bsgen:<stage>[:key=value...]", each sharded with
// worker=<i>:workers=<n> so independent processes split the work:
//
//   positions  Plays games=<n> independent no-PAT static games for lex and
//              keeps one position per game: game g's stratum is g mod 3
//              (early: bag >= early, middle: bag in [middle, early),
//              late: bag in [minbag, middle)), and the turn is uniform over
//              that stratum's turns. Rows: game,turn,bag,lead,phase,cgp.
//              Games are seeded by seed and the game index only, so every
//              shard's games are the same games.
//   labels     For each position in in=: the candidate universe (the top
//              universe= no-PAT static non-pass moves, plus up to exchanges=
//              exchanges within exchmargin= points of the top move, plus the
//              top patn= moves under pat= when given), and each candidate's
//              pass-relative blocking and setup deltas (blocking_setup.h) at
//              each rack count in racks= (nested prefixes of one deal of the
//              largest). Rows per candidate; timing per position goes to
//              out=<path>.timing (CSV).
//   refs       For each position in in= and its universe from labels=, a
//              round-robin reference simulation of every universe candidate:
//              plies=, ms= per position, no-PAT static rollouts, seed=. With
//              cands=<file> of "game,cand" lines (bs_fit.py choices), only
//              those candidates are simulated, so a few contenders get the
//              whole budget; positions with fewer than two are skipped.
//
// Only what the player on turn can see enters the labels: the board, their
// rack, the scores and the unseen pool. The positions file keeps both racks
// (CGP) so the games can be audited, but the teacher replaces the opponent's
// rack with samples.
enum {
  BSG_MAX_RACK_COUNTS = 8,
  BSG_MAX_UNIVERSE = 512,
  BSG_POOL_CAPACITY = 256,
  BSG_LINE_CAPACITY = 65536,
  BSG_MAX_TURNS = 256,
  BSG_MOVE_LIST_CAPACITY = 100000,
  BSG_DEFAULT_GAMES = 100,
  BSG_DEFAULT_EARLY_BAG = 60,
  BSG_DEFAULT_MIDDLE_BAG = 30,
  BSG_DEFAULT_MIN_BAG = 7,
  BSG_DEFAULT_UNIVERSE = 60,
  BSG_DEFAULT_EXCHANGES = 5,
  BSG_DEFAULT_EXCHANGE_MARGIN = 35,
  BSG_DEFAULT_PAT_NOMINEES = 25,
  BSG_DEFAULT_PLIES = 4,
  BSG_DEFAULT_REF_MS = 10000,
  BSG_DEFAULT_MIN_PLAY_ITERATIONS = 30,
};

static double bsg_now_ms(void) {
  return (double)ctimer_monotonic_ns() / 1000000.0;
}

// "set -lex L -leaves K -wmp W ..." with pat appended when given.
static Config *bsg_config_create(const BSOptions *options, const char *pat) {
  const char *lexicon = bs_options_require(options, "lex");
  char *settings = get_formatted_string(
      "set -lex %s -leaves %s -wmp %s -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1%s%s",
      lexicon, bs_options_get(options, "leaves", lexicon),
      bs_options_get(options, "wmp", "true"), pat != NULL ? " -pat " : "",
      pat != NULL ? pat : "");
  Config *config = config_create_or_die(settings);
  free(settings);
  return config;
}

static int bsg_lead(const Game *game) {
  const int on_turn = game_get_player_on_turn_index(game);
  return equity_to_int(player_get_score(game_get_player(game, on_turn)) -
                       player_get_score(game_get_player(game, 1 - on_turn)));
}

static void bsg_positions(const BSOptions *options) {
  const long games = bs_options_get_long(options, "games", BSG_DEFAULT_GAMES);
  const long first_game = bs_options_get_long(options, "first", 0);
  const long worker = bs_options_get_long(options, "worker", 0);
  const long workers = bs_options_get_long(options, "workers", 1);
  const uint64_t seed = bs_options_get_u64(options, "seed", 0);
  const int early =
      (int)bs_options_get_long(options, "early", BSG_DEFAULT_EARLY_BAG);
  const int middle =
      (int)bs_options_get_long(options, "middle", BSG_DEFAULT_MIDDLE_BAG);
  const int min_bag =
      (int)bs_options_get_long(options, "minbag", BSG_DEFAULT_MIN_BAG);
  assert(min_bag >= 1 && min_bag <= middle && middle <= early);
  Config *config = bsg_config_create(options, NULL);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *list = move_list_create(1);
  FILE *out = fopen_or_die(bs_options_require(options, "out"), "w");
  (void)fprintf(out, "game,turn,bag,lead,phase,cgp\n");
  const char *phase_names[] = {"early", "middle", "late"};
  for (long game_idx = first_game; game_idx < first_game + games; game_idx++) {
    if (game_idx % workers != worker) {
      continue;
    }
    const uint64_t game_seed_value = bs_mix(seed ^ bs_mix((uint64_t)game_idx));
    game_reset(game);
    game_seed(game, game_seed_value);
    game_set_starting_player_index(game, (int)(game_idx % 2));
    draw_starting_racks(game);
    const int phase = (int)(game_idx % 3);
    char *eligible[BSG_MAX_TURNS];
    int eligible_turns[BSG_MAX_TURNS];
    int eligible_bags[BSG_MAX_TURNS];
    int eligible_leads[BSG_MAX_TURNS];
    int num_eligible = 0;
    int turn = 0;
    while (!game_over(game) && turn < BSG_MAX_TURNS) {
      const int bag = bag_get_letters(game_get_bag(game));
      const bool in_phase = (phase == 0 && bag >= early) ||
                            (phase == 1 && bag >= middle && bag < early) ||
                            (phase == 2 && bag >= min_bag && bag < middle);
      if (in_phase) {
        eligible[num_eligible] = game_get_cgp(game, true);
        eligible_turns[num_eligible] = turn;
        eligible_bags[num_eligible] = bag;
        eligible_leads[num_eligible] = bsg_lead(game);
        num_eligible++;
      }
      play_move(get_top_equity_move(game, list), game, NULL);
      turn++;
    }
    if (num_eligible > 0) {
      XoshiroPRNG *prng = prng_create(bs_mix(game_seed_value));
      const int pick =
          (int)prng_get_random_number(prng, (uint64_t)num_eligible);
      prng_destroy(prng);
      (void)fprintf(out, "%ld,%d,%d,%d,%s,%s\n", game_idx, eligible_turns[pick],
                    eligible_bags[pick], eligible_leads[pick],
                    phase_names[phase], eligible[pick]);
      (void)fflush(out);
    }
    for (int eligible_idx = 0; eligible_idx < num_eligible; eligible_idx++) {
      free(eligible[eligible_idx]);
    }
  }
  (void)fclose(out);
  move_list_destroy(list);
  config_destroy(config);
}

static void bsg_generate(const Game *game, MoveList *list, bool disable_pat) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .disable_pat = disable_pat,
  };
  generate_moves(&args);
  move_list_sort_moves(list);
}

static int bsg_find(const MoveList *list, const Move *move) {
  for (int move_idx = 0; move_idx < move_list_get_count(list); move_idx++) {
    if (compare_moves_without_equity(move, move_list_get_move(list, move_idx),
                                     true) == -1) {
      return move_idx;
    }
  }
  return -1;
}

// One position's candidate universe: indices into the static list, in
// admission order (static prefix, then exchanges, then PAT nominees).
typedef struct BSGUniverse {
  int count;
  int static_index[BSG_MAX_UNIVERSE];
  int pat_rank[BSG_MAX_UNIVERSE];
  const char *source[BSG_MAX_UNIVERSE];
} BSGUniverse;

static bool bsg_universe_has(const BSGUniverse *universe, int static_index) {
  for (int idx = 0; idx < universe->count; idx++) {
    if (universe->static_index[idx] == static_index) {
      return true;
    }
  }
  return false;
}

static void bsg_universe_add(BSGUniverse *universe, int static_index,
                             const char *source) {
  if (bsg_universe_has(universe, static_index)) {
    return;
  }
  assert(universe->count < BSG_MAX_UNIVERSE);
  universe->static_index[universe->count] = static_index;
  universe->pat_rank[universe->count] = 0;
  universe->source[universe->count] = source;
  universe->count++;
}

static void bsg_build_universe(const BSOptions *options,
                               const MoveList *static_list,
                               const MoveList *pat_list,
                               BSGUniverse *universe) {
  const int universe_size =
      (int)bs_options_get_long(options, "universe", BSG_DEFAULT_UNIVERSE);
  const int exchanges =
      (int)bs_options_get_long(options, "exchanges", BSG_DEFAULT_EXCHANGES);
  const double margin = (double)bs_options_get_long(
      options, "exchmargin", BSG_DEFAULT_EXCHANGE_MARGIN);
  universe->count = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(static_list) &&
                         universe->count < universe_size;
       move_idx++) {
    if (move_get_type(move_list_get_move(static_list, move_idx)) !=
        GAME_EVENT_PASS) {
      bsg_universe_add(universe, move_idx, "static");
    }
  }
  const double best =
      equity_to_double(move_get_equity(move_list_get_move(static_list, 0)));
  int taken = 0;
  for (int move_idx = 0;
       move_idx < move_list_get_count(static_list) && taken < exchanges;
       move_idx++) {
    const Move *move = move_list_get_move(static_list, move_idx);
    if (move_get_type(move) == GAME_EVENT_EXCHANGE &&
        equity_to_double(move_get_equity(move)) >= best - margin) {
      bsg_universe_add(universe, move_idx, "exchange");
      taken++;
    }
  }
  if (pat_list == NULL) {
    return;
  }
  const int pat_nominees =
      (int)bs_options_get_long(options, "patn", BSG_DEFAULT_PAT_NOMINEES);
  int pat_rank = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(pat_list); move_idx++) {
    const Move *move = move_list_get_move(pat_list, move_idx);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    pat_rank++;
    const int static_index = bsg_find(static_list, move);
    assert(static_index >= 0);
    if (pat_rank <= pat_nominees) {
      bsg_universe_add(universe, static_index, "pat");
    }
    for (int idx = 0; idx < universe->count; idx++) {
      if (universe->static_index[idx] == static_index) {
        universe->pat_rank[idx] = pat_rank;
      }
    }
  }
}

static int bsg_parse_rack_counts(const char *text, int *counts) {
  StringSplitter *split = split_string(text, ',', true);
  const int num_counts = string_splitter_get_number_of_items(split);
  assert(num_counts >= 1 && num_counts <= BSG_MAX_RACK_COUNTS);
  for (int count_idx = 0; count_idx < num_counts; count_idx++) {
    counts[count_idx] =
        (int)strtol(string_splitter_get_item(split, count_idx), NULL, 10);
    assert(counts[count_idx] >= BLOCKING_SETUP_MIN_RACKS &&
           counts[count_idx] <= BLOCKING_SETUP_MAX_RACKS);
    assert(count_idx == 0 || counts[count_idx] > counts[count_idx - 1]);
  }
  string_splitter_destroy(split);
  return num_counts;
}

// Splits a positions row into its fields; returns false for the header.
static bool bsg_parse_position(char *line, long *game_idx, int *turn, int *bag,
                               int *lead, char **phase, char **cgp) {
  line[strcspn(line, "\r\n")] = '\0';
  char *fields[5];
  char *cursor = line;
  for (int field_idx = 0; field_idx < 5; field_idx++) {
    fields[field_idx] = cursor;
    cursor = strchr(cursor, ',');
    if (cursor == NULL) {
      return false;
    }
    *cursor++ = '\0';
  }
  char *end = NULL;
  *game_idx = strtol(fields[0], &end, 10);
  if (end == fields[0]) {
    return false;
  }
  *turn = (int)strtol(fields[1], NULL, 10);
  *bag = (int)strtol(fields[2], NULL, 10);
  *lead = (int)strtol(fields[3], NULL, 10);
  *phase = fields[4];
  *cgp = cursor;
  return true;
}

// PAT's charge for the board as it stands: the defense term every
// non-placement move gets, before any candidate changes the board. Built the
// way validated moves build their PAT context.
static double bsg_pat_board_term(const Game *game) {
  const int player_index = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, player_index);
  const PATWeights *pat = player_get_pat(player);
  const Board *board = game_get_board(game);
  if (pat == NULL || bag_get_letters(game_get_bag(game)) == 0 ||
      board_get_transposed(board) || !board_get_cross_sets_valid(board)) {
    return 0.0;
  }
  PATEvalContext pat_eval_ctx;
  pat_eval_context_disable(&pat_eval_ctx);
  pat_eval_context_load(
      &pat_eval_ctx, pat,
      board_get_readonly_lanes(
          board, board_get_cross_set_index(
                     game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG),
                     player_index)),
      game_get_ld(game), player_get_rack(player), PAT_CLASS_MASK_ALL,
      rack_get_total_letters(
          player_get_rack(game_get_player(game, 1 - player_index))));
  pat_eval_context_set_kwg(&pat_eval_ctx, player_get_kwg(player));
  return equity_to_double(pat_eval_non_placement_penalty(&pat_eval_ctx));
}

static void bsg_labels(const BSOptions *options) {
  const char *pat = bs_options_get(options, "pat", NULL);
  Config *config = bsg_config_create(options, pat);
  const long worker = bs_options_get_long(options, "worker", 0);
  const long workers = bs_options_get_long(options, "workers", 1);
  const uint64_t seed = bs_options_get_u64(options, "seed", 0);
  int rack_counts[BSG_MAX_RACK_COUNTS];
  const int num_counts = bsg_parse_rack_counts(
      bs_options_get(options, "racks", "64"), rack_counts);
  const int max_racks = rack_counts[num_counts - 1];
  FILE *in = fopen_or_die(bs_options_require(options, "in"), "r");
  const char *out_path = bs_options_require(options, "out");
  FILE *out = fopen_or_die(out_path, "w");
  // Next to the labels, named so that a "labels*.csv" glob misses it.
  char *timing_path = get_formatted_string("%s.timing", out_path);
  FILE *timing = fopen_or_die(timing_path, "w");
  free(timing_path);
  (void)fprintf(out,
                "game,bag,lead,phase,cand,source,static_rank,pat_rank,type,"
                "tiles_played,score,static_eq,pat_eq,pass_pat_term,move");
  for (int count_idx = 0; count_idx < num_counts; count_idx++) {
    (void)fprintf(out, ",blocking_r%d,setup_r%d", rack_counts[count_idx],
                  rack_counts[count_idx]);
  }
  (void)fprintf(
      out, ",pass_reply,cand_reply,pass_followup,cand_followup,terminal\n");
  (void)fprintf(timing, "game,universe,movegen_ms");
  for (int count_idx = 0; count_idx < num_counts; count_idx++) {
    (void)fprintf(timing, ",teacher_r%d_ms", rack_counts[count_idx]);
  }
  (void)fprintf(timing, "\n");
  MoveList *static_list = move_list_create(BSG_MOVE_LIST_CAPACITY);
  MoveList *pat_list =
      pat != NULL ? move_list_create(BSG_MOVE_LIST_CAPACITY) : NULL;
  BlockingSetupSamples *samples =
      blocking_setup_samples_create(max_racks, BSG_POOL_CAPACITY);
  BlockingSetupChecker *checker = blocking_setup_checker_create();
  BlockingSetupResult *results =
      malloc_or_die(sizeof(BlockingSetupResult) * (size_t)BSG_MAX_UNIVERSE *
                    (size_t)num_counts);
  BSGUniverse *universe = malloc_or_die(sizeof(BSGUniverse));
  char *line = malloc_or_die(BSG_LINE_CAPACITY);
  long input_idx = 0;
  while (fgets(line, BSG_LINE_CAPACITY, in) != NULL) {
    long game_idx = 0;
    int turn = 0;
    int bag = 0;
    int lead = 0;
    char *phase = NULL;
    char *cgp = NULL;
    if (!bsg_parse_position(line, &game_idx, &turn, &bag, &lead, &phase,
                            &cgp)) {
      continue;
    }
    if (input_idx++ % workers != worker) {
      continue;
    }
    char *command = get_formatted_string("cgp %s", cgp);
    load_and_exec_config_or_die(config, command);
    free(command);
    const Game *game = config_get_game(config);
    const double movegen_start = bsg_now_ms();
    bsg_generate(game, static_list, true);
    if (pat_list != NULL) {
      bsg_generate(game, pat_list, false);
      assert(move_list_get_count(pat_list) == move_list_get_count(static_list));
    }
    const double movegen_ms = bsg_now_ms() - movegen_start;
    bsg_build_universe(options, static_list, pat_list, universe);
    const double pass_pat_term =
        pat_list != NULL ? bsg_pat_board_term(game) : 0.0;
    blocking_setup_samples_deal(samples, game, max_racks, true, true,
                                bs_mix(seed ^ bs_mix((uint64_t)game_idx)));
    (void)fprintf(timing, "%ld,%d,%.3f", game_idx, universe->count, movegen_ms);
    for (int count_idx = 0; count_idx < num_counts; count_idx++) {
      const double teacher_start = bsg_now_ms();
      samples->num_racks = rack_counts[count_idx];
      blocking_setup_checker_load(checker, game, samples, 1);
      for (int cand_idx = 0; cand_idx < universe->count; cand_idx++) {
        blocking_setup_checker_measure(
            checker,
            move_list_get_move(static_list, universe->static_index[cand_idx]),
            &results[(count_idx * BSG_MAX_UNIVERSE) + cand_idx]);
      }
      (void)fprintf(timing, ",%.3f", bsg_now_ms() - teacher_start);
    }
    (void)fprintf(timing, "\n");
    (void)fflush(timing);
    for (int cand_idx = 0; cand_idx < universe->count; cand_idx++) {
      const Move *move =
          move_list_get_move(static_list, universe->static_index[cand_idx]);
      double pat_eq = equity_to_double(move_get_equity(move));
      if (pat_list != NULL) {
        pat_eq = equity_to_double(move_get_equity(
            move_list_get_move(pat_list, bsg_find(pat_list, move))));
      }
      StringBuilder *move_text = string_builder_create();
      string_builder_add_ucgi_move(move_text, move, game_get_board(game),
                                   game_get_ld(game));
      (void)fprintf(
          out, "%ld,%d,%d,%s,%d,%s,%d,%d,%s,%d,%d,%.3f,%.3f,%.3f,%s", game_idx,
          bag, lead, phase, cand_idx, universe->source[cand_idx],
          universe->static_index[cand_idx] + 1, universe->pat_rank[cand_idx],
          move_get_type(move) == GAME_EVENT_EXCHANGE ? "exchange" : "place",
          move_get_tiles_played(move), equity_to_int(move_get_score(move)),
          equity_to_double(move_get_equity(move)), pat_eq, pass_pat_term,
          string_builder_peek(move_text));
      string_builder_destroy(move_text);
      for (int count_idx = 0; count_idx < num_counts; count_idx++) {
        const BlockingSetupResult *result =
            &results[(count_idx * BSG_MAX_UNIVERSE) + cand_idx];
        (void)fprintf(out, ",%.6f,%.6f", result->blocking_delta,
                      result->setup_delta);
      }
      const BlockingSetupResult *largest =
          &results[((num_counts - 1) * BSG_MAX_UNIVERSE) + cand_idx];
      (void)fprintf(out, ",%.6f,%.6f,%.6f,%.6f,%d\n", largest->pass_reply_mean,
                    largest->candidate_reply_mean, largest->pass_followup_mean,
                    largest->candidate_followup_mean,
                    largest->terminal_replies);
    }
    (void)fflush(out);
  }
  free(line);
  free(universe);
  free(results);
  blocking_setup_checker_destroy(checker);
  blocking_setup_samples_destroy(samples);
  if (pat_list != NULL) {
    move_list_destroy(pat_list);
  }
  move_list_destroy(static_list);
  (void)fclose(timing);
  (void)fclose(out);
  (void)fclose(in);
  config_destroy(config);
}

// The universe candidates of one game from a labels file: the move text of
// each row, in candidate order.
static int bsg_read_universe(const char *labels_path, long game_idx,
                             char **moves) {
  FILE *labels = fopen_or_die(labels_path, "r");
  char *line = malloc_or_die(BSG_LINE_CAPACITY);
  int count = 0;
  while (fgets(line, BSG_LINE_CAPACITY, labels) != NULL) {
    char *end = NULL;
    if (strtol(line, &end, 10) != game_idx || end == line || *end != ',') {
      continue;
    }
    // The move text is the 15th field.
    char *field = line;
    for (int field_idx = 0; field_idx < 14 && field != NULL; field_idx++) {
      field = strchr(field, ',');
      if (field != NULL) {
        field++;
      }
    }
    assert(field != NULL && count < BSG_MAX_UNIVERSE);
    field[strcspn(field, ",\r\n")] = '\0';
    moves[count++] = string_duplicate(field);
  }
  free(line);
  (void)fclose(labels);
  return count;
}

// The (game, cand) pairs of a cands= file.
typedef struct BSGCandFilter {
  int count;
  int capacity;
  long *games;
  int *cands;
} BSGCandFilter;

static void bsg_cand_filter_load(BSGCandFilter *filter, const char *path) {
  filter->count = 0;
  filter->capacity = 0;
  filter->games = NULL;
  filter->cands = NULL;
  if (path == NULL) {
    return;
  }
  FILE *file = fopen_or_die(path, "r");
  char *line = malloc_or_die(BSG_LINE_CAPACITY);
  while (fgets(line, BSG_LINE_CAPACITY, file) != NULL) {
    char *end = NULL;
    const long game_idx = strtol(line, &end, 10);
    if (end == line || *end != ',') {
      continue;
    }
    if (filter->count == filter->capacity) {
      filter->capacity = filter->capacity == 0 ? 1024 : 2 * filter->capacity;
      filter->games = realloc_or_die(filter->games,
                                     sizeof(long) * (size_t)filter->capacity);
      filter->cands =
          realloc_or_die(filter->cands, sizeof(int) * (size_t)filter->capacity);
    }
    filter->games[filter->count] = game_idx;
    filter->cands[filter->count] = (int)strtol(end + 1, NULL, 10);
    filter->count++;
  }
  free(line);
  (void)fclose(file);
}

// Whether the filter keeps candidate cand of game_idx (no filter keeps all).
static bool bsg_cand_filter_keeps(const BSGCandFilter *filter, long game_idx,
                                  int cand) {
  if (filter->games == NULL) {
    return true;
  }
  for (int idx = 0; idx < filter->count; idx++) {
    if (filter->games[idx] == game_idx && filter->cands[idx] == cand) {
      return true;
    }
  }
  return false;
}

static void bsg_refs(const BSOptions *options) {
  Config *config = bsg_config_create(options, NULL);
  const long worker = bs_options_get_long(options, "worker", 0);
  const long workers = bs_options_get_long(options, "workers", 1);
  const uint64_t seed = bs_options_get_u64(options, "seed", 0);
  const int plies =
      (int)bs_options_get_long(options, "plies", BSG_DEFAULT_PLIES);
  const double seconds =
      (double)bs_options_get_long(options, "ms", BSG_DEFAULT_REF_MS) / 1000.0;
  const char *labels_path = bs_options_require(options, "labels");
  BSGCandFilter filter;
  bsg_cand_filter_load(&filter, bs_options_get(options, "cands", NULL));
  ErrorStack *errors = error_stack_create();
  config_load_win_pcts(config, errors);
  assert(error_stack_is_empty(errors));
  FILE *in = fopen_or_die(bs_options_require(options, "in"), "r");
  FILE *out = fopen_or_die(bs_options_require(options, "out"), "w");
  (void)fprintf(out, "game,cand,move,sim_wp,sim_wp_sem,sim_eq,sim_eq_sem,"
                     "iterations,wall_ms\n");
  MoveList *static_list = move_list_create(BSG_MOVE_LIST_CAPACITY);
  SimResults *results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  char *line = malloc_or_die(BSG_LINE_CAPACITY);
  long input_idx = 0;
  while (fgets(line, BSG_LINE_CAPACITY, in) != NULL) {
    long game_idx = 0;
    int turn = 0;
    int bag = 0;
    int lead = 0;
    char *phase = NULL;
    char *cgp = NULL;
    if (!bsg_parse_position(line, &game_idx, &turn, &bag, &lead, &phase,
                            &cgp)) {
      continue;
    }
    if (input_idx++ % workers != worker) {
      continue;
    }
    char *command = get_formatted_string("cgp %s", cgp);
    load_and_exec_config_or_die(config, command);
    free(command);
    const Game *game = config_get_game(config);
    bsg_generate(game, static_list, true);
    char *moves[BSG_MAX_UNIVERSE];
    const int count = bsg_read_universe(labels_path, game_idx, moves);
    assert(count > 0);
    int kept = 0;
    for (int cand_idx = 0; cand_idx < count; cand_idx++) {
      if (bsg_cand_filter_keeps(&filter, game_idx, cand_idx)) {
        kept++;
      } else {
        // Dropped candidates match no generated move below.
        moves[cand_idx][0] = '\0';
      }
    }
    if (kept < 2) {
      for (int cand_idx = 0; cand_idx < count; cand_idx++) {
        free(moves[cand_idx]);
      }
      continue;
    }
    MoveList *candidates = move_list_create(kept);
    for (int move_idx = 0; move_idx < move_list_get_count(static_list);
         move_idx++) {
      const Move *move = move_list_get_move(static_list, move_idx);
      StringBuilder *move_text = string_builder_create();
      string_builder_add_ucgi_move(move_text, move, game_get_board(game),
                                   game_get_ld(game));
      for (int cand_idx = 0; cand_idx < count; cand_idx++) {
        if (strings_equal(moves[cand_idx], string_builder_peek(move_text))) {
          move_list_add_move(candidates, move);
        }
      }
      string_builder_destroy(move_text);
    }
    assert(move_list_get_count(candidates) == kept);
    ThreadControl *control = thread_control_create();
    thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
    SimArgs args;
    sim_args_fill(plies, candidates, kept, NULL, config_get_win_pcts(config),
                  NULL, control, game, false, false, 1, 0, kept, plies,
                  bs_mix(seed ^ bs_mix((uint64_t)game_idx)),
                  UINT64_C(1000000000000000), BSG_DEFAULT_MIN_PLAY_ITERATIONS,
                  0.0, BAI_THRESHOLD_NONE, seconds,
                  BAI_SAMPLING_RULE_ROUND_ROBIN, -1.0, 1.0, 0.0, 100.0, false,
                  NULL, &args);
    args.pat_rollout_disabled = true;
    const double start = bsg_now_ms();
    simulate(&args, &sim_ctx, results, errors);
    const double wall_ms = bsg_now_ms() - start;
    assert(error_stack_is_empty(errors));
    for (int play_idx = 0; play_idx < sim_results_get_number_of_plays(results);
         play_idx++) {
      const SimmedPlay *play = sim_results_get_simmed_play(results, play_idx);
      const Move *move = simmed_play_get_move(play);
      StringBuilder *move_text = string_builder_create();
      string_builder_add_ucgi_move(move_text, move, game_get_board(game),
                                   game_get_ld(game));
      int cand = -1;
      for (int cand_idx = 0; cand_idx < count; cand_idx++) {
        if (strings_equal(moves[cand_idx], string_builder_peek(move_text))) {
          cand = cand_idx;
        }
      }
      assert(cand >= 0);
      const Stat *wp = simmed_play_get_win_pct_stat(play);
      const Stat *eq = simmed_play_get_equity_stat(play);
      (void)fprintf(
          out, "%ld,%d,%s,%.9f,%.9f,%.6f,%.6f,%llu,%.1f\n", game_idx, cand,
          string_builder_peek(move_text), stat_get_mean(wp), stat_get_sem(wp),
          stat_get_mean(eq), stat_get_sem(eq),
          (unsigned long long)sim_results_get_iteration_count(results),
          wall_ms);
      string_builder_destroy(move_text);
    }
    (void)fflush(out);
    thread_control_destroy(control);
    move_list_destroy(candidates);
    for (int cand_idx = 0; cand_idx < count; cand_idx++) {
      free(moves[cand_idx]);
    }
  }
  free(line);
  free(filter.games);
  free(filter.cands);
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(results);
  move_list_destroy(static_list);
  error_stack_destroy(errors);
  (void)fclose(out);
  (void)fclose(in);
  config_destroy(config);
}

void blocking_setup_gen_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  assert(string_splitter_get_number_of_items(fields) >= 1);
  BSOptions options;
  bs_options_parse(&options, fields);
  const char *stage = string_splitter_get_item(fields, 0);
  if (strings_equal(stage, "positions")) {
    bsg_positions(&options);
  } else if (strings_equal(stage, "labels")) {
    bsg_labels(&options);
  } else if (strings_equal(stage, "refs")) {
    bsg_refs(&options);
  } else {
    log_fatal("bsgen: unknown stage '%s'", stage);
  }
  bs_options_destroy(&options);
  string_splitter_destroy(fields);
}
