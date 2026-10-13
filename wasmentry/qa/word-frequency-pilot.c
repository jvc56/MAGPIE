// Offline CSW24 self-play corpus. No changes to the engine or browser build.
#include "../../src/def/config_defs.h"
#include "../../src/def/letter_distribution_defs.h"
#include "../../src/def/thread_control_defs.h"
#include "../../src/ent/board.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/thread_control.h"
#include "../../src/impl/cgp.h"
#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"
#include "../../src/impl/play_chooser.h"
#include "../../src/util/io_util.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TABLE_SIZE = 1 << 20 };
typedef struct {
  char word[16];
  uint64_t count;
} WordCount;
static WordCount words[TABLE_SIZE];

static void check(ErrorStack *errors) {
  if (!error_stack_is_empty(errors)) {
    error_stack_print_and_reset(errors);
    exit(EXIT_FAILURE);
  }
}
static void count_word(const char *word) {
  uint32_t hash = 2166136261U;
  for (const char *letter = word; *letter; letter++) {
    hash = (hash ^ (unsigned char)*letter) * 16777619U;
  }
  unsigned int slot = hash & (TABLE_SIZE - 1);
  while (words[slot].word[0] && strcmp(words[slot].word, word)) {
    slot = (slot + 1) & (TABLE_SIZE - 1);
  }
  strcpy(words[slot].word, word);
  words[slot].count++;
}
static void count_formed(const Board *board, const unsigned char before[225]) {
  for (int direction = 0; direction < 2; direction++) {
    for (int line = 0; line < 15; line++) {
      char word[16];
      int length = 0;
      int changed = 0;
      for (int offset = 0; offset <= 15; offset++) {
        const int row = direction ? offset : line;
        const int col = direction ? line : offset;
        const MachineLetter letter =
            offset < 15 ? board_get_letter(board, row, col) : 0;
        if (letter) {
          word[length++] =
              (char)('A' + get_unblanked_machine_letter(letter) - 1);
          changed |= !before[row * 15 + col];
        } else {
          if (length > 1 && changed) {
            word[length] = '\0';
            count_word(word);
          }
          length = 0;
          changed = 0;
        }
      }
    }
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
int main(int argc, char **argv) {
  if (argc != 5 && argc != 6) {
    fprintf(stderr,
            "usage: %s OFFSET GAMES COUNTS.tsv SAMPLES.cgp [CSW24|NWL23]\n",
            argv[0]);
    return 1;
  }
  const char *lexicon = argc == 6 ? argv[5] : "CSW24";
  if (strcmp(lexicon, "CSW24") && strcmp(lexicon, "NWL23")) {
    return 1;
  }
  const int offset = parse_count(argv[1]);
  const int games = parse_count(argv[2]);
  if (offset < 0 || games < 1 || offset > INT_MAX - games) {
    return 1;
  }
  FILE *samples = fopen(argv[4], "w");
  if (!samples) {
    return 1;
  }
  ErrorStack *errors = error_stack_create();
  const ConfigArgs args = {.data_paths = "data",
                           .settings_filename = DEFAULT_SETTINGS_FILENAME,
                           .use_wmp = true};
  Config *config = config_create(&args, errors);
  check(errors);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  char command[256];
  snprintf(command, sizeof(command),
           "set -lex %s -wmp true -wit false -rit false -s1 equity -s2 equity "
           "-r1 best -r2 best -numplays 1 -threads 1 -savesettings false "
           "-autosavegcg false",
           lexicon);
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
  for (int index = 0; index < games; index++) {
    game_reset(game);
    game_seed(game, 0x5eedU + (uint64_t)(index + offset) * 0x9e3779b9U);
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
      count_formed(game_get_board(game), before);
      turn++;
      if (index % 100 == 0 &&
          (turn == 8 || turn == 16 || turn == 24 || game_over(game))) {
        char cgp[1024];
        game_get_cgp_string(game, true, cgp, sizeof(cgp));
        fprintf(samples, "%s -lex %s\n", cgp, lexicon);
      }
    }
    if ((index + 1) % 1000 == 0) {
      fprintf(stderr, "%d/%d games\n", index + 1, games);
    }
  }
  if (fclose(samples)) {
    return 1;
  }
  FILE *counts = fopen(argv[3], "w");
  if (!counts) {
    return 1;
  }
  for (int index = 0; index < TABLE_SIZE; index++) {
    if (words[index].count) {
      fprintf(counts, "%s\t%llu\n", words[index].word,
              (unsigned long long)words[index].count);
    }
  }
  if (fclose(counts)) {
    return 1;
  }
  game_destroy(game);
  play_chooser_destroy(chooser);
  config_destroy(config);
  error_stack_destroy(errors);
  return 0;
}
