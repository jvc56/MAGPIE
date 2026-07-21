#include "json_api.h"

#include "../def/board_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/board_layout.h"
#include "../ent/bonus_square.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/players_data.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/string_util.h"
#include "cgp.h"
#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Upper bound on the number of moves serialized in one json_api_get_moves call.
// Generate can produce thousands of plays; the UI only ever displays the top
// handful, so capping keeps the payload small. "total" reports the true count.
enum { JSON_MAX_MOVES = 300 };

// Appends a JSON string literal (surrounding quotes plus escaping) for s.
// A NULL s is emitted as an empty string "".
static void json_add_escaped_string(StringBuilder *sb, const char *s) {
  string_builder_add_char(sb, '"');
  if (s) {
    for (const char *p = s; *p; p++) {
      const unsigned char c = (unsigned char)*p;
      switch (c) {
      case '"':
        string_builder_add_string(sb, "\\\"");
        break;
      case '\\':
        string_builder_add_string(sb, "\\\\");
        break;
      case '\n':
        string_builder_add_string(sb, "\\n");
        break;
      case '\r':
        string_builder_add_string(sb, "\\r");
        break;
      case '\t':
        string_builder_add_string(sb, "\\t");
        break;
      default:
        if (c < 0x20) {
          string_builder_add_formatted_string(sb, "\\u%04x", (unsigned int)c);
        } else {
          string_builder_add_char(sb, (char)c);
        }
        break;
      }
    }
  }
  string_builder_add_char(sb, '"');
}

// Appends the user-visible display string for a machine letter as a JSON
// string literal. Blanks render lowercase (handled by ld_ml_to_hl).
static void json_add_tile_letter(StringBuilder *sb,
                                 const LetterDistribution *ld,
                                 MachineLetter ml) {
  char *hl = ld_ml_to_hl(ld, ml);
  json_add_escaped_string(sb, hl);
  free(hl);
}

// Returns the premium-square code for a bonus square: one of
// "tws"/"dws"/"qws"/"tls"/"dls"/"qls"/"brk", or "" for a plain square.
static const char *json_bonus_code(BonusSquare bonus_square) {
  if (bonus_square_is_brick(bonus_square)) {
    return "brk";
  }
  const uint8_t word_mult = bonus_square_get_word_multiplier(bonus_square);
  const uint8_t letter_mult = bonus_square_get_letter_multiplier(bonus_square);
  if (word_mult == 3) {
    return "tws";
  }
  if (word_mult == 2) {
    return "dws";
  }
  if (word_mult == 4) {
    return "qws";
  }
  if (letter_mult == 3) {
    return "tls";
  }
  if (letter_mult == 2) {
    return "dls";
  }
  if (letter_mult == 4) {
    return "qls";
  }
  return "";
}

// Appends one board cell object: {"l":<letter or "">,"b":<bonus
// code>[,"k":true]}.
static void json_add_cell(StringBuilder *sb, const Board *board,
                          const LetterDistribution *ld, int row, int col) {
  // Read in natural (row, col) orientation without mutating the board: if it is
  // transposed, swap the physical coordinates instead of toggling transpose
  // (a getter must be side-effect-free and safe to call alongside the engine).
  int read_row = row;
  int read_col = col;
  if (board_get_transposed(board)) {
    read_row = col;
    read_col = row;
  }
  const MachineLetter ml = board_get_letter(board, read_row, read_col);
  const bool is_empty = (ml == ALPHABET_EMPTY_SQUARE_MARKER);
  const BonusSquare bonus_square =
      board_get_bonus_square(board, read_row, read_col);
  string_builder_add_string(sb, "{\"l\":");
  if (is_empty) {
    string_builder_add_string(sb, "\"\"");
  } else {
    json_add_tile_letter(sb, ld, ml);
  }
  string_builder_add_formatted_string(sb, ",\"b\":\"%s\"",
                                      json_bonus_code(bonus_square));
  if (!is_empty && get_is_blanked(ml)) {
    string_builder_add_string(sb, ",\"k\":true");
  }
  string_builder_add_char(sb, '}');
}

