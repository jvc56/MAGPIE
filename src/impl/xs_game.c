#include "xs_game.h"

#include "../util/io_util.h"
#include "../util/string_util.h"
#include "../util/xs_bits.h"
#include "xs_position.h"
#include "xs_rank.h"
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { XS_GAME_MAX_EVENTS = 1000, XS_DRAW_CAPACITY = 3654 };

enum { XS_PLAY, XS_PASS, XS_EXCHANGE, XS_COUNT, XS_WITHDRAW, XS_OTHER };

// Wire event IDs must start at zero, independently of allocation limits.
static const unsigned xs_kinds[] = {XS_PLAY,  XS_PASS,     XS_EXCHANGE,
                                    XS_COUNT, XS_WITHDRAW, XS_OTHER};
static const char xs_alphabet[] = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const int xs_distribution_counts[27] = {2, 9, 2, 2, 4, 12, 2, 3, 2,
                                               9, 1, 1, 4, 2, 6,  8, 2, 1,
                                               6, 4, 6, 4, 2, 2,  1, 2, 1};

typedef struct XSEvent {
  unsigned player, kind;
  char rack[8];
  XSMove move;
  char *action;
  int32_t delta, total;
} XSEvent;

typedef struct XSGame {
  char language[64], lexicon[256];
  char *nick[2], *name[2];
  XSEvent *events;
  unsigned count;
  char final_racks[2][8];
  bool has_final_rack[2];
} XSGame;

typedef struct XSState {
  char board[225], before[225];
  int carry[2][27];
  int32_t scores[2];
  int last;
} XSState;

typedef struct XSDraw {
  uint32_t weight;
  char tiles[4];
} XSDraw;

static void xs_game_destroy(XSGame *game) {
  if (!game) {
    return;
  }
  for (int player = 0; player < 2; player++) {
    free(game->nick[player]);
    free(game->name[player]);
  }
  for (unsigned index = 0; index < game->count; index++) {
    free(game->events[index].action);
  }
  free(game->events);
  free(game);
}

static bool xs_line_text(const char *text, bool token) {
  if (!text || !*text || strlen(text) > XS_MAX_TEXT || !xs_utf8(text)) {
    return false;
  }
  for (const unsigned char *byte = (const unsigned char *)text; *byte; byte++) {
    if (*byte < 32 || *byte == 127 ||
        (token && (*byte == ' ' || *byte == ':'))) {
      return false;
    }
  }
  return true;
}

static bool xs_rack_counts(const char *rack, int counts[27]) {
  memset(counts, 0, sizeof(int) * 27);
  if (strlen(rack) > 7) {
    return false;
  }
  for (const char *letter = rack; *letter; letter++) {
    const char *found = strchr(xs_alphabet, *letter);
    if (!found) {
      return false;
    }
    counts[found - xs_alphabet]++;
  }
  return true;
}

static bool xs_rack_string(const int counts[27], char result[8]) {
  unsigned size = 0;
  for (int letter = 0; letter < 27; letter++) {
    if (counts[letter] < 0 || counts[letter] > 7 ||
        size + (unsigned)counts[letter] > 7) {
      return false;
    }
    for (int tile = 0; tile < counts[letter]; tile++) {
      result[size++] = xs_alphabet[letter];
    }
  }
  result[size] = '\0';
  return true;
}

static void xs_implied(const XSEvent *event, int result[27]) {
  memset(result, 0, sizeof(int) * 27);
  if (event->kind == XS_PLAY) {
    for (const char *letter = event->move.word; *letter; letter++) {
      if (*letter != '.') {
        result[*letter >= 'a' ? 0 : *letter - 'A' + 1]++;
      }
    }
  } else if (event->kind == XS_EXCHANGE) {
    xs_rack_counts(event->action, result);
  }
}

static bool xs_move_valid(const char board[225], XSMove *move) {
  size_t length = strlen(move->word);
  unsigned placed = 0;
  if (move->start >= 225 || move->down > 1 || !length || length > 15) {
    return false;
  }
  for (unsigned index = 0; index < length; index++) {
    unsigned square = move->start + (index * (move->down ? 15 : 1));
    if (square >= 225 || (!move->down && square / 15 != move->start / 15)) {
      return false;
    }
    char *letter = &move->word[index];
    if (board[square]) {
      if (*letter != '.' && xs_upper(*letter) != xs_upper(board[square])) {
        return false;
      }
      *letter = '.';
    } else {
      if (!xs_letter(*letter)) {
        return false;
      }
      placed++;
    }
  }
  return placed > 0 && placed <= 7;
}

