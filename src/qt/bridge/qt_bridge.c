#include "qt_bridge.h"

#include "../../ent/board.h"
#include "../../ent/board_layout.h"
#include "../../ent/game.h"
#include "../../ent/game_history.h"
#include "../../ent/letter_distribution.h"
#include "../../ent/move.h"
#include "../../ent/players_data.h"
#include "../../ent/rack.h"
#include "../../ent/sim_results.h"
#include "../../ent/thread_control.h"
#include "../../ent/validated_move.h"
#include "../../impl/config.h"
#include "../../impl/gameplay.h"
#include "../../impl/gcg.h"
#include "../../impl/move_gen.h"
#include "../../impl/simmer.h"
#include "../../str/move_string.h"
#include "../../util/io_util.h"
#include "../../util/string_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _BridgeGameHistory {
  Config *config;
};

struct _BridgeGame {
  Game *game;
  Config *config; // Reference
};

// Cast opaque handles
#define TO_GH(x)                                                               \
  (((BridgeGameHistory *)(x))->config                                          \
       ? config_get_game_history(((BridgeGameHistory *)(x))->config)           \
       : NULL)
#define TO_CONFIG(x) (((BridgeGameHistory *)(x))->config)
#define TO_GAME(x) (((BridgeGame *)(x))->game)

static void internal_refresh_ld(Config *cfg, const char *lexicon_name,
                                ErrorStack *err) {
  char *ld_name = ld_get_default_name_from_lexicon_name(lexicon_name, err);
  if (error_stack_is_empty(err)) {
    LetterDistribution *ld =
        ld_create(config_get_data_paths(cfg), ld_name, err);
    if (error_stack_is_empty(err)) {
      config_set_ld(cfg, ld);
    }
  }
  free(ld_name);
}

BridgeGameHistory *bridge_game_history_create(void) {
  BridgeGameHistory *gh = calloc(1, sizeof(BridgeGameHistory));
  return gh;
}

void bridge_game_history_destroy(BridgeGameHistory *gh) {
  if (gh) {
    if (gh->config) {
      config_destroy(gh->config);
    }
    free(gh);
  }
}

int bridge_load_gcg(BridgeGameHistory *gh, const char *gcg_content,
                    const char *data_path, char *error_msg, int error_msg_len) {
  if (!gh)
    return 1;

  if (gh->config) {
    config_destroy(gh->config);
    gh->config = NULL;
  }

  ErrorStack *err = error_stack_create();
  ConfigArgs config_args = {
      .data_paths = data_path, .settings_filename = NULL, .use_wmp = false};
  gh->config = config_create(&config_args, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    snprintf(error_msg, error_msg_len, "Config create failed: %s", msg);
    free(msg);
    error_stack_destroy(err);
    return 1;
  }

  // Pre-load default lexicon (CSW24) to allow loading GCGs without headers
  PlayersData *pd = config_get_players_data(gh->config);
  players_data_set(pd, PLAYERS_DATA_TYPE_KWG, data_path, "CSW24", "CSW24", err);
  internal_refresh_ld(gh->config, "CSW24", err);

  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    snprintf(error_msg, error_msg_len, "Default KWG load failed: %s", msg);
    free(msg);
    error_stack_destroy(err);
    return 1;
  }

  players_data_set(pd, PLAYERS_DATA_TYPE_KLV, data_path, "CSW24", "CSW24", err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    snprintf(error_msg, error_msg_len, "Default KLV load failed: %s", msg);
    free(msg);
    error_stack_destroy(err);
    return 1;
  }

  config_parse_gcg_string(gh->config, gcg_content,
                          config_get_game_history(gh->config), err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    snprintf(error_msg, error_msg_len, "GCG parse failed: %s", msg);
    free(msg);
    error_stack_destroy(err);
    return 1;
  }

  error_stack_destroy(err);
  return 0;
}

BridgeGame *bridge_game_create_from_history(BridgeGameHistory *gh) {
  if (!gh || !gh->config)
    return NULL;

  BridgeGame *b_game = calloc(1, sizeof(BridgeGame));
  b_game->config = gh->config;

  // We create a new game using the config's factory, which links it to
  // PlayersData etc. config_game_create initializes a new game with config
  // settings.
  b_game->game = config_game_create(gh->config);

  return b_game;
}

BridgeGame *bridge_game_clone(BridgeGame *game) {
  if (!game || !game->game)
    return NULL;

  BridgeGame *clone = calloc(1, sizeof(BridgeGame));
  clone->config = game->config; // Share config ref
  clone->game = game_duplicate(game->game);

  return clone;
}

int bridge_game_get_history_num_events(BridgeGame *game) {
  if (!game || !game->config)
    return 0;
  GameHistory *hist = config_get_game_history(game->config);
  if (!hist)
    return 0;
  return game_history_get_num_events(hist);
}

void bridge_game_destroy(BridgeGame *game) {
  if (!game)
    return;
  if (game->game)
    game_destroy(game->game);
  // config is owned by BridgeGameHistory
  free(game);
}

void bridge_game_play_to_index(BridgeGameHistory *gh, BridgeGame *game,
                               int index) {
  if (!game || !gh || !gh->config)
    return;

  ErrorStack *err = error_stack_create();

  if (game->game) {
    game_destroy(game->game);
  }

  // Re-create game to ensure clean state
  game->game = config_game_create(gh->config);

  game_play_n_events(TO_GH(gh), TO_GAME(game), index, true, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    printf("BRIDGE_DEBUG: game_play_n_events failed: %s\n", msg);
    free(msg);
  }
  error_stack_destroy(err);
}

