#include "value_net_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/endian_io.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/value_net.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/value_net_features.h"
#include "../src/impl/value_net_metal.h"
#include "../src/impl/value_net_player.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  VALUE_NET_PARITY_ROWS = 64,
  VALUE_NET_PARITY_REPEATS = 20,
  VNT_MAX_TURNS = 256,
};

static uint64_t vnt_mix(uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

static value_net_backend_t vnt_parse_backend(const char *name) {
  if (strings_equal(name, "cpu")) {
    return VALUE_NET_BACKEND_CPU;
  }
  if (strings_equal(name, "fp16")) {
    return VALUE_NET_BACKEND_METAL_FP16;
  }
  if (!strings_equal(name, "fp32")) {
    log_fatal("backend must be cpu, fp32 or fp16, got %s", name);
  }
  return VALUE_NET_BACKEND_METAL_FP32;
}

// The top static-equity move for the player on turn.
static const Move *vnt_static_move(const Game *game, MoveList *list) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  return move_list_get_move(list, 0);
}

// "games:<model_dir>:<backend>:<lexicon>:<pairs>:<seed>:<worker>:<workers>:
// <out>": game pairs between the value net player (a) and the static player
// (b). Both games of a pair use one seed and the players swap seats, so each
// seat draws the same tiles. Writes <out>.games.csv and per-decision timing
// to <out>.moves.csv.
static void vnt_games(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 9) {
    log_fatal("games needs 9 fields");
  }
  const char *model_dir = string_splitter_get_item(fields, 1);
  const value_net_backend_t backend =
      vnt_parse_backend(string_splitter_get_item(fields, 2));
  const char *lexicon = string_splitter_get_item(fields, 3);
  const long pairs = strtol(string_splitter_get_item(fields, 4), NULL, 10);
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 5), NULL, 10);
  const long worker = strtol(string_splitter_get_item(fields, 6), NULL, 10);
  const long workers = strtol(string_splitter_get_item(fields, 7), NULL, 10);
  const char *out = string_splitter_get_item(fields, 8);
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1",
      lexicon);
  Config *config = config_create_or_die(settings);
  free(settings);
  ErrorStack *error_stack = error_stack_create();
  ValueNetPlayer *player =
      value_net_player_create(model_dir, backend, 0, error_stack);
  if (player == NULL) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not create the value net player");
  }
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.games.csv", out);
  FILE *games_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.moves.csv", out);
  FILE *moves_out = fopen_or_die(path, "w");
  free(path);
  (void)fprintf(games_out,
                "pair,game,a_seat,a_score,b_score,a_spread,a_win,turns\n");
  (void)fprintf(moves_out, "pair,game,turn,player,bag,total_ms\n");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  ValueNetHistory histories[2];
  for (long pair_idx = 0; pair_idx < pairs; pair_idx++) {
    if (pair_idx % workers != worker) {
      continue;
    }
    const uint64_t pair_seed = vnt_mix(seed ^ vnt_mix((uint64_t)pair_idx));
    for (int game_in_pair = 0; game_in_pair < 2; game_in_pair++) {
      const int a_seat = game_in_pair;
      game_reset(game);
      game_seed(game, pair_seed);
      game_set_starting_player_index(game, (int)(pair_idx % 2));
      draw_starting_racks(game);
      value_net_history_reset(&histories[0]);
      value_net_history_reset(&histories[1]);
      int turn = 0;
      while (!game_over(game) && turn < VNT_MAX_TURNS) {
        const int seat = game_get_player_on_turn_index(game);
        const int64_t start = ctimer_monotonic_ns();
        const Move *chosen =
            seat == a_seat
                ? value_net_player_choose(player, game, &histories[seat])
                : vnt_static_move(game, static_list);
        Move move;
        move_copy(&move, chosen);
        (void)fprintf(moves_out, "%ld,%d,%d,%c,%d,%.3f\n", pair_idx,
                      game_in_pair, turn, seat == a_seat ? 'a' : 'b',
                      bag_get_letters(game_get_bag(game)),
                      (double)(ctimer_monotonic_ns() - start) / 1e6);
        value_net_history_record_opponent_move(&histories[1 - seat], &move);
        play_move(&move, game, NULL);
        turn++;
      }
      const int a_score =
          equity_to_int(player_get_score(game_get_player(game, a_seat)));
      const int b_score =
          equity_to_int(player_get_score(game_get_player(game, 1 - a_seat)));
      const int spread = a_score - b_score;
      const double result = spread > 0 ? 1.0 : (spread == 0 ? 0.5 : 0.0);
      (void)fprintf(games_out, "%ld,%d,%d,%d,%d,%d,%.1f,%d\n", pair_idx,
                    game_in_pair, a_seat, a_score, b_score, spread, result,
                    turn);
      (void)fflush(games_out);
    }
    (void)fflush(moves_out);
  }
  (void)fclose(games_out);
  (void)fclose(moves_out);
  move_list_destroy(static_list);
  value_net_player_destroy(player);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Reads count little-endian floats from path.