static bool xs_apply(XSState *state, const XSEvent *event) {
  int rack[27];
  int implied[27];
  if (!xs_rack_counts(event->rack, rack)) {
    return false;
  }
  xs_implied(event, implied);
  for (int index = 0; index < 27; index++) {
    if (rack[index] < implied[index]) {
      return false;
    }
    rack[index] -= implied[index];
  }
  if (event->kind == XS_PLAY) {
    memcpy(state->before, state->board, 225);
    state->last = (int)event->player;
    for (unsigned index = 0; event->move.word[index]; index++) {
      if (event->move.word[index] != '.') {
        state
            ->board[event->move.start + (index * (event->move.down ? 15 : 1))] =
            event->move.word[index];
      }
    }
  } else if (event->kind == XS_COUNT) {
    memset(rack, 0, sizeof(rack));
  } else if (event->kind == XS_WITHDRAW) {
    if (state->last != (int)event->player) {
      return false;
    }
    memcpy(state->board, state->before, 225);
    state->last = -1;
  }
  memcpy(state->carry[event->player], rack, sizeof(rack));
  state->scores[event->player] = event->total;
  return true;
}

static bool xs_location(const char *text, XSMove *move) {
  size_t length = strlen(text);
  if (length < 2 || length > 3) {
    return false;
  }
  move->down = text[0] >= 'A' && text[0] <= 'O';
  char col = (char)(move->down ? text[0] : text[length - 1]);
  char digits[3] = {0};
  memcpy(digits, text + move->down, length - 1);
  int32_t row = 0;
  if (col < 'A' || col > 'O' || digits[0] < '1' || digits[0] > '9' ||
      !xs_int32(digits, &row) || row < 1 || row > 15) {
    return false;
  }
  move->start = (unsigned)(row - 1) * 15 + (unsigned)(col - 'A');
  return true;
}

static unsigned xs_action_kind(const StringSplitter *parts, int start,
                               int count) {
  const char *action = string_splitter_get_item(parts, start);
  XSMove location;
  if (count == 2 && xs_location(action, &location)) {
    return XS_PLAY;
  }
  if (count == 1) {
    int tiles[27];
    if (!strcmp(action, "-")) {
      return XS_PASS;
    }
    if (!strcmp(action, "--")) {
      return XS_WITHDRAW;
    }
    if (*action == '-' && action[1] && xs_rack_counts(action + 1, tiles)) {
      return XS_EXCHANGE;
    }
    if (*action == '-' && action[1] >= '1' && action[1] <= '7' && !action[2]) {
      return XS_COUNT;
    }
  }
  return XS_OTHER;
}

static bool xs_other_action(const char *text) {
  if (!xs_line_text(text, false)) {
    return false;
  }
  StringSplitter *parts = split_string_by_whitespace(text, true);
  int count = string_splitter_get_number_of_items(parts);
  bool valid = count > 0 && xs_action_kind(parts, 0, count) == XS_OTHER;
  int rack[27];
  if (count >= 2 && xs_rack_counts(string_splitter_get_item(parts, 0), rack)) {
    valid = false; // Otherwise an omitted rack would consume this token.
  }
  size_t normalized = count > 0 ? (size_t)count - 1 : 0;
  for (int index = 0; index < count; index++) {
    normalized += strlen(string_splitter_get_item(parts, index));
  }
  valid = valid && normalized == strlen(text);
  // xs_line_text already rejects tabs/newlines/control characters.
  string_splitter_destroy(parts);
  return valid;
}

