#include "threats_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/move_gen_threats.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Enough for every play any of these tests can find.
  THREATS_TEST_MOVE_LIST_CAPACITY = 400000,
};

// A play by where it goes and what it spells, without which tiles are blanks:
// the same placement made with blanks in other places is the same threat.
typedef struct Placement {
  uint8_t row;
  uint8_t col;
  uint8_t dir;
  uint8_t length;
  MachineLetter tiles[BOARD_DIM];
  Equity score;
} Placement;

typedef struct Placements {
  Placement *placements;
  int count;
  int capacity;
} Placements;

static void placements_add_move(Placements *placements, const Move *move) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  if (placements->count == placements->capacity) {
    placements->capacity = placements->capacity * 2 + 1024;
    placements->placements =
        realloc_or_die(placements->placements,
                       sizeof(Placement) * (size_t)placements->capacity);
  }
  Placement *placement = &placements->placements[placements->count++];
  memset(placement, 0, sizeof(Placement));
  placement->row = (uint8_t)move_get_row_start(move);
  placement->col = (uint8_t)move_get_col_start(move);
  placement->dir = (uint8_t)move_get_dir(move);
  placement->length = (uint8_t)move_get_tiles_length(move);
  for (int tile_idx = 0; tile_idx < placement->length; tile_idx++) {
    placement->tiles[tile_idx] =
        get_unblanked_machine_letter(move_get_tile(move, tile_idx));
  }
  placement->score = move_get_score(move);
}

static int compare_placement_keys(const Placement *a, const Placement *b) {
  const size_t key_size = offsetof(Placement, score);
  return memcmp(a, b, key_size);
}

// By placement, then from the highest score.
static int compare_placements(const void *a, const void *b) {
  const Placement *placement_a = a;
  const Placement *placement_b = b;
  const int key_order = compare_placement_keys(placement_a, placement_b);
  if (key_order != 0) {
    return key_order;
  }
  if (placement_a->score != placement_b->score) {
    return placement_a->score > placement_b->score ? -1 : 1;
  }
  return 0;
}

// Sorts the placements and keeps each one once, at its highest score.
static void placements_keep_best(Placements *placements) {
  if (placements->count == 0) {
    return;
  }
  qsort(placements->placements, (size_t)placements->count, sizeof(Placement),
        compare_placements);
  int kept = 0;
  for (int placement_idx = 0; placement_idx < placements->count;
       placement_idx++) {
    if (kept > 0 &&
        compare_placement_keys(&placements->placements[kept - 1],
                               &placements->placements[placement_idx]) == 0) {
      continue;
    }
    placements->placements[kept++] = placements->placements[placement_idx];
  }
  placements->count = kept;
}

// Adds every play generate_moves finds for the opponent of the player on turn
// in `game` with each rack of rack_size tiles the pool can make, choosing
// how many of each letter from `ml` up.
static void add_plays_of_every_rack(Game *opponent_game, const Rack *pool,
                                    Rack *rack, MachineLetter ml,
                                    int tiles_left, MoveList *move_list,
                                    Placements *placements) {
  if (ml == rack_get_dist_size(pool)) {
    if (tiles_left > 0) {
      return;
    }
    Rack *opponent_rack = player_get_rack(game_get_player(
        opponent_game, game_get_player_on_turn_index(opponent_game)));
    rack_copy(opponent_rack, rack);
    const MoveGenArgs args = {
        .game = opponent_game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&args);
    assert(move_list_get_count(move_list) < THREATS_TEST_MOVE_LIST_CAPACITY);
    for (int move_idx = 0; move_idx < move_list_get_count(move_list);
         move_idx++) {
      placements_add_move(placements, move_list_get_move(move_list, move_idx));
    }
    return;
  }
  const int available = rack_get_letter(pool, ml);
  for (int count = 0; count <= available && count <= tiles_left; count++) {
    add_plays_of_every_rack(opponent_game, pool, rack, ml + 1,
                            tiles_left - count, move_list, placements);
    rack_add_letter(rack, ml);
  }
  for (int count = 0; count <= available && count <= tiles_left; count++) {
    rack_take_letter(rack, ml);
  }
}