const char *bridge_get_player_name(BridgeGameHistory *gh, int player_index) {
  GameHistory *hist = TO_GH(gh);
  if (!hist)
    return "Unknown";
  return game_history_player_get_name(hist, player_index);
}

int bridge_get_player_score(BridgeGame *game, int player_index) {
  Game *g = TO_GAME(game);
  if (!g)
    return 0;
  return equity_to_int(player_get_score(game_get_player(g, player_index)));
}

int bridge_get_player_on_turn_index(BridgeGame *game) {
  Game *g = TO_GAME(game);
  if (!g)
    return 0;
  return game_get_player_on_turn_index(g);
}

int bridge_get_num_events(BridgeGameHistory *gh) {
  GameHistory *hist = TO_GH(gh);
  if (!hist)
    return 0;
  return game_history_get_num_events(hist);
}

const char *bridge_get_lexicon(BridgeGameHistory *gh) {
  if (!gh)
    return "CSW24";
  GameHistory *hist = TO_GH(gh);
  if (!hist)
    return "CSW24";
  const char *lex = game_history_get_lexicon_name(hist);
  return lex ? lex : "CSW24";
}

char *bridge_get_board_square_string(BridgeGame *game, int row, int col) {
  Game *g = TO_GAME(game);
  if (!g)
    return string_duplicate("");
  Board *b = game_get_board(g);
  MachineLetter ml = board_get_letter(b, row, col);
  if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
    return string_duplicate("");
  }
  const LetterDistribution *ld = game_get_ld(g);
  return string_duplicate(ld_ml_to_hl(ld, ml));
}

uint8_t bridge_get_board_bonus(BridgeGame *game, int row, int col) {
  Game *g = TO_GAME(game);
  if (!g)
    return 0x11; // Default to normal square
  Board *b = game_get_board(g);
  BonusSquare bonus = board_get_bonus_square(b, row, col);
  return bonus.raw;
}

uint8_t bridge_get_machine_letter(BridgeGame *game, int row, int col) {
  Game *g = TO_GAME(game);
  if (!g)
    return ALPHABET_EMPTY_SQUARE_MARKER;
  Board *b = game_get_board(g);
  return board_get_letter(b, row, col);
}

int bridge_get_letter_score(BridgeGame *game, uint8_t ml) {
  Game *g = TO_GAME(game);
  if (!g)
    return 0;
  const LetterDistribution *ld = game_get_ld(g);
  if (bridge_is_blank(ml)) {
    return equity_to_int(ld_get_score(ld, BLANK_MACHINE_LETTER));
  }
  return equity_to_int(ld_get_score(ld, ml));
}

bool bridge_is_blank(uint8_t ml) { return get_is_blanked(ml); }

static char *internal_format_rack(const Rack *r, const LetterDistribution *ld) {
  if (!r || !ld)
    return string_duplicate("");
  char buffer[64] = {0};
  int pos = 0;
  for (int i = 0; i < ld_get_size(ld); i++) {
    int count = rack_get_letter(r, i);
    if (count > 0) {
      char *hl = ld_ml_to_hl(ld, i);
      for (int c = 0; c < count; c++) {
        if (pos + strlen(hl) < sizeof(buffer) - 1) {
          strcpy(buffer + pos, hl);
          pos += strlen(hl);
        }
      }
    }
  }
  return string_duplicate(buffer);
}

char *bridge_get_current_rack(BridgeGame *game) {
  Game *g = TO_GAME(game);
  if (!g) {
    fprintf(stderr, "bridge_get_current_rack: game is NULL\n");
    return string_duplicate("");
  }

  int playerIdx = game_get_player_on_turn_index(g);
  Player *p = game_get_player(g, playerIdx);
  Rack *r = player_get_rack(p);
  const LetterDistribution *ld = game_get_ld(g);
  Bag *bag = game_get_bag(g);

  int totalTiles = r ? rack_get_total_letters(r) : -1;
  int bagTiles = bag ? bag_get_letters(bag) : -1;
  int ldSize = ld ? ld_get_size(ld) : -1;

  fprintf(stderr,
          "bridge_get_current_rack: player=%d, rack_tiles=%d, bag_tiles=%d, "
          "ld_size=%d\n",
          playerIdx, totalTiles, bagTiles, ldSize);

  return internal_format_rack(r, ld);
}

// Get number of tiles in bag
int bridge_get_bag_count(BridgeGame *game) {
  Game *g = TO_GAME(game);
  if (!g)
    return 0;
  return bag_get_letters(game_get_bag(g));
}