static bool xs_parse_event(XSGame *game, XSState *state, const char *line) {
  const char *colon = strchr(line, ':');
  if (!colon || game->count == XS_GAME_MAX_EVENTS) {
    return false;
  }
  unsigned player = 2;
  for (unsigned index = 0; index < 2; index++) {
    if (game->nick[index] &&
        (size_t)(colon - line - 1) == strlen(game->nick[index]) &&
        !strncmp(line + 1, game->nick[index], (size_t)(colon - line - 1))) {
      player = index;
    }
  }
  if (player == 2) {
    return false;
  }
  StringSplitter *parts = split_string_by_whitespace(colon + 1, true);
  int count = string_splitter_get_number_of_items(parts);
  XSEvent *event = &game->events[game->count++];
  event->player = player;
  bool valid = count >= 3;
  if (valid) {
    valid =
        xs_int32(string_splitter_get_item(parts, count - 2), &event->delta) &&
        xs_int32(string_splitter_get_item(parts, count - 1), &event->total);
    int start = 0;
    int counts[27];
    const char *first = string_splitter_get_item(parts, 0);
    if (count >= 4 && xs_rack_counts(first, counts)) {
      xs_rack_string(counts, event->rack);
      start++;
    }
    const char *action = string_splitter_get_item(parts, start);
    int action_count = count - start - 2;
    event->kind = xs_action_kind(parts, start, action_count);
    if (event->kind == XS_PLAY) {
      xs_location(action, &event->move);
      const char *word = string_splitter_get_item(parts, start + 1);
      unsigned length = 0;
      for (const char *letter = word; *letter; letter++) {
        if (*letter == '(' || *letter == ')') {
          continue;
        }
        if (length == 15) {
          valid = false;
          break;
        }
        event->move.word[length++] = *letter;
      }
      valid = valid && xs_move_valid(state->board, &event->move);
    } else if (event->kind == XS_PASS || event->kind == XS_WITHDRAW) {
      // No action payload.
    } else if (event->kind == XS_EXCHANGE) {
      xs_rack_counts(action + 1, counts);
      event->kind = XS_EXCHANGE;
      char tiles[8];
      xs_rack_string(counts, tiles);
      event->action = string_duplicate(tiles);
    } else if (event->kind == XS_COUNT) {
      event->kind = XS_COUNT;
      event->action = string_duplicate(action + 1);
    } else if (action_count > 0) {
      event->kind = XS_OTHER;
      StringBuilder *builder = string_builder_create();
      for (int index = start; index < count - 2; index++) {
        if (index > start) {
          string_builder_add_char(builder, ' ');
        }
        string_builder_add_string(builder,
                                  string_splitter_get_item(parts, index));
      }
      event->action = string_builder_dump(builder, NULL);
      string_builder_destroy(builder);
      valid = valid && xs_other_action(event->action);
    } else {
      valid = false;
    }
    if (valid) {
      int implied[27];
      xs_implied(event, implied);
      xs_rack_counts(event->rack, counts);
      for (int index = 0; index < 27; index++) {
        if (implied[index] > counts[index]) {
          counts[index] = implied[index];
        }
      }
      valid = xs_rack_string(counts, event->rack) && xs_apply(state, event);
    }
  }
  string_splitter_destroy(parts);
  return valid;
}