// Appends a player's rack tiles as a JSON array of {"l":<letter>,"k":<blank>}.
static void json_add_rack_tiles(StringBuilder *sb, const Rack *rack,
                                const LetterDistribution *ld) {
  string_builder_add_char(sb, '[');
  bool first = true;
  const int dist_size = rack_get_dist_size(rack);
  for (int ml = 1; ml < dist_size; ml++) {
    const int count = rack_get_letter(rack, (MachineLetter)ml);
    for (int copy = 0; copy < count; copy++) {
      if (!first) {
        string_builder_add_char(sb, ',');
      }
      first = false;
      string_builder_add_string(sb, "{\"l\":");
      json_add_tile_letter(sb, ld, (MachineLetter)ml);
      string_builder_add_string(sb, ",\"k\":false}");
    }
  }
  const int blanks = rack_get_letter(rack, BLANK_MACHINE_LETTER);
  for (int copy = 0; copy < blanks; copy++) {
    if (!first) {
      string_builder_add_char(sb, ',');
    }
    first = false;
    string_builder_add_string(sb, "{\"l\":");
    json_add_tile_letter(sb, ld, BLANK_MACHINE_LETTER);
    string_builder_add_string(sb, ",\"k\":true}");
  }
  string_builder_add_char(sb, ']');
}

// Appends the geometry fields shared by every move:
//   "type":"play"|"exch"|"pass","desc":"...","row":N,"col":N,
//   "vertical":bool,"placed":[{"row":N,"col":N,"l":"X"[,"k":true]}, ...]
// "placed" holds only the tiles laid from the rack (played-through squares are
// skipped). board may be NULL for moves with no placement (pass/exchange).
static void json_add_move_geometry(StringBuilder *sb, const Board *board,
                                   const LetterDistribution *ld,
                                   const Move *move) {
  const game_event_t type = move_get_type(move);
  const char *type_str = "play";
  if (type == GAME_EVENT_PASS) {
    type_str = "pass";
  } else if (type == GAME_EVENT_EXCHANGE) {
    type_str = "exch";
  }
  string_builder_add_formatted_string(sb,
                                      "\"type\":\"%s\",\"desc\":", type_str);
  StringBuilder *desc_sb = string_builder_create();
  string_builder_add_move_description(desc_sb, move, ld);
  char *desc = string_builder_dump(desc_sb, NULL);
  string_builder_destroy(desc_sb);
  json_add_escaped_string(sb, desc);
  free(desc);

  const int row = move_get_row_start(move);
  const int col = move_get_col_start(move);
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  string_builder_add_formatted_string(
      sb, ",\"row\":%d,\"col\":%d,\"vertical\":%s,\"placed\":[", row, col,
      vertical ? "true" : "false");
  if (type == GAME_EVENT_TILE_PLACEMENT_MOVE && board) {
    const int length = move_get_tiles_length(move);
    int cur_row = row;
    int cur_col = col;
    bool first = true;
    for (int tile_idx = 0; tile_idx < length; tile_idx++) {
      const MachineLetter ml = move_get_tile(move, tile_idx);
      if (ml != PLAYED_THROUGH_MARKER) {
        if (!first) {
          string_builder_add_char(sb, ',');
        }
        first = false;
        string_builder_add_formatted_string(
            sb, "{\"row\":%d,\"col\":%d,\"l\":", cur_row, cur_col);
        json_add_tile_letter(sb, ld, ml);
        if (get_is_blanked(ml)) {
          string_builder_add_string(sb, ",\"k\":true");
        }
        string_builder_add_char(sb, '}');
      }
      if (vertical) {
        cur_row++;
      } else {
        cur_col++;
      }
    }
  }
  string_builder_add_char(sb, ']');
}