static float *vnt_read_floats(const char *path, size_t count) {
  FILE *stream = fopen_or_die(path, "rb");
  uint32_t *bits = malloc_or_die(sizeof(uint32_t) * count);
  if (!fread_le_uint32s(bits, count, stream)) {
    log_fatal("could not read %zu floats from %s", count, path);
  }
  (void)fclose(stream);
  float *values = malloc_or_die(sizeof(float) * count);
  memcpy(values, bits, sizeof(float) * count);
  free(bits);
  return values;
}

// Compares a backend's outputs with the parity set's reference outputs and
// prints the largest differences.
static void vnt_report(const char *backend, int rows, const float *value,
                       const float *spread, const float *ref_value,
                       const float *ref_spread, double seconds) {
  double max_value = 0.0;
  double max_spread = 0.0;
  for (int row = 0; row < rows; row++) {
    max_value = fmax(max_value, fabs((double)value[row] - ref_value[row]));
    max_spread = fmax(max_spread, fabs((double)spread[row] - ref_spread[row]));
  }
  printf("value_net_parity backend=%s rows=%d max_abs_value_diff=%.3g "
         "max_abs_spread_diff=%.3g seconds=%.3f\n",
         backend, rows, max_value, max_spread, seconds);
}

// "parity:<dir>:<parity_dir>[:<rows>]": the CPU forward pass on the
// parity set's input rows against its reference outputs.
static void vnt_parity(const char *dir, const char *parity_dir, int rows) {
  ErrorStack *error_stack = error_stack_create();
  ValueNet *net = value_net_create(dir, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not load the value net from %s", dir);
  }
  char *path = get_formatted_string("%s/board.f32", parity_dir);
  float *board = vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS *
                                           VALUE_NET_BOARD_FLOATS);
  free(path);
  path = get_formatted_string("%s/scalars.f32", parity_dir);
  float *scalars =
      vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS * VALUE_NET_SCALARS);
  free(path);
  path = get_formatted_string("%s/value.f32", parity_dir);
  float *ref_value = vnt_read_floats(path, VALUE_NET_PARITY_ROWS);
  free(path);
  path = get_formatted_string("%s/spread.f32", parity_dir);
  float *ref_spread = vnt_read_floats(path, VALUE_NET_PARITY_ROWS);
  free(path);
  float value[VALUE_NET_PARITY_ROWS];
  float spread[VALUE_NET_PARITY_ROWS];
  int64_t start = ctimer_monotonic_ns();
  value_net_evaluate_cpu(net, rows, board, scalars, value, spread);
  vnt_report("cpu", rows, value, spread, ref_value, ref_spread,
             (double)(ctimer_monotonic_ns() - start) / 1e9);
  for (int half = 0; half <= 1; half++) {
    ValueNetMetal *metal = value_net_metal_create(net, half, error_stack);
    if (metal == NULL) {
      error_stack_print_and_reset(error_stack);
      continue;
    }
    // The first run compiles the graph for this batch size.
    start = ctimer_monotonic_ns();
    value_net_metal_evaluate(metal, rows, board, scalars, value, spread);
    const double first = (double)(ctimer_monotonic_ns() - start) / 1e9;
    start = ctimer_monotonic_ns();
    for (int repeat = 0; repeat < VALUE_NET_PARITY_REPEATS; repeat++) {
      value_net_metal_evaluate(metal, rows, board, scalars, value, spread);
    }
    const double seconds = (double)(ctimer_monotonic_ns() - start) / 1e9 /
                           VALUE_NET_PARITY_REPEATS;
    vnt_report(half ? "metal_fp16" : "metal_fp32", rows, value, spread,
               ref_value, ref_spread, seconds);
    printf("value_net_metal first_call_seconds=%.3f\n", first);
    value_net_metal_destroy(metal);
  }
  free(board);
  free(scalars);
  free(ref_value);
  free(ref_spread);
  value_net_destroy(net);
  error_stack_destroy(error_stack);
}

// "xsprobe": prints MAGPIE's cross sets in both directions around CAT
// played across at 8H, to pin down which direction holds which constraint.
static void vnt_cross_set_probe(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/7CAT5/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int squares[3][2] = {{7, 10}, {6, 7}, {7, 8}};
  for (int idx = 0; idx < 3; idx++) {
    for (int dir = 0; dir < 2; dir++) {
      const uint64_t cross_set =
          board_get_cross_set(board, squares[idx][0], squares[idx][1], dir, 0);
      printf("square (%d,%d) dir %d:", squares[idx][0], squares[idx][1], dir);
      for (int ml = 1; ml <= 26; ml++) {
        if ((cross_set >> ml) & 1) {
          char *letter = ld_ml_to_hl(ld, (MachineLetter)ml);
          printf("%s", letter);
          free(letter);
        }
      }
      printf("\n");
    }
  }
  config_destroy(config);
}