static XSGame *xs_parse_game(const char *text) {
  if (strlen(text) > XS_MAX_BYTES || !xs_utf8(text)) {
    return NULL;
  }
  XSGame *game = calloc_or_die(1, sizeof(*game));
  memcpy(game->language, "en", strlen("en") + 1);
  game->events = calloc_or_die(XS_GAME_MAX_EVENTS, sizeof(*game->events));
  XSState state = {.last = -1};
  StringSplitter *lines = split_string_by_newline(text, true);
  bool valid = true;
  for (int index = 0;
       valid && index < string_splitter_get_number_of_items(lines); index++) {
    const char *line = string_splitter_get_item(lines, index);
    if (*line == '>') {
      valid = xs_parse_event(game, &state, line);
      continue;
    }
    StringSplitter *parts = split_string_by_whitespace(line, true);
    int count = string_splitter_get_number_of_items(parts);
    if (count) {
      const char *key = string_splitter_get_item(parts, 0);
      if (!strcmp(key, "#player1") || !strcmp(key, "#player2")) {
        unsigned player = key[7] - '1';
        valid = count >= 2 && !game->nick[player] && !game->count;
        if (valid) {
          game->nick[player] =
              string_duplicate(string_splitter_get_item(parts, 1));
          const char *name = line;
          for (int field = 0; field < 2; field++) {
            while (*name && !isspace((unsigned char)*name)) {
              name++;
            }
            while (isspace((unsigned char)*name)) {
              name++;
            }
          }
          game->name[player] =
              string_duplicate(*name ? name : game->nick[player]);
          valid = xs_line_text(game->nick[player], true) &&
                  xs_line_text(game->name[player], false);
        }
      } else if (!strcmp(key, "#lexicon") || !strcmp(key, "#language")) {
        char *target =
            !strcmp(key, "#lexicon") ? game->lexicon : game->language;
        size_t limit = target == game->lexicon ? sizeof(game->lexicon)
                                               : sizeof(game->language);
        valid = count == 2 && !game->count;
        if (valid) {
          const char *value = string_splitter_get_item(parts, 1);
          valid = strlen(value) < limit && xs_line_text(value, true);
          if (valid) {
            memcpy(target, value, strlen(value) + 1);
          }
        }
      } else if (!strcmp(key, "#rack1") || !strcmp(key, "#rack2")) {
        int counts[27];
        valid = count == 2 &&
                xs_rack_counts(string_splitter_get_item(parts, 1), counts);
        if (valid) {
          unsigned player = key[5] - '1';
          xs_rack_string(counts, game->final_racks[player]);
          game->has_final_rack[player] = true;
        }
      } else if (!strcmp(key, "#game-type")) {
        valid = count == 2 &&
                !strcmp(string_splitter_get_item(parts, 1), "classic");
      } else if (!strcmp(key, "#board-layout")) {
        valid = count == 2 &&
                (!strcmp(string_splitter_get_item(parts, 1), "standard15") ||
                 !strcmp(string_splitter_get_item(parts, 1), "CrosswordGame"));
      } else if (!strcmp(key, "#tile-distribution")) {
        valid = count == 2 &&
                !strcmp(string_splitter_get_item(parts, 1), "english");
      }
    }
    string_splitter_destroy(parts);
  }
  string_splitter_destroy(lines);
  valid = valid && game->nick[0] && game->nick[1] &&
          strcmp(game->nick[0], game->nick[1]) != 0 &&
          !strcmp(game->language, "en");
  if (!valid) {
    xs_game_destroy(game);
    return NULL;
  }
  return game;
}

static char *xs_game_gcg(const XSGame *game) {
  StringBuilder *out = string_builder_create();
  string_builder_add_string(out, "#character-encoding UTF-8\n");
  for (int player = 0; player < 2; player++) {
    string_builder_add_formatted_string(out, "#player%d %s %s\n", player + 1,
                                        game->nick[player], game->name[player]);
  }
  // Standard GCG has no #language header. English is implicit for this codec.
  if (*game->lexicon) {
    string_builder_add_formatted_string(out, "#lexicon %s\n", game->lexicon);
  }
  for (unsigned index = 0; index < game->count; index++) {
    const XSEvent *event = &game->events[index];
    string_builder_add_formatted_string(out, ">%s: %s ",
                                        game->nick[event->player], event->rack);
    if (event->kind == XS_PLAY) {
      if (event->move.down) {
        string_builder_add_formatted_string(out, "%c%u",
                                            'A' + (event->move.start % 15),
                                            (event->move.start / 15) + 1);
      } else {
        string_builder_add_formatted_string(out, "%u%c",
                                            (event->move.start / 15) + 1,
                                            'A' + (event->move.start % 15));
      }
      string_builder_add_formatted_string(out, " %s", event->move.word);
    } else if (event->kind == XS_PASS) {
      string_builder_add_char(out, '-');
    } else if (event->kind == XS_WITHDRAW) {
      string_builder_add_string(out, "--");
    } else {
      if (event->kind == XS_EXCHANGE || event->kind == XS_COUNT) {
        string_builder_add_char(out, '-');
      }
      string_builder_add_string(out, event->action);
    }
    string_builder_add_formatted_string(out, " %+d %d\n", event->delta,
                                        event->total);
  }
  for (int player = 0; player < 2; player++) {
    if (game->has_final_rack[player]) {
      string_builder_add_formatted_string(out, "#rack%d %s\n", player + 1,
                                          game->final_racks[player]);
    }
  }
  char *result = string_builder_dump(out, NULL);
  string_builder_destroy(out);
  return result;
}

static int xs_draw_order(const void *left, const void *right) {
  const XSDraw *first = left;
  const XSDraw *second = right;
  if (first->weight != second->weight) {
    return first->weight > second->weight ? -1 : 1;
  }
  return strcmp(first->tiles, second->tiles);
}