// Get unseen tiles (bag + opponent rack)
// Returns string in *tiles (caller must free), and counts
void bridge_get_unseen_tiles(BridgeGame *game, char **tiles, int *vowel_count,
                             int *consonant_count, int *blank_count) {
  Game *g = TO_GAME(game);
  if (!g)
    return;

  int playerIdx = game_get_player_on_turn_index(g);
  int opponentIdx = 1 - playerIdx;

  Bag *bag = game_get_bag(g);
  Player *opponent = game_get_player(g, opponentIdx);
  Rack *opponentRack = player_get_rack(opponent);
  const LetterDistribution *ld = game_get_ld(g);
  Board *board = game_get_board(g);

  int unseen_counts[MAX_ALPHABET_SIZE];
  memset(unseen_counts, 0, sizeof(unseen_counts));

  // Add bag tiles
  bag_increment_unseen_count(bag, unseen_counts);

  // Add opponent rack tiles
  for (int i = 0; i < ld_get_size(ld); i++) {
    unseen_counts[i] += rack_get_letter(opponentRack, i);
  }

  StringBuilder *sb = string_builder_create();
  int v = 0;
  int c = 0;
  int b = 0;

  for (int i = 0; i < ld_get_size(ld); i++) {
    if (unseen_counts[i] > 0) {
      // Check if blank
      if (bridge_is_blank((uint8_t)i) || i == BLANK_MACHINE_LETTER) {
        b += unseen_counts[i];
      } else if (ld_get_is_vowel(ld, i)) {
        v += unseen_counts[i];
      } else {
        c += unseen_counts[i];
      }

      char *hl = ld_ml_to_hl(ld, i);
      for (int k = 0; k < unseen_counts[i]; k++) {
        string_builder_add_string(sb, hl);
      }

      string_builder_add_string(sb, " ");
    }
  }

  if (tiles)
    *tiles = string_duplicate(string_builder_peek(sb));
  string_builder_destroy(sb);

  // Invariant Calculation for Unknowns
  int total_tiles = ld_get_total_tiles(ld);
  int tiles_on_board = board_get_tiles_played(board);
  int p1_tiles =
      rack_get_total_letters(player_get_rack(game_get_player(g, playerIdx)));
  int bag_tiles = bag_get_letters(bag);
  int opp_tiles = rack_get_total_letters(opponentRack);

  int true_unseen = total_tiles - tiles_on_board - p1_tiles;
  int visible_unseen = bag_tiles + opp_tiles;
  int unknown = true_unseen - visible_unseen;

  if (unknown < 0)
    unknown = 0;

  if (vowel_count)
    *vowel_count = v;
  if (consonant_count)
    *consonant_count = c;
  if (blank_count)
    *blank_count = b + unknown;
}

int bridge_get_board_tiles_played(BridgeGame *game) {
  Game *g = TO_GAME(game);
  if (!g)
    return 0;
  return board_get_tiles_played(game_get_board(g));
}

int bridge_get_current_event_index(BridgeGame *game) {
  if (!game || !game->config)
    return 0;
  // We can get the current index from the game history attached to the config.
  // But wait, BridgeGame creates a new Game.
  // The UI tracks `currentEventIndex`.
  // Actually, the bridge doesn't inherently know the "current event index" of
  // the UI state unless we pass it or track it in BridgeGame. However,
  // `game_history` object knows how many events have been *played* onto the
  // `Game` object? No, `Game` object stores state, `GameHistory` stores events.
  // `game_play_n_events` replays history.

  // BUT, AnalysisModel needs to know if there are *any* events in the history
  // to decide whether to sim. "empty board is not the condition... it's whether
  // the game history has events in it." So we actually want
  // `bridge_get_num_events(gh)`.

  // The user said "sim shouldn't happen on default empty game state".
  // Default empty game state has 0 events.
  // So `bridge_get_num_events` > 0 check in AnalysisModel should suffice?

  // Wait, "loaded a game and didn't get any analysis".
  // If we load a game, num_events > 0.
  // But if we jump to index 0 (start of game), we might still want analysis for
  // the opening move? MAGPIE's `generate_moves` works for the opening move
  // (empty board). The issue was "win_pct shouldn't be called with > 93". At
  // start of game, unseen is 100 (bag 86 + opp 14? No, bag + opp rack).
  // Standard bag 100 tiles. 7 on rack. 93 unseen.
  // Wait, `win_pct` usually goes up to 100 or more?
  // `src/ent/win_pct.c` checks `wp->max_tiles_unseen`.
  // If the `win_pct` file only has data up to 93 tiles (common for endgame
  // tables), then it fails for start of game. BUT, opening book / static eval
  // should handle start of game without looking up win pct for endgame?

  // If we are at the start of the game (index 0), we want to generate moves for
  // the *first* move. The board is empty. If we are at index N, we want to
  // generate moves for the (N+1)th move? Or the move that *was* played at N?
  // Analysis usually shows "what is the best move here?".
  // So if we are at state 0, we want best moves for player 1's first turn.

  // The crash `cannot get win percentage value for 94 unseen tiles` implies we
  // ARE trying to sim. And `win_pct` lookup is failing. If `win_pct` data is
  // limited, we shouldn't sim if unseen > max. OR, we should rely on static
  // evaluation (score) instead of win % for early game.

  return 0; // Placeholder if needed, but logic above suggests check elsewhere.
}