// "dump:<lexicon>:<positions>:<seed>:<out>": plays static games and, every
// third turn while tiles remain, writes the position's CGP (player on turn
// first, then a tab, the opponent's last move and its score) to <out>.cgps
// and, for its top 50 static moves with that one move as the history, the move
// (<out>.tsv: position, move, equity, score) and its input row (<out>.bin:
// board floats then scalars), for comparison with Macondo's encoder on the same
// positions.
static void vnt_dump(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("dump needs 4 fields");
  }
  const char *lexicon = string_splitter_get_item(fields, 1);
  const long positions = strtol(string_splitter_get_item(fields, 2), NULL, 10);
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *out = string_splitter_get_item(fields, 4);
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1",
      lexicon);
  Config *config = config_create_or_die(settings);
  free(settings);
  char *path = get_formatted_string("%s.cgps", out);
  FILE *cgps_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.tsv", out);
  FILE *tsv_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.bin", out);
  FILE *bin_out = fopen_or_die(path, "wb");
  free(path);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scratch = game_duplicate(game);
  MoveList *list = move_list_create(50);
  MoveList *static_list = move_list_create(1);
  float *board_row = malloc_or_die(sizeof(float) * VALUE_NET_BOARD_FLOATS);
  float scalars_row[VALUE_NET_SCALARS];
  // A one-move history: the opponent's last move, and 0 or 1 moves since
  // their last bingo (Macondo's count over that one move).
  ValueNetHistory history;
  Move last;
  bool has_last = false;
  long written = 0;
  for (uint64_t game_idx = 0; written < positions; game_idx++) {
    game_reset(game);
    game_seed(game, vnt_mix(seed ^ game_idx));
    draw_starting_racks(game);
    has_last = false;
    for (int turn = 0; !game_over(game) && written < positions; turn++) {
      if (turn % 3 == 1 && bag_get_letters(game_get_bag(game)) > 0) {
        char *cgp = game_get_cgp(game, true);
        value_net_history_reset(&history);
        if (has_last) {
          value_net_history_record_opponent_move(&history, &last);
          StringBuilder *last_sb = string_builder_create();
          string_builder_add_move_description(last_sb, &last,
                                              game_get_ld(game));
          (void)fprintf(cgps_out, "%s\t%s\t%d\n", cgp,
                        string_builder_peek(last_sb),
                        equity_to_int(move_get_score(&last)));
          string_builder_destroy(last_sb);
        } else {
          (void)fprintf(cgps_out, "%s\n", cgp);
        }
        free(cgp);
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
        };
        generate_moves(&args);
        move_list_sort_moves(list);
        for (int move_idx = 0; move_idx < move_list_get_count(list);
             move_idx++) {
          const Move *move = move_list_get_move(list, move_idx);
          StringBuilder *sb = string_builder_create();
          string_builder_add_move_description(sb, move, game_get_ld(game));
          (void)fprintf(tsv_out, "%ld\t%s\t%.4f\t%d\n", written,
                        string_builder_peek(sb),
                        equity_to_double(move_get_equity(move)),
                        equity_to_int(move_get_score(move)));
          string_builder_destroy(sb);
          value_net_features_for_move(game, move, &history, scratch, board_row,
                                      scalars_row);
          (void)fwrite(board_row, sizeof(float), VALUE_NET_BOARD_FLOATS,
                       bin_out);
          (void)fwrite(scalars_row, sizeof(float), VALUE_NET_SCALARS, bin_out);
        }
        written++;
      }
      move_copy(&last, vnt_static_move(game, static_list));
      has_last = true;
      play_move(&last, game, NULL);
    }
  }
  free(board_row);
  move_list_destroy(list);
  move_list_destroy(static_list);
  game_destroy(scratch);
  (void)fclose(cgps_out);
  (void)fclose(tsv_out);
  (void)fclose(bin_out);
  config_destroy(config);
}

void value_net_test_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  const int num_fields = string_splitter_get_number_of_items(fields);
  const char *mode = string_splitter_get_item(fields, 0);
  if (strings_equal(mode, "dump")) {
    vnt_dump(fields);
  } else if (strings_equal(mode, "games")) {
    vnt_games(fields);
  } else if (strings_equal(mode, "xsprobe")) {
    vnt_cross_set_probe();
  } else if (strings_equal(mode, "parity") && num_fields >= 3) {
    int rows = VALUE_NET_PARITY_ROWS;
    if (num_fields >= 4) {
      rows = (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
    }
    if (rows < 1 || rows > VALUE_NET_PARITY_ROWS) {
      log_fatal("parity rows must be 1..%d", VALUE_NET_PARITY_ROWS);
    }
    vnt_parity(string_splitter_get_item(fields, 1),
               string_splitter_get_item(fields, 2), rows);
  } else {
    log_fatal("unknown valuenet spec: %s", spec);
  }
  string_splitter_destroy(fields);
}