static unsigned xs_draws(const XSState *state, const int base[27],
                         unsigned count, XSDraw *draws) {
  if (count < 1 || count > 3) {
    return 0;
  }
  int pool[27];
  memcpy(pool, xs_distribution_counts, sizeof(pool));
  for (int square = 0; square < 225; square++) {
    char letter = state->board[square];
    if (letter) {
      pool[letter >= 'a' ? 0 : letter - 'A' + 1]--;
    }
  }
  for (int letter = 0; letter < 27; letter++) {
    pool[letter] -= base[letter];
    if (pool[letter] < 0) {
      pool[letter] = 0;
    }
  }
  unsigned size = 0;
  for (int first = 0; first < 27; first++) {
    for (int second = count > 1 ? first : 26; second < 27; second++) {
      for (int third = count > 2 ? second : 26; third < 27; third++) {
        int counts[27] = {0};
        counts[first]++;
        if (count > 1) {
          counts[second]++;
        }
        if (count > 2) {
          counts[third]++;
        }
        uint32_t weight = 1;
        for (int letter = 0; letter < 27; letter++) {
          uint32_t ways = 1;
          for (int tile = 1; tile <= counts[letter]; tile++) {
            ways =
                ways *
                (uint32_t)(pool[letter] >= tile ? pool[letter] - tile + 1 : 0) /
                (unsigned)tile;
          }
          weight *= ways;
        }
        if (weight && size < XS_DRAW_CAPACITY) {
          XSDraw *draw = &draws[size++];
          memset(draw, 0, sizeof(*draw));
          draw->weight = weight;
          draw->tiles[0] = xs_alphabet[first];
          if (count > 1) {
            draw->tiles[1] = xs_alphabet[second];
          }
          if (count > 2) {
            draw->tiles[2] = xs_alphabet[third];
          }
        }
      }
    }
  }
  qsort(draws, size, sizeof(*draws), xs_draw_order);
  return size;
}

static unsigned xs_rank_bits(unsigned rank, unsigned order) {
  unsigned prefix = (rank >> order) + 1;
  unsigned width = 0;
  while (prefix) {
    width++;
    prefix >>= 1;
  }
  return (width * 2) - 1 + order;
}

static void xs_rack_base(const XSState *state, unsigned player,
                         const int implied[27], bool reset, int base[27]) {
  for (int letter = 0; letter < 27; letter++) {
    base[letter] = reset ? 0 : state->carry[player][letter];
    if (implied[letter] > base[letter]) {
      base[letter] = implied[letter];
    }
  }
}

static void xs_write_rack(XSBuffer *out, const XSState *state,
                          const XSEvent *event, const int implied[27],
                          XSGameStats *stats) {
  int base[27];
  int rack[27];
  int extra[27];
  xs_rack_counts(event->rack, rack);
  xs_rack_base(state, event->player, implied, false, base);
  bool reset = false;
  for (int letter = 0; letter < 27; letter++) {
    if (base[letter] > rack[letter]) {
      reset = true;
    }
  }
  xs_rack_base(state, event->player, implied, reset, base);
  for (int letter = 0; letter < 27; letter++) {
    extra[letter] = rack[letter] - base[letter];
  }
  char tiles[8];
  if (!xs_rack_string(extra, tiles)) {
    out->valid = false;
    return;
  }
  unsigned count = (unsigned)strlen(tiles);
  xs_put(out, reset, 1);
  xs_put(out, count, 3);
  if (!count) {
    return;
  }
  XSDraw *draws = calloc_or_die(XS_DRAW_CAPACITY, sizeof(*draws));
  unsigned size = xs_draws(state, base, count, draws);
  unsigned rank = size;
  for (unsigned index = 0; index < size; index++) {
    if (!strcmp(draws[index].tiles, tiles)) {
      rank = index;
      break;
    }
  }
  bool ranked = rank < size && xs_rank_bits(rank, 4) < count * 5;
  xs_put(out, ranked, 1);
  if (ranked) {
    xs_put_rank(out, 4, rank);
    if (stats) {
      stats->ranked_draws++;
    }
  } else {
    for (unsigned index = 0; index < count; index++) {
      xs_put(out, (uint32_t)(strchr(xs_alphabet, tiles[index]) - xs_alphabet),
             5);
    }
  }
  free(draws);
}