void bridge_get_event_details(BridgeGameHistory *gh, BridgeGame *game,
                              int index, int *player_index, int *type,
                              char **move_str, char **rack_str, int *score,
                              int *cumulative_score) {
  GameHistory *hist = TO_GH(gh);
  Game *g = TO_GAME(game);
  if (!hist || !g)
    return;

  GameEvent *event = game_history_get_event(hist, index);
  if (!event)
    return;

  if (player_index)
    *player_index = game_event_get_player_index(event);
  if (type)
    *type = (int)game_event_get_type(event);
  if (score)
    *score = equity_to_int(game_event_get_move_score(event));
  if (cumulative_score)
    *cumulative_score = equity_to_int(game_event_get_cumulative_score(event));

  if (move_str) {
    char *human_readable = NULL;

    ValidatedMoves *vms = game_event_get_vms(event);
    if (vms && validated_moves_get_number_of_moves(vms) > 0) {
      const Move *move = validated_moves_get_move(vms, 0);
      game_event_t type = move_get_type(move);

      if (type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
          type == GAME_EVENT_EXCHANGE || type == GAME_EVENT_PASS) {

        // Temporarily rewind game to state before this move to get proper board
        // state This is inefficient (O(N^2)) but ensures accurate notation
        // generation Since we are using Config factory, we can create a temp
        // game easily.

        Game *temp_game = config_game_create(gh->config);
        ErrorStack *err = error_stack_create();
        // We don't reset because it's fresh.
        game_play_n_events(hist, temp_game, index, true, err);
        error_stack_destroy(err);

        Board *board = game_get_board(temp_game);
        const LetterDistribution *ld = game_get_ld(temp_game);

        StringBuilder *sb = string_builder_create();
        string_builder_add_human_readable_move(sb, move, board, ld);

        human_readable = string_duplicate(string_builder_peek(sb));
        string_builder_destroy(sb);
        game_destroy(temp_game);
      }
    }

    if (!human_readable) {
      game_event_t type = game_event_get_type(event);
      switch (type) {
      case GAME_EVENT_CHALLENGE_BONUS:
        human_readable = string_duplicate("challenged");
        break;
      case GAME_EVENT_PHONY_TILES_RETURNED:
        human_readable = string_duplicate("phony");
        break;
      case GAME_EVENT_TIME_PENALTY:
        human_readable = string_duplicate("time");
        break;
      case GAME_EVENT_END_RACK_POINTS: {
        const Rack *r = game_event_get_const_rack(event);
        char *tiles = internal_format_rack(r, game_get_ld(g));
        char buf[128];
        snprintf(buf, sizeof(buf), "2x %s", tiles);
        free(tiles);
        human_readable = string_duplicate(buf);
        break;
      }
      case GAME_EVENT_END_RACK_PENALTY:
        human_readable = string_duplicate("rack penalty");
        break;
      default:
        break;
      }
    }

    if (human_readable) {
      *move_str = human_readable;
    } else {
      const char *s = game_event_get_cgp_move_string(event);
      *move_str = s ? string_duplicate(s) : string_duplicate("");
    }
  }

  if (rack_str) {
    const Rack *r = game_event_get_const_rack(event);
    if (r) {
      *rack_str = internal_format_rack(r, game_get_ld(g));
    } else {
      *rack_str = string_duplicate("");
    }
  }
}

int bridge_get_last_move_tiles(BridgeGameHistory *gh, int index, int *rows,
                               int *cols, int max_count) {
  GameHistory *hist = TO_GH(gh);
  if (!hist)
    return 0;

  GameEvent *event = game_history_get_event(hist, index);
  if (!event)
    return 0;

  ValidatedMoves *vms = game_event_get_vms(event);
  if (!vms || validated_moves_get_number_of_moves(vms) == 0)
    return 0;

  const Move *move = validated_moves_get_move(vms, 0);
  if (!move)
    return 0;

  if (move->move_type != GAME_EVENT_TILE_PLACEMENT_MOVE)
    return 0;

  int count = 0;
  int r = move->row_start;
  int c = move->col_start;
  int ri = (move->dir == BOARD_VERTICAL_DIRECTION) ? 1 : 0;
  int ci = (move->dir == BOARD_HORIZONTAL_DIRECTION) ? 1 : 0;

  for (int i = 0; i < move->tiles_length; i++) {
    // Check if tile is placed. Assuming 0 means existing tile.
    if (move->tiles[i] != 0) {
      if (rows && cols && count < max_count) {
        rows[count] = r;
        cols[count] = c;
      }
      count++;
    }
    r += ri;
    c += ci;
  }

  return count;
}

// -----------------------------------------------------------------------------
// Analysis Implementation
// -----------------------------------------------------------------------------

#include "../../ent/sim_results.h"
#include "../../ent/thread_control.h"
#include "../../impl/move_gen.h"
#include "../../impl/simmer.h"

BridgeMoveList *bridge_generate_moves(BridgeGame *game) {
  if (!game || !game->game || !game->config)
    return NULL;

  // MoveList *move_list_create(int capacity);
  MoveList *ml = move_list_create(100);

  MoveGenArgs args;
  memset(&args, 0, sizeof(MoveGenArgs));
  args.game = TO_GAME(game);

  // Use config settings
  Config *cfg = game->config;
  PlayersData *pd = config_get_players_data(cfg);
  int playerIdx = game_get_player_on_turn_index(TO_GAME(game));

  args.move_record_type = players_data_get_move_record_type(pd, playerIdx);
  // Force record all for analysis? Or respect config?
  // Usually we want all moves for analysis.
  args.move_record_type = MOVE_RECORD_ALL;

  args.move_sort_type = players_data_get_move_sort_type(pd, playerIdx);

  // Ensure eq margin is set if needed
  // args.eq_margin_movegen = ...

  args.thread_index = 0;
  args.move_list = ml;
  // args.override_kwg = NULL;

  generate_moves(&args);

  // Sort by equity/score? generate_moves usually leaves them in heap or sorted
  // depending on impl. Let's ensure sorted.
  move_list_sort_moves(ml);

  return (BridgeMoveList *)ml;
}

void bridge_move_list_destroy(BridgeMoveList *ml) {
  if (ml) {
    move_list_destroy((MoveList *)ml);
  }
}

BridgeSimResults *bridge_sim_results_create(void) {
  return (BridgeSimResults *)sim_results_create(0.99);
}

void bridge_sim_results_destroy(BridgeSimResults *sr) {
  if (sr) {
    sim_results_destroy((SimResults *)sr);
  }
}

BridgeThreadControl *bridge_thread_control_create(void) {
  return (BridgeThreadControl *)thread_control_create();
}

