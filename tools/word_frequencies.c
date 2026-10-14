// Deterministic static-selfplay word frequencies used by the XS codebooks.
#include "../src/def/board_defs.h"
#include "../src/def/config_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/word_counts.h"
#include "../src/util/io_util.h"
#include "../src/util/xs_bits.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(ErrorStack *errors) {
  if (!error_stack_is_empty(errors)) {
    error_stack_print_and_reset(errors);
    exit(EXIT_FAILURE);
  }
}

static int parse_count(const char *text) {
  char *end;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (errno || end == text || *end || value < 0 || value > INT_MAX) {
    return -1;
  }
  return (int)value;
}

static int merge_counts(int argc, char **argv) {
  if (argc < 4) {
    return EXIT_FAILURE;
  }
  ErrorStack *errors = error_stack_create();
  WordCounts *counts = word_counts_create();
  bool valid = true;
  for (int index = 3; valid && index < argc; index++) {
    valid = strcmp(argv[2], argv[index]) != 0 &&
            word_counts_merge(counts, argv[index], errors);
  }
  if (valid) {
    valid = word_counts_write(counts, argv[2], errors);
  }
  error_stack_print_and_reset(errors);
  error_stack_destroy(errors);
  word_counts_destroy(counts);
  return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "merge")) {
    return merge_counts(argc, argv);
  }
  // NOLINTNEXTLINE(misc-redundant-expression)
  if (BOARD_DIM != 15 || RACK_SIZE != 7) {
    (void)fputs(
        "Word-frequency training requires BOARD_DIM=15 and RACK_SIZE=7.\n",
        stderr);
    return EXIT_FAILURE;
  }
  if (argc < 5 || argc > 7) {
    (void)fprintf(stderr,
                  "usage: %s OFFSET GAMES COUNTS.tsv SAMPLES.cgp [CSW24|NWL23 "
                  "[DATA_PATHS]]\n"
                  "       %s merge COUNTS.tsv SHARD.tsv [SHARD.tsv ...]\n",
                  argv[0], argv[0]);
    return 1;
  }
  const char *lexicon = argc >= 6 ? argv[5] : "CSW24";
  if (strcmp(lexicon, "CSW24") != 0 && strcmp(lexicon, "NWL23") != 0) {
    return 1;
  }
  const int offset = parse_count(argv[1]);
  const int games = parse_count(argv[2]);
  if (offset < 0 || games < 1 || offset > INT_MAX - games ||
      !strcmp(argv[3], argv[4])) {
    return 1;
  }
  ErrorStack *errors = error_stack_create();
  FILE *samples = fopen_safe(argv[4], "wb", errors);
  check(errors);
  const char *paths = argc == 7 ? argv[6] : "data";
  const ConfigArgs args = {.data_paths = paths,
                           .settings_filename = DEFAULT_SETTINGS_FILENAME,
                           .use_wmp = true};
  Config *config = config_create(&args, errors);
  check(errors);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  char command[256];
  const int command_length = snprintf(
      command, sizeof(command),
      "set -lex %s -wmp true -wit false -rit false -s1 equity -s2 equity "
      "-r1 best -r2 best -numplays 1 -threads 1 -savesettings false "
      "-autosavegcg false",
      lexicon);
  if (command_length < 0 || (size_t)command_length >= sizeof(command)) {
    xs_error(errors, "cannot format word-frequency training command");
    check(errors);
  }
  config_load_command(config, command, errors);
  check(errors);
  config_execute_command(config, errors);
  check(errors);
  const PlayChooserStrategy strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .num_threads = 1,
      .seed = 0x5eed};
  PlayChooser *chooser = play_chooser_create(&strategy);
  Game *game = config_game_create(config);
  WordCounts *counts = word_counts_create();
  for (int index = 0; index < games; index++) {
    game_reset(game);
    game_seed(game, 0x5eedU + ((uint64_t)(index + offset) * 0x9e3779b9U));
    game_set_starting_player_index(game, (index + offset) % 2);
    draw_starting_racks(game);
    int turn = 0;
    while (!game_over(game)) {
      unsigned char before[225];
      for (int cell = 0; cell < 225; cell++) {
        before[cell] =
            board_get_letter(game_get_board(game), cell / 15, cell % 15);
      }
      Move move;
      play_chooser_choose_move(chooser, game, &move, errors);
      check(errors);
      play_move(&move, game, NULL);
      if (!word_counts_record(counts, game_get_board(game), before)) {
        xs_error(errors, "word frequency overflow");
        check(errors);
      }
      turn++;
      if (index % 100 == 0 &&
          (turn == 8 || turn == 16 || turn == 24 || game_over(game))) {
        char cgp[1024];
        game_get_cgp_string(game, true, cgp, sizeof(cgp));
        if (fprintf(samples, "%s -lex %s\n", cgp, lexicon) < 0) {
          xs_error(errors, "cannot write sampled position");
          check(errors);
        }
      }
    }
    if ((index + 1) % 1000 == 0) {
      (void)fprintf(stderr, "%d/%d games\n", index + 1, games);
    }
  }
  fclose_or_die(samples);
  word_counts_write(counts, argv[3], errors);
  check(errors);
  word_counts_destroy(counts);
  game_destroy(game);
  play_chooser_destroy(chooser);
  config_destroy(config);
  error_stack_destroy(errors);
  return 0;
}