// The threats from `pool` are exactly the plays generate_moves finds for the
// opponent from some rack the pool can make, each at the best score any of
// those racks gives it.
static void assert_threats_match_every_rack(const Game *game,
                                            const char *pool_str) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack pool;
  rack_set_dist_size_and_reset(&pool, ld_get_size(ld));
  rack_set_to_string(ld, &pool, pool_str);

  MoveList *move_list = move_list_create(THREATS_TEST_MOVE_LIST_CAPACITY);
  generate_threats(game, &pool, move_list);
  assert(move_list_get_count(move_list) < THREATS_TEST_MOVE_LIST_CAPACITY);
  Placements threats = {0};
  for (int move_idx = 0; move_idx < move_list_get_count(move_list);
       move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    // Sorted by score, from the highest.
    if (move_idx > 0) {
      assert(move_get_score(move) <=
             move_get_score(move_list_get_move(move_list, move_idx - 1)));
    }
    placements_add_move(&threats, move);
  }
  const int num_threats = threats.count;
  placements_keep_best(&threats);
  // Each placement is a threat once.
  assert(threats.count == num_threats);

  Game *opponent_game = game_duplicate(game);
  game_set_player_on_turn_index(opponent_game,
                                1 - game_get_player_on_turn_index(game));
  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_get_size(ld));
  int rack_size = rack_get_total_letters(&pool);
  if (rack_size > RACK_SIZE) {
    rack_size = RACK_SIZE;
  }
  Placements plays = {0};
  add_plays_of_every_rack(opponent_game, &pool, &rack, 0, rack_size, move_list,
                          &plays);
  placements_keep_best(&plays);

  assert(plays.count > 0);
  assert(threats.count == plays.count);
  for (int placement_idx = 0; placement_idx < plays.count; placement_idx++) {
    assert(compare_placements(&threats.placements[placement_idx],
                              &plays.placements[placement_idx]) == 0);
  }

  free(threats.placements);
  free(plays.placements);
  game_destroy(opponent_game);
  move_list_destroy(move_list);
}

static char *run_threats_command(Config *config, const char *command,
                                 error_code_t expected_status) {
  ErrorStack *error_stack = error_stack_create();
  config_load_command(config, command, error_stack);
  assert(error_stack_is_empty(error_stack));
  char *output = NULL;
  config_run_str_api_command(config, error_stack, &output);
  assert(error_stack_top(error_stack) == expected_status);
  error_stack_destroy(error_stack);
  return output;
}

static int count_lines_starting_with_rank(const char *output) {
  int count = 0;
  for (const char *line = output; line != NULL && *line != '\0';) {
    const char *end = strchr(line, '\n');
    if (line[0] >= '1' && line[0] <= '9' && strchr(line, ':') != NULL &&
        (end == NULL || strchr(line, ':') < end)) {
      count++;
    }
    line = end ? end + 1 : NULL;
  }
  return count;
}

static void test_threats_command(void) {
  Config *config = config_create_or_die("set -lex CSW21 -numplays 4");
  load_and_exec_config_or_die(config, "cgp " DOUG_V_EMELY_CGP);

  // -numplays threats by default, as many as asked for otherwise.
  char *output = run_threats_command(config, "threats", ERROR_STATUS_SUCCESS);
  assert(strstr(output, "Unseen tiles (") != NULL);
  assert(count_lines_starting_with_rank(output) == 4);
  free(output);
  output = run_threats_command(config, "threats 2", ERROR_STATUS_SUCCESS);
  assert(count_lines_starting_with_rank(output) == 2);
  free(output);
  output = run_threats_command(config, "threats 0",
                               ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS);
  free(output);

  // At most 100, whether asked for or from -numplays.
  output = run_threats_command(config, "threats 100", ERROR_STATUS_SUCCESS);
  assert(count_lines_starting_with_rank(output) == 100);
  free(output);
  output = run_threats_command(config, "threats 101",
                               ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS);
  free(output);
  load_and_exec_config_or_die(config, "set -numplays 150");
  output = run_threats_command(config, "threats", ERROR_STATUS_SUCCESS);
  assert(count_lines_starting_with_rank(output) == 100);
  free(output);
  load_and_exec_config_or_die(config, "set -numplays 4");

  // The unseen tiles are the opponent's rack and the bag: everything but
  // the board and the rack of the player on turn.
  const Game *game = config_get_game(config);
  Rack unseen;
  threats_get_unseen_tiles(game, &unseen);
  const LetterDistribution *ld = game_get_ld(game);
  const int tiles_on_board = 5;
  assert(rack_get_total_letters(&unseen) ==
         ld_get_total_tiles(ld) - tiles_on_board - RACK_SIZE);

  // The search follows the classic rules, so other variants are refused.
  load_and_exec_config_or_die(config, "set -var wordsmog");
  output = run_threats_command(config, "threats",
                               ERROR_STATUS_THREATS_UNSUPPORTED_GAME);
  free(output);
  config_destroy(config);
}

// Positions from CGPs, which are 15x15.
static void test_threats_in_positions(void) {
  if (BOARD_DIM != 15) {
    return;
  }
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, DOUG_V_EMELY_CGP);
  assert_threats_match_every_rack(game, "AEILOUY");
  assert_threats_match_every_rack(game, "ADEGIL?Z");
  load_cgp_or_die(game, VS_ANDY_CGP);
  assert_threats_match_every_rack(game, "DENQRUW");
  assert_threats_match_every_rack(game, "AEEIRST?");
  game_destroy(game);
  config_destroy(config);
  test_threats_command();
}

void test_threats(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);

  // The opening: from exactly a rack, from more than a rack with two blanks,
  // and from fewer tiles than a rack, where a blank must stand for a letter
  // the pool also holds.
  assert_threats_match_every_rack(game, "AEINST?");
  assert_threats_match_every_rack(game, "EEIRST??Q");
  assert_threats_match_every_rack(game, "EESS?");
  game_destroy(game);
  config_destroy(config);

  test_threats_in_positions();
}