static void xs_read_rack(XSBuffer *in, const XSState *state, XSEvent *event,
                         const int implied[27]) {
  int base[27];
  bool reset = xs_get(in, 1);
  xs_rack_base(state, event->player, implied, reset, base);
  unsigned count = xs_get(in, 3);
  char extra[8] = {0};
  if (count) {
    if (xs_get(in, 1)) {
      XSDraw *draws = calloc_or_die(XS_DRAW_CAPACITY, sizeof(*draws));
      unsigned size = xs_draws(state, base, count, draws);
      unsigned rank = xs_get_rank(in, 4);
      if (rank >= size) {
        in->valid = false;
      } else {
        memcpy(extra, draws[rank].tiles, strlen(draws[rank].tiles) + 1);
      }
      free(draws);
    } else {
      for (unsigned index = 0; index < count; index++) {
        unsigned letter = xs_get(in, 5);
        if (letter > 26) {
          in->valid = false;
        } else {
          extra[index] = xs_alphabet[letter];
        }
      }
    }
  }
  for (unsigned index = 0; index < count && in->valid; index++) {
    if (!extra[index] || (index && extra[index] < extra[index - 1])) {
      in->valid = false;
      break;
    }
    base[strchr(xs_alphabet, extra[index]) - xs_alphabet]++;
  }
  if (!xs_rack_string(base, event->rack)) {
    in->valid = false;
  }
}

static void xs_write_move(XSBuffer *out, const XSState *state,
                          const XSMove *move) {
  xs_put(out, move->start, 8);
  xs_put(out, move->down, 1);
  xs_put(out, (unsigned)strlen(move->word) - 1, 4);
  for (unsigned index = 0; move->word[index]; index++) {
    if (!state->board[move->start + (index * (move->down ? 15 : 1))]) {
      char letter = move->word[index];
      xs_put(out, (unsigned)(xs_upper(letter) - 'A') + (letter >= 'a' ? 26 : 0),
             6);
    }
  }
}

static void xs_read_move(XSBuffer *in, const XSState *state, XSMove *move) {
  move->start = xs_get(in, 8);
  move->down = xs_get(in, 1);
  unsigned length = xs_get(in, 4) + 1;
  if (length > 15 || move->start >= 225) {
    in->valid = false;
    return;
  }
  for (unsigned index = 0; index < length && in->valid; index++) {
    unsigned square = move->start + (index * (move->down ? 15 : 1));
    if (square >= 225 || (!move->down && square / 15 != move->start / 15)) {
      in->valid = false;
      break;
    }
    if (state->board[square]) {
      move->word[index] = '.';
    } else {
      unsigned letter = xs_get(in, 6);
      if (letter >= 52) {
        in->valid = false;
      } else {
        move->word[index] = (char)((letter >= 26 ? 'a' : 'A') + (letter % 26));
      }
    }
  }
  if (in->valid && !xs_move_valid(state->board, move)) {
    in->valid = false;
  }
}

static void xs_write_event(XSBuffer *out, XSState *state, const XSEvent *event,
                           XSRanker *ranker, XSGameStats *stats,
                           ErrorStack *errors) {
  unsigned wire = 0;
  while (wire < 6 && xs_kinds[wire] != event->kind) {
    wire++;
  }
  xs_put(out, event->player, 1);
  xs_put(out, wire, 3);
  int implied[27];
  xs_implied(event, implied);
  if (event->kind == XS_PLAY) {
    XSBuffer explicit_bits;
    XSBuffer ranked_bits;
    xs_buffer_init(&explicit_bits);
    xs_buffer_init(&ranked_bits);
    XSGameStats explicit_stats = {0};
    XSGameStats ranked_stats = {0};
    xs_write_move(&explicit_bits, state, &event->move);
    xs_write_rack(&explicit_bits, state, event, implied, &explicit_stats);
    bool ranked = false;
    if (ranker) {
      int count = 0;
      XSMove *moves =
          xs_rank_moves(ranker, state->board, event->rack, &count, errors);
      for (int index = 0; index < count; index++) {
        if (moves[index].start == event->move.start &&
            moves[index].down == event->move.down &&
            !strcmp(moves[index].word, event->move.word)) {
          int empty[27] = {0};
          xs_write_rack(&ranked_bits, state, event, empty, &ranked_stats);
          xs_put_rank(&ranked_bits, 2, (unsigned)index);
          ranked = ranked_bits.valid && ranked_bits.bits < explicit_bits.bits;
          break;
        }
      }
      free(moves);
    }
    xs_put(out, ranked, 1);
    xs_append(out, ranked ? &ranked_bits : &explicit_bits);
    if (stats) {
      if (ranked) {
        stats->ranked++;
      } else {
        stats->explicit_moves++;
      }
      stats->ranked_draws +=
          ranked ? ranked_stats.ranked_draws : explicit_stats.ranked_draws;
    }
    xs_buffer_destroy(&explicit_bits);
    xs_buffer_destroy(&ranked_bits);
  } else {
    if (event->kind == XS_EXCHANGE) {
      xs_put(out, (unsigned)strlen(event->action), 3);
      for (const char *letter = event->action; *letter; letter++) {
        xs_put(out, (uint32_t)(strchr(xs_alphabet, *letter) - xs_alphabet), 5);
      }
    } else if (event->kind == XS_COUNT) {
      xs_put(out, (unsigned)(event->action[0] - '0'), 3);
    } else if (event->kind == XS_OTHER) {
      xs_put_text(out, event->action);
    }
    xs_write_rack(out, state, event, implied, stats);
  }
  xs_put_signed(out, event->delta);
  int64_t expected = (int64_t)state->scores[event->player] + event->delta;
  xs_put(out, expected != event->total, 1);
  if (expected != event->total) {
    xs_put_signed(out, event->total);
  }
  if (!xs_apply(state, event)) {
    out->valid = false;
  }
}