void bridge_thread_control_destroy(BridgeThreadControl *tc) {
  if (tc) {
    thread_control_destroy((ThreadControl *)tc);
  }
}

void bridge_thread_control_stop(BridgeThreadControl *tc) {
  if (tc) {
    thread_control_set_status((ThreadControl *)tc,
                              THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
}

void bridge_simulate(BridgeGame *game, BridgeMoveList *moves,
                     BridgeSimResults *results, BridgeThreadControl *tc,
                     int plies) {
  if (!game || !game->config || !moves || !results || !tc)
    return;

  SimArgs args;
  memset(&args, 0, sizeof(SimArgs));
  args.num_plies = plies;
  // Use the game from BridgeGame (which reflects UI state), not config->game
  args.game = TO_GAME(game);
  args.move_list = (MoveList *)moves;
  args.thread_control = (ThreadControl *)tc;
  args.num_threads = 10;

  // Inherit settings from config
  Config *cfg = game->config;
  args.seed = config_get_seed(cfg);

  ErrorStack *err = error_stack_create();

  // Lazy-load win_pcts if not already loaded
  args.win_pcts = config_load_win_pcts(cfg, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    printf("Failed to load win percentages: %s\n", msg);
    free(msg);
    error_stack_destroy(err);
    return;
  }

  // BAI Options
  args.bai_options.threshold = BAI_THRESHOLD_GK16;
  args.bai_options.sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS;

  // Force high sample limit for GUI analysis (default 5000 is too low for deep
  // analysis)
  args.bai_options.sample_limit = 500000;
  args.bai_options.sample_minimum = 100; // Default
  args.bai_options.num_threads = 10;

  // Initialize results
  sim_results_reset((MoveList *)moves, (SimResults *)results, plies, 0, false);

  // Start the thread control
  thread_control_set_status((ThreadControl *)tc, THREAD_CONTROL_STATUS_STARTED);

  SimCtx *sim_ctx = NULL;
  simulate(&args, &sim_ctx, (SimResults *)results, err);
  // sim_ctx is cleaned up by simulate

  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    printf("Simulation Error: %s\n", msg);
    free(msg);
  }
  error_stack_destroy(err);
}

int bridge_sim_results_get_num_plays(BridgeSimResults *results) {
  if (!results)
    return 0;
  return sim_results_get_number_of_plays((SimResults *)results);
}

int bridge_sim_results_get_play_info(BridgeGame *game,
                                     BridgeSimResults *results, int index,
                                     char **notation, double *win_pct,
                                     double *spread, int *iterations) {
  if (!results || !game)
    return 1;

  SimResults *sr = (SimResults *)results;

  sim_results_lock_simmed_plays(sr);

  if (index < 0 || index >= sim_results_get_number_of_plays(sr)) {
    sim_results_unlock_simmed_plays(sr);
    return 1;
  }

  SimmedPlay *sp = sim_results_get_simmed_play(sr, index);

  if (win_pct) {
    Stat *wp = simmed_play_get_win_pct_stat(sp);
    *win_pct = stat_get_mean(wp);
    if (iterations) {
      *iterations = (int)stat_get_num_samples(wp);
    }
  }
  if (spread) {
    Stat *eq = simmed_play_get_equity_stat(sp);
    *spread = stat_get_mean(eq);
  }

  if (notation) {
    Move *m = simmed_play_get_move(sp);
    Board *board = game_get_board(TO_GAME(game));
    const LetterDistribution *ld = game_get_ld(TO_GAME(game));

    StringBuilder *sb = string_builder_create();
    string_builder_add_human_readable_move(sb, m, board, ld);
    *notation = string_duplicate(string_builder_peek(sb));
    string_builder_destroy(sb);
  }

  sim_results_unlock_simmed_plays(sr);
  return 0;
}

uint64_t bridge_sim_results_get_iterations(BridgeSimResults *results) {
  if (!results)
    return 0;
  return sim_results_get_iteration_count((SimResults *)results);
}

double bridge_sim_results_get_confidence(BridgeSimResults *results) {
  if (!results)
    return 0.0;
  SimResults *sr = (SimResults *)results;
  BAIResult *bai = sim_results_get_bai_result(sr);
  return bai_result_get_confidence(bai);
}

// -----------------------------------------------------------------------------
// Gameplay Implementation
// -----------------------------------------------------------------------------

BridgeGameHistory *bridge_game_create_fresh(const char *data_path,
                                            const char *lexicon_name,
                                            char *error_msg,
                                            int error_msg_len) {
  if (!data_path || !lexicon_name)
    return NULL;

  BridgeGameHistory *gh = bridge_game_history_create();
  ErrorStack *err = error_stack_create();

  ConfigArgs config_args2 = {
      .data_paths = data_path, .settings_filename = NULL, .use_wmp = false};
  gh->config = config_create(&config_args2, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    snprintf(error_msg, error_msg_len, "Config create failed: %s", msg);
    free(msg);
    error_stack_destroy(err);
    bridge_game_history_destroy(gh);
    return NULL;
  }

  PlayersData *pd = config_get_players_data(gh->config);
  // Use the provided lexicon for both players
  players_data_set(pd, PLAYERS_DATA_TYPE_KWG, data_path, lexicon_name,
                   lexicon_name, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    snprintf(error_msg, error_msg_len, "KWG load failed for %s: %s",
             lexicon_name, msg);
    free(msg);
    error_stack_destroy(err);
    bridge_game_history_destroy(gh);
    return NULL;
  }

  players_data_set(pd, PLAYERS_DATA_TYPE_KLV, data_path, lexicon_name,
                   lexicon_name, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    snprintf(error_msg, error_msg_len, "KLV load failed for %s: %s",
             lexicon_name, msg);
    free(msg);
    error_stack_destroy(err);
    bridge_game_history_destroy(gh);
    return NULL;
  }

  internal_refresh_ld(gh->config, lexicon_name, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    snprintf(error_msg, error_msg_len, "LD load failed for %s: %s",
             lexicon_name, msg);
    free(msg);
    error_stack_destroy(err);
    bridge_game_history_destroy(gh);
    return NULL;
  }

  error_stack_destroy(err);
  return gh;
}

char *bridge_validate_move(BridgeGame *game, const char *notation) {
  if (!game || !game->game || !notation)
    return string_duplicate("Invalid game or move string");

  ErrorStack *err = error_stack_create();
  int playerIdx = game_get_player_on_turn_index(game->game);

  ValidatedMoves *vms = validated_moves_create(game->game, playerIdx, notation,
                                               true,  // allow_phonies
                                               false, // allow_unknown_exchanges
                                               true,  // allow_playthrough
                                               err);

  char *error_msg = NULL;
  if (!error_stack_is_empty(err)) {
    error_msg = error_stack_get_string_and_reset(err);
  }

  validated_moves_destroy(vms);
  error_stack_destroy(err);

  return error_msg;
}

int bridge_preview_move(BridgeGame *game, const char *ucgi_notation,
                        char **notation_out, int *score_out,
                        bool *is_phony_out, char **leave_out,
                        char **error_out) {
  if (!game || !game->game || !ucgi_notation) {
    if (error_out)
      *error_out = string_duplicate("Invalid game or move string");
    return 1;
  }

  ErrorStack *err = error_stack_create();
  Game *g = game->game;
  int playerIdx = game_get_player_on_turn_index(g);

  ValidatedMoves *vms = validated_moves_create(g, playerIdx, ucgi_notation,
                                               true,  // allow_phonies
                                               false, // allow_unknown_exchanges
                                               true,  // allow_playthrough
                                               err);

  if (!error_stack_is_empty(err) || !vms ||
      validated_moves_get_number_of_moves(vms) == 0) {
    if (error_out) {
      *error_out = error_stack_is_empty(err)
                       ? string_duplicate("No valid move found")
                       : error_stack_get_string_and_reset(err);
    }
    validated_moves_destroy(vms);
    error_stack_destroy(err);
    return 1;
  }

  const Move *m = validated_moves_get_move(vms, 0);
  const LetterDistribution *ld = game_get_ld(g);

  if (score_out)
    *score_out = equity_to_int(move_get_score(m));
  if (is_phony_out)
    *is_phony_out = validated_moves_is_phony(vms, 0);

  if (notation_out) {
    Board *board = game_get_board(g);
    StringBuilder *sb = string_builder_create();
    string_builder_add_human_readable_move(sb, m, board, ld);
    *notation_out = string_duplicate(string_builder_peek(sb));
    string_builder_destroy(sb);
  }

  if (leave_out) {
    Rack *leave_rack =
        rack_duplicate(player_get_rack(game_get_player(g, playerIdx)));
    if (move_get_type(m) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      for (int i = 0; i < move_get_tiles_length(m); i++) {
        MachineLetter tile = move_get_tile(m, i);
        if (tile == 0)
          continue; // playthrough
        if (get_is_blanked(tile)) {
          rack_take_letter(leave_rack, BLANK_MACHINE_LETTER);
        } else {
          rack_take_letter(leave_rack, tile);
        }
      }
    }
    *leave_out = internal_format_rack(leave_rack, ld);
    rack_destroy(leave_rack);
  }

  validated_moves_destroy(vms);
  error_stack_destroy(err);
  return 0;
}

char *bridge_play_move(BridgeGameHistory *gh, BridgeGame *game,
                       const char *notation) {
  if (!game || !game->game || !notation || !gh || !gh->config)
    return string_duplicate("Invalid game or move string");

  ErrorStack *err = error_stack_create();
  Game *g = game->game;
  GameHistory *hist = config_get_game_history(gh->config);
  int playerIdx = game_get_player_on_turn_index(g);

  ValidatedMoves *vms = validated_moves_create(g, playerIdx, notation,
                                               true,  // allow_phonies
                                               false, // allow_unknown_exchanges
                                               true,  // allow_playthrough
                                               err);

  if (!error_stack_is_empty(err) || vms == NULL) {
    char *error_msg = (vms == NULL && error_stack_is_empty(err))
                          ? string_duplicate("Validation failed")
                          : error_stack_get_string_and_reset(err);
    validated_moves_destroy(vms);
    error_stack_destroy(err);
    return error_msg;
  }

  if (validated_moves_get_number_of_moves(vms) == 0) {
    validated_moves_destroy(vms);
    error_stack_destroy(err);
    return string_duplicate("No valid move found");
  }

  const Move *m = validated_moves_get_move(vms, 0);

  // Create new history event
  GameEvent *event = game_history_add_game_event(hist, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    validated_moves_destroy(vms);
    error_stack_destroy(err);
    return msg;
  }

  // Populate event
  game_event_set_player_index(event, playerIdx);
  game_event_set_type(event, (game_event_t)move_get_type(m));
  game_event_set_move_score(event, move_get_score(m));

  // Record current rack into event
  Rack *event_rack = game_event_get_rack(event);
  rack_copy(event_rack, player_get_rack(game_get_player(g, playerIdx)));

  // UCGI move string
  StringBuilder *sb = string_builder_create();
  string_builder_add_ucgi_move(sb, m, game_get_board(g), game_get_ld(g));
  game_event_set_cgp_move_string(event,
                                 string_duplicate(string_builder_peek(sb)));
  string_builder_destroy(sb);

  // Play move on game
  play_move_without_drawing_tiles(m, g);
  draw_to_full_rack(g, playerIdx);

  // Set cumulative score in event
  game_event_set_cumulative_score(
      event, player_get_score(game_get_player(g, playerIdx)));

  game_event_set_vms(event, vms);
  error_stack_destroy(err);

  return NULL;
}

char *bridge_challenge_last_move(BridgeGameHistory *gh, BridgeGame *game) {
  if (!game || !game->game || !gh || !gh->config)
    return string_duplicate("Invalid game state");

  GameHistory *hist = TO_GH(gh);
  Game *g = TO_GAME(game);
  int num_events = game_history_get_num_events(hist);
  if (num_events == 0)
    return string_duplicate("No events to challenge");

  GameEvent *last_event = game_history_get_event(hist, num_events - 1);
  if (game_event_get_type(last_event) != GAME_EVENT_TILE_PLACEMENT_MOVE)
    return string_duplicate("Last event is not a tile placement");

  int phony_player = game_event_get_player_index(last_event);
  Equity move_score = game_event_get_move_score(last_event);
  const Rack *pre_move_rack = game_event_get_const_rack(last_event);

  // Get the move tiles from the validated moves
  ValidatedMoves *vms = game_event_get_vms(last_event);
  if (!vms || validated_moves_get_number_of_moves(vms) == 0)
    return string_duplicate("No validated move data");
  const Move *move = validated_moves_get_move(vms, 0);

  // 1. Return the phony player's current rack (post-draw) to the bag
  return_rack_to_bag(g, phony_player);

  // 2. Remove placed tiles from the board and return them to the bag
  Board *board = game_get_board(g);
  int row = move_get_row_start(move);
  int col = move_get_col_start(move);
  int dir = move_get_dir(move);
  int ri = (dir == BOARD_VERTICAL_DIRECTION) ? 1 : 0;
  int ci = (dir == BOARD_HORIZONTAL_DIRECTION) ? 1 : 0;
  int tiles_removed = 0;

  for (int i = 0; i < move_get_tiles_length(move); i++) {
    MachineLetter tile = move_get_tile(move, i);
    if (tile != 0) { // 0 = playthrough (existing tile)
      board_set_letter(board, row, col, ALPHABET_EMPTY_SQUARE_MARKER);
      // Return the tile to the bag (blanks go back as BLANK_MACHINE_LETTER)
      MachineLetter bag_letter =
          get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
      bag_add_letter(game_get_bag(g), bag_letter, phony_player);
      tiles_removed++;
    }
    row += ri;
    col += ci;
  }
  board_set_tiles_played(board, board_get_tiles_played(board) - tiles_removed);

  // 3. Rebuild is_cross_word flags and anchors (board_set_letter with
  //    EMPTY_SQUARE_MARKER does not clear is_cross_word on adjacent squares,
  //    and anchors are not updated by game_gen_all_cross_sets)
  for (int r = 0; r < BOARD_DIM; r++) {
    for (int c = 0; c < BOARD_DIM; c++) {
      board_reset_is_cross_word(board, r, c, BOARD_HORIZONTAL_DIRECTION);
      board_reset_is_cross_word(board, r, c, BOARD_VERTICAL_DIRECTION);
    }
  }
  // Re-derive is_cross_word from remaining tiles on the board
  for (int r = 0; r < BOARD_DIM; r++) {
    for (int c = 0; c < BOARD_DIM; c++) {
      if (!board_is_empty(board, r, c)) {
        for (int csi = 0; csi < 2; csi++) {
          if (r > 0)
            square_set_is_cross_word(
                board_get_writable_square(board, r - 1, c,
                                          BOARD_HORIZONTAL_DIRECTION, csi),
                true);
          if (r < BOARD_DIM - 1)
            square_set_is_cross_word(
                board_get_writable_square(board, r + 1, c,
                                          BOARD_HORIZONTAL_DIRECTION, csi),
                true);
          if (c > 0)
            square_set_is_cross_word(
                board_get_writable_square(board, r, c - 1,
                                          BOARD_VERTICAL_DIRECTION, csi),
                true);
          if (c < BOARD_DIM - 1)
            square_set_is_cross_word(
                board_get_writable_square(board, r, c + 1,
                                          BOARD_VERTICAL_DIRECTION, csi),
                true);
        }
      }
    }
  }
  board_update_all_anchors(board);

  // 4. Regenerate all cross sets (board changed)
  game_gen_all_cross_sets(g);

  // 5. Draw the pre-move rack from the bag (restores original tiles)
  draw_rack_from_bag(g, phony_player, pre_move_rack);

  // 6. Restore the phony player's score
  Player *phony_p = game_get_player(g, phony_player);
  player_add_to_score(phony_p, -move_score);

  // 7. Increment consecutive scoreless turns
  game_increment_consecutive_scoreless_turns(g);

  // 8. Add PHONY_TILES_RETURNED event to history
  ErrorStack *err = error_stack_create();
  GameEvent *phony_event = game_history_add_game_event(hist, err);
  if (!error_stack_is_empty(err)) {
    char *msg = error_stack_get_string_and_reset(err);
    error_stack_destroy(err);
    return msg;
  }

  game_event_set_player_index(phony_event, phony_player);
  game_event_set_type(phony_event, GAME_EVENT_PHONY_TILES_RETURNED);
  game_event_set_move_score(phony_event, int_to_equity(0));
  game_event_set_cumulative_score(phony_event, player_get_score(phony_p));

  Rack *phony_event_rack = game_event_get_rack(phony_event);
  rack_copy(phony_event_rack, pre_move_rack);

  error_stack_destroy(err);
  return NULL;
}

void bridge_get_event_rack_info(BridgeGameHistory *gh, BridgeGame *game,
                                int index, char **played_tiles_out,
                                char **leave_out, char **full_rack_out) {
  GameHistory *hist = TO_GH(gh);
  Game *g = TO_GAME(game);
  if (!hist || !g) {
    if (played_tiles_out)
      *played_tiles_out = string_duplicate("");
    if (leave_out)
      *leave_out = string_duplicate("");
    if (full_rack_out)
      *full_rack_out = string_duplicate("");
    return;
  }

  GameEvent *event = game_history_get_event(hist, index);
  if (!event) {
    if (played_tiles_out)
      *played_tiles_out = string_duplicate("");
    if (leave_out)
      *leave_out = string_duplicate("");
    if (full_rack_out)
      *full_rack_out = string_duplicate("");
    return;
  }

  game_event_t type = game_event_get_type(event);
  const LetterDistribution *ld = game_get_ld(g);
  int ld_size = ld_get_size(ld);

  if (type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    // For non-tile-placement events, just return the event rack as full rack
    const Rack *event_rack = game_event_get_const_rack(event);
    if (played_tiles_out)
      *played_tiles_out = string_duplicate("");
    if (leave_out)
      *leave_out = string_duplicate("");
    if (full_rack_out)
      *full_rack_out = event_rack ? internal_format_rack(event_rack, ld)
                                  : string_duplicate("");
    return;
  }

  ValidatedMoves *vms = game_event_get_vms(event);
  if (!vms || validated_moves_get_number_of_moves(vms) == 0) {
    const Rack *event_rack = game_event_get_const_rack(event);
    if (played_tiles_out)
      *played_tiles_out = string_duplicate("");
    if (leave_out)
      *leave_out = string_duplicate("");
    if (full_rack_out)
      *full_rack_out = event_rack ? internal_format_rack(event_rack, ld)
                                  : string_duplicate("");
    return;
  }

  const Move *move = validated_moves_get_move(vms, 0);

  // Extract played tiles from the Move
  Rack played;
  played.dist_size = ld_size;
  rack_reset(&played);
  for (int i = 0; i < move_get_tiles_length(move); i++) {
    MachineLetter tile = move_get_tile(move, i);
    if (tile == 0)
      continue; // playthrough
    if (get_is_blanked(tile)) {
      rack_add_letter(&played, BLANK_MACHINE_LETTER);
    } else {
      rack_add_letter(&played, tile);
    }
  }

  // Determine if event rack is the full pre-move rack or the leave
  const Rack *event_rack = game_event_get_const_rack(event);
  Rack test_rack;
  rack_copy(&test_rack, event_rack);

  if (rack_subtract(&test_rack, &played)) {
    // event_rack contained the played tiles, so it's the full rack
    // test_rack is now the leave (full - played)
    if (played_tiles_out)
      *played_tiles_out = internal_format_rack(&played, ld);
    if (leave_out)
      *leave_out = internal_format_rack(&test_rack, ld);
    if (full_rack_out)
      *full_rack_out = internal_format_rack(event_rack, ld);
  } else {
    // event_rack doesn't contain the played tiles, so it's the leave
    Rack full;
    rack_copy(&full, event_rack);
    rack_add(&full, &played);
    if (played_tiles_out)
      *played_tiles_out = internal_format_rack(&played, ld);
    if (leave_out)
      *leave_out = internal_format_rack(event_rack, ld);
    if (full_rack_out)
      *full_rack_out = internal_format_rack(&full, ld);
  }
}

char *bridge_get_computer_move(BridgeGame *game) {
  if (!game || !game->game)
    return NULL;

  MoveList *ml = move_list_create(1);

  // Use thread_index 0 for GUI main thread calls
  Move *top_move = get_top_equity_move(game->game, 0, ml);

  if (!top_move) {
    move_list_destroy(ml);
    return NULL;
  }

  StringBuilder *sb = string_builder_create();
  string_builder_add_ucgi_move(sb, top_move, game_get_board(game->game),
                               game_get_ld(game->game));
  char *move_string = string_duplicate(string_builder_peek(sb));

  string_builder_destroy(sb);
  move_list_destroy(ml);

  return move_string;
}

void bridge_game_draw_racks(BridgeGame *game) {
  if (!game || !game->game) {
    fprintf(stderr, "bridge_game_draw_racks: game or game->game is NULL\n");
    return;
  }

  Game *g = game->game;
  Bag *bag = game_get_bag(g);
  int bag_before = bag ? bag_get_letters(bag) : -1;

  fprintf(stderr, "bridge_game_draw_racks: bag_count_before=%d\n", bag_before);
  draw_starting_racks(g);

  int bag_after = bag ? bag_get_letters(bag) : -1;
  Player *p0 = game_get_player(g, 0);
  Player *p1 = game_get_player(g, 1);
  int r0 = p0 ? rack_get_total_letters(player_get_rack(p0)) : -1;
  int r1 = p1 ? rack_get_total_letters(player_get_rack(p1)) : -1;

  fprintf(stderr, "bridge_game_draw_racks: bag_after=%d, rack0=%d, rack1=%d\n",
          bag_after, r0, r1);
}