char *json_api_get_state(const Config *config) {
  StringBuilder *sb = string_builder_create();
  const Game *game = config ? config_get_game(config) : NULL;
  if (!game) {
    string_builder_add_string(sb,
                              "{\"ok\":false,\"error\":\"no game loaded\"}");
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  // Read-only: json_add_cell handles a transposed board by swapping coordinates
  // rather than mutating it, so this getter never writes shared game state.
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  const BoardLayout *board_layout = config_get_board_layout(config);
  const char *lexicon = players_data_get_data_name(
      config_get_players_data(config), PLAYERS_DATA_TYPE_KWG, 0);

  string_builder_add_formatted_string(
      sb, "{\"ok\":true,\"dim\":%d,\"layout\":", BOARD_DIM);
  json_add_escaped_string(sb, board_layout ? board_layout_get_name(board_layout)
                                           : "");
  string_builder_add_string(sb, ",\"lexicon\":");
  json_add_escaped_string(sb, lexicon);
  string_builder_add_string(sb, ",\"ld\":");
  json_add_escaped_string(sb, ld_get_name(ld));
  string_builder_add_formatted_string(sb, ",\"onTurn\":%d,\"bagCount\":%d",
                                      game_get_player_on_turn_index(game),
                                      bag_get_letters(game_get_bag(game)));
  int center_row = BOARD_DIM / 2;
  int center_col = BOARD_DIM / 2;
  if (board_layout) {
    center_row = board_layout_get_start_coord(board_layout, 0);
    center_col = board_layout_get_start_coord(board_layout, 1);
  }
  string_builder_add_formatted_string(sb, ",\"center\":{\"row\":%d,\"col\":%d}",
                                      center_row, center_col);

  string_builder_add_string(sb, ",\"players\":[");
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const Player *player = game_get_player(game, player_idx);
    const Rack *rack = player_get_rack(player);
    if (player_idx) {
      string_builder_add_char(sb, ',');
    }
    string_builder_add_formatted_string(
        sb, "{\"score\":%d,\"rack\":", equity_to_int(player_get_score(player)));
    StringBuilder *rack_sb = string_builder_create();
    string_builder_add_rack(rack_sb, rack, ld, false);
    char *rack_str = string_builder_dump(rack_sb, NULL);
    string_builder_destroy(rack_sb);
    json_add_escaped_string(sb, rack_str);
    free(rack_str);
    string_builder_add_string(sb, ",\"tiles\":");
    json_add_rack_tiles(sb, rack, ld);
    string_builder_add_char(sb, '}');
  }
  string_builder_add_char(sb, ']');

  string_builder_add_string(sb, ",\"board\":[");
  for (int row = 0; row < BOARD_DIM; row++) {
    if (row) {
      string_builder_add_char(sb, ',');
    }
    string_builder_add_char(sb, '[');
    for (int col = 0; col < BOARD_DIM; col++) {
      if (col) {
        string_builder_add_char(sb, ',');
      }
      json_add_cell(sb, board, ld, row, col);
    }
    string_builder_add_char(sb, ']');
  }
  string_builder_add_char(sb, ']');

  char *cgp = game_get_cgp(game, true);
  string_builder_add_string(sb, ",\"cgp\":");
  json_add_escaped_string(sb, cgp);
  free(cgp);

  string_builder_add_char(sb, '}');
  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}