char *xs_game_encode(const char *gcg, const char *paths, unsigned profile,
                     XSGameStats *stats, ErrorStack *errors) {
  if (stats) {
    memset(stats, 0, sizeof(*stats));
  }
  XSGame *game = xs_parse_game(gcg);
  if (!game || profile > 2) {
    xs_game_destroy(game);
    xs_error(errors, "invalid or unsupported GCG");
    return NULL;
  }
  XSRanker *ranker = profile ? xs_ranker_create(paths, profile, errors) : NULL;
  if (profile && !ranker) {
    xs_game_destroy(game);
    return NULL;
  }
  XSBuffer out;
  xs_buffer_init(&out);
  xs_put(&out, 2, 4);
  xs_write_metadata(&out, game->language, game->lexicon);
  xs_put(&out, profile, 2);
  for (int player = 0; player < 2; player++) {
    xs_put_text(&out, game->nick[player]);
    bool same = !strcmp(game->nick[player], game->name[player]);
    xs_put(&out, same, 1);
    if (!same) {
      xs_put_text(&out, game->name[player]);
    }
  }
  xs_put_var(&out, game->count);
  XSState state = {.last = -1};
  for (unsigned index = 0;
       index < game->count && out.valid && error_stack_is_empty(errors);
       index++) {
    xs_write_event(&out, &state, &game->events[index], ranker, stats, errors);
  }
  for (int player = 0; player < 2; player++) {
    xs_put(&out, game->has_final_rack[player], 1);
    if (game->has_final_rack[player]) {
      xs_put_text(&out, game->final_racks[player]);
    }
  }
  char *result =
      out.valid && error_stack_is_empty(errors) ? xs_token(&out, "XSG") : NULL;
  xs_buffer_destroy(&out);
  xs_game_destroy(game);
  xs_ranker_destroy(ranker);
  if (!result && error_stack_is_empty(errors)) {
    xs_error(errors, "cannot encode XSG");
  }
  return result;
}

static void xs_read_event(XSBuffer *in, XSState *state, XSEvent *event,
                          XSRanker *ranker, ErrorStack *errors) {
  event->player = xs_get(in, 1);
  unsigned wire = xs_get(in, 3);
  if (wire >= 6) {
    in->valid = false;
    return;
  }
  event->kind = xs_kinds[wire];
  bool read_rack = true;
  int implied[27] = {0};
  if (event->kind == XS_PLAY) {
    if (xs_get(in, 1)) {
      if (!ranker) {
        in->valid = false;
        return;
      }
      xs_read_rack(in, state, event, implied);
      read_rack = false;
      unsigned rank = xs_get_rank(in, 2);
      int count = 0;
      if (!in->valid || rank >= 1024) {
        in->valid = false;
        return;
      }
      XSMove *moves =
          xs_rank_moves(ranker, state->board, event->rack, &count, errors);
      if (rank >= (unsigned)count) {
        in->valid = false;
      } else {
        event->move = moves[rank];
      }
      free(moves);
    } else {
      xs_read_move(in, state, &event->move);
      if (in->valid) {
        xs_implied(event, implied);
      }
    }
  } else if (event->kind == XS_EXCHANGE) {
    unsigned count = xs_get(in, 3);
    event->action = calloc_or_die(8, 1);
    if (!count) {
      in->valid = false;
    }
    for (unsigned index = 0; index < count; index++) {
      unsigned letter = xs_get(in, 5);
      if (letter > 26 ||
          (index && xs_alphabet[letter] < event->action[index - 1])) {
        in->valid = false;
        break;
      }
      event->action[index] = xs_alphabet[letter];
      implied[letter]++;
    }
  } else if (event->kind == XS_COUNT) {
    unsigned count = xs_get(in, 3);
    event->action = calloc_or_die(2, 1);
    event->action[0] = (char)('0' + count);
    if (!count) {
      in->valid = false;
    }
  } else if (event->kind == XS_OTHER) {
    event->action = xs_get_text(in);
    if (!xs_other_action(event->action)) {
      in->valid = false;
    }
  }
  if (!in->valid) {
    return;
  }
  if (read_rack) {
    xs_read_rack(in, state, event, implied);
  }
  event->delta = xs_get_signed(in);
  int64_t total = (int64_t)state->scores[event->player] + event->delta;
  if (xs_get(in, 1)) {
    total = xs_get_signed(in);
  }
  if (total < INT32_MIN || total > INT32_MAX) {
    in->valid = false;
    return;
  }
  event->total = (int32_t)total;
  if (in->valid && !xs_apply(state, event)) {
    in->valid = false;
  }
}

char *xs_game_decode(const char *token, const char *paths, ErrorStack *errors) {
  XSBuffer in;
  xs_buffer_init(&in);
  if (!xs_untoken(&in, token, "XSG") || xs_get(&in, 4) != 2) {
    xs_buffer_destroy(&in);
    xs_error(errors, "damaged XSG or unsupported version");
    return NULL;
  }
  XSGame *game = calloc_or_die(1, sizeof(*game));
  xs_read_metadata(&in, game->language, game->lexicon);
  unsigned profile = xs_get(&in, 2);
  if (profile > 2 || strcmp(game->language, "en") != 0) {
    in.valid = false;
  }
  XSRanker *ranker =
      in.valid && profile ? xs_ranker_create(paths, profile, errors) : NULL;
  if (profile && !ranker) {
    in.valid = false;
  }
  for (int player = 0; player < 2 && in.valid; player++) {
    game->nick[player] = xs_get_text(&in);
    bool same = xs_get(&in, 1);
    game->name[player] =
        same ? string_duplicate(game->nick[player]) : xs_get_text(&in);
    if (!xs_line_text(game->nick[player], true) ||
        !xs_line_text(game->name[player], false)) {
      in.valid = false;
    }
  }
  if (in.valid && !strcmp(game->nick[0], game->nick[1])) {
    in.valid = false;
  }
  unsigned count = xs_get_var(&in);
  if (count > XS_GAME_MAX_EVENTS) {
    in.valid = false;
  }
  game->events =
      calloc_or_die(in.valid && count ? count : 1, sizeof(*game->events));
  XSState state = {.last = -1};
  for (unsigned index = 0;
       index < count && in.valid && error_stack_is_empty(errors); index++) {
    XSEvent *event = &game->events[game->count++];
    xs_read_event(&in, &state, event, ranker, errors);
  }
  for (int player = 0; player < 2 && in.valid; player++) {
    game->has_final_rack[player] = xs_get(&in, 1);
    if (game->has_final_rack[player]) {
      char *rack = xs_get_text(&in);
      int counts[27];
      if (!xs_rack_counts(rack, counts) || !*rack) {
        in.valid = false;
      } else {
        xs_rack_string(counts, game->final_racks[player]);
      }
      free(rack);
    }
  }
  char *result =
      xs_finish(&in) && error_stack_is_empty(errors) ? xs_game_gcg(game) : NULL;
  xs_buffer_destroy(&in);
  xs_game_destroy(game);
  xs_ranker_destroy(ranker);
  if (!result && error_stack_is_empty(errors)) {
    xs_error(errors, "invalid XSG fields or padding");
  }
  return result;
}