char *json_api_get_moves(const Config *config) {
  StringBuilder *sb = string_builder_create();
  const Game *game = config ? config_get_game(config) : NULL;
  const MoveList *move_list = config ? config_get_move_list(config) : NULL;
  const int total = move_list ? move_list_get_count(move_list) : 0;
  if (!game || total == 0) {
    string_builder_add_string(
        sb, "{\"ok\":true,\"hasMoves\":false,\"hasSim\":false,\"moves\":[]}");
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *gen_rack = move_list_get_rack(move_list);
  const int emit_count = total < JSON_MAX_MOVES ? total : JSON_MAX_MOVES;

  const SimResults *sim_results = config_get_sim_results(config);
  const bool has_sim =
      sim_results && sim_results_get_valid_for_current_game_state(sim_results);
  double win_pct[JSON_MAX_MOVES];
  double equity_mean[JSON_MAX_MOVES];
  bool simmed[JSON_MAX_MOVES];
  memset(simmed, 0, sizeof(simmed));
  uint64_t iterations = 0;
  if (has_sim) {
    iterations = sim_results_get_iteration_count(sim_results);
    const int num_plays = sim_results_get_number_of_plays(sim_results);
    for (int play_idx = 0; play_idx < num_plays; play_idx++) {
      const SimmedPlay *simmed_play =
          sim_results_get_simmed_play(sim_results, play_idx);
      const int move_idx = simmed_play_get_play_index_by_sort_type(simmed_play);
      if (move_idx >= 0 && move_idx < emit_count) {
        // win_pct_stat mean is a fraction in [0,1]; scale to a percentage to
        // match the engine's own displays (analyze.c, sim_string.c).
        win_pct[move_idx] =
            stat_get_mean(simmed_play_get_win_pct_stat(simmed_play)) * 100.0;
        equity_mean[move_idx] =
            stat_get_mean(simmed_play_get_equity_stat(simmed_play));
        simmed[move_idx] = true;
      }
    }
  }

  string_builder_add_formatted_string(
      sb,
      "{\"ok\":true,\"hasMoves\":true,\"hasSim\":%s,\"total\":%d,"
      "\"iterations\":%llu,\"moves\":[",
      has_sim ? "true" : "false", total, (unsigned long long)iterations);
  for (int move_idx = 0; move_idx < emit_count; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    const game_event_t type = move_get_type(move);
    if (move_idx) {
      string_builder_add_char(sb, ',');
    }
    string_builder_add_formatted_string(sb, "{\"i\":%d,", move_idx);
    json_add_move_geometry(sb, board, ld, move);
    const int score =
        (type == GAME_EVENT_PASS) ? 0 : equity_to_int(move_get_score(move));
    string_builder_add_formatted_string(sb, ",\"score\":%d,\"equity\":", score);
    if (type == GAME_EVENT_PASS) {
      string_builder_add_string(sb, "null");
    } else {
      string_builder_add_formatted_string(
          sb, "%.3f", equity_to_double(move_get_equity(move)));
    }
    string_builder_add_string(sb, ",\"leave\":");
    if (gen_rack) {
      StringBuilder *leave_sb = string_builder_create();
      string_builder_add_move_leave(leave_sb, gen_rack, move, ld);
      char *leave = string_builder_dump(leave_sb, NULL);
      string_builder_destroy(leave_sb);
      json_add_escaped_string(sb, leave);
      free(leave);
    } else {
      string_builder_add_string(sb, "\"\"");
    }
    if (simmed[move_idx]) {
      string_builder_add_formatted_string(sb, ",\"win\":%.4f,\"eqMean\":%.4f",
                                          win_pct[move_idx],
                                          equity_mean[move_idx]);
    }
    string_builder_add_char(sb, '}');
  }
  string_builder_add_string(sb, "]}");
  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}

char *json_api_get_endgame(const Config *config) {
  StringBuilder *sb = string_builder_create();
  EndgameResults *endgame_results =
      config ? config_get_endgame_results(config) : NULL;
  const Game *game = config ? config_get_game(config) : NULL;
  if (!game || !endgame_results ||
      !endgame_results_get_valid_for_current_game_state(endgame_results)) {
    string_builder_add_string(sb, "{\"ok\":true,\"valid\":false}");
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  const LetterDistribution *ld = game_get_ld(game);
  const int value =
      endgame_results_get_value(endgame_results, ENDGAME_RESULT_BEST);
  const int depth =
      endgame_results_get_depth(endgame_results, ENDGAME_RESULT_BEST);
  const double seconds = endgame_results_get_seconds_elapsed(endgame_results);
  const int num_pvs = endgame_results_get_num_pvs(endgame_results);

  string_builder_add_formatted_string(
      sb,
      "{\"ok\":true,\"valid\":true,\"value\":%d,\"depth\":%d,"
      "\"seconds\":%.3f,\"numPvs\":%d,\"best\":",
      value, depth, seconds, num_pvs);

  endgame_results_lock(endgame_results, ENDGAME_RESULT_BEST);
  const PVLine *pv_line =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
  if (pv_line && pv_line->num_moves > 0) {
    const Game *pv_game = pv_line->game ? pv_line->game : game;
    const Board *board = game_get_board(pv_game);
    Move best_move;
    small_move_to_move(&best_move, &pv_line->moves[0], board);
    string_builder_add_char(sb, '{');
    json_add_move_geometry(sb, board, ld, &best_move);
    string_builder_add_char(sb, '}');
  } else {
    string_builder_add_string(sb, "null");
  }
  endgame_results_unlock(endgame_results, ENDGAME_RESULT_BEST);

  string_builder_add_char(sb, '}');
  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}
