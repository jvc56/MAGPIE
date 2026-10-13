#include "xs_position.h"

#include "../compat/ctime.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "../util/xs_bits.h"
#include "xs_codebook.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { XS_SQUARES = 225, XS_CANDIDATES = 450, XS_POSITION_VERSION = 5 };
static const char *const xs_languages[] = {"",   "en", "ca", "nl",
                                           "fr", "de", "pl"};
static const char *const xs_lexica[7][16] = {
    {NULL},
    {"CSW24", "NWL23", "CSW07", "CSW12", "CSW15", "CSW21", "NWL20", "OSW1",
     "OSW2", "OSW3", "OSW4", "OSWI", "TWL06", "TWL14", "TWL98", NULL},
    {"DISC2", NULL},
    {"DSW25", NULL},
    {"FRA20", NULL},
    {"RD29", NULL},
    {"OSPS49", NULL}};
bool xs_letter(char letter) {
  return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
}
char xs_upper(char letter) {
  return (char)(letter >= 'a' && letter <= 'z' ? letter - 'a' + 'A' : letter);
}
static void xs_meta_literal(XSBuffer *bits, const char *text, size_t limit) {
  size_t length = strlen(text);
  if (!length || length > limit || !xs_utf8(text)) {
    bits->valid = false;
    return;
  }
  xs_put(bits, (uint32_t)length, 8);
  for (size_t index = 0; index < length; index++) {
    xs_put(bits, (unsigned char)text[index], 8);
  }
}
static void xs_meta_read(XSBuffer *bits, char *text, unsigned limit) {
  unsigned length = xs_get(bits, 8);
  if (!length || length > limit) {
    bits->valid = false;
    text[0] = '\0';
    return;
  }
  for (unsigned index = 0; index < length; index++) {
    text[index] = (char)xs_get(bits, 8);
    if ((unsigned char)text[index] <= 32) {
      bits->valid = false;
    }
  }
  text[length] = '\0';
  if (!xs_utf8(text)) {
    bits->valid = false;
  }
}
void xs_write_metadata(XSBuffer *bits, const char *language,
                       const char *lexicon) {
  unsigned lang = 0;
  unsigned lex = 0;
  for (unsigned index = 1; index < 7; index++) {
    if (strings_equal(language, xs_languages[index])) {
      lang = index;
    }
  }
  if (lang) {
    for (unsigned index = 0; xs_lexica[lang][index]; index++) {
      if (strings_equal(lexicon, xs_lexica[lang][index])) {
        lex = index + 1;
      }
    }
  }
  xs_put(bits, lang, 4);
  if (!lang) {
    xs_meta_literal(bits, language, 63);
  }
  xs_put(bits, lex, 5);
  if (!lex) {
    xs_meta_literal(bits, lexicon, 255);
  }
}
void xs_read_metadata(XSBuffer *bits, char language[64], char lexicon[256]) {
  unsigned lang = xs_get(bits, 4);
  language[0] = '\0';
  lexicon[0] = '\0';
  if (lang >= 7) {
    bits->valid = false;
    return;
  }
  if (lang) {
    strcpy(language, xs_languages[lang]);
  } else {
    xs_meta_read(bits, language, 63);
  }
  unsigned lex = xs_get(bits, 5);
  if (lex) {
    if (!lang) {
      for (unsigned index = 1; index < 7; index++) {
        if (strings_equal(language, xs_languages[index])) {
          lang = index;
        }
      }
    }
    if (lex > 15 || !xs_lexica[lang][lex - 1]) {
      bits->valid = false;
      return;
    }
    strcpy(lexicon, xs_lexica[lang][lex - 1]);
  } else {
    xs_meta_read(bits, lexicon, 255);
  }
}
static bool xs_inventory(const XSPosition *position) {
  static const unsigned available[27] = {2, 9, 2, 2, 4, 12, 2, 3, 2,
                                         9, 1, 1, 4, 2, 6,  8, 2, 1,
                                         6, 4, 6, 4, 2, 2,  1, 2, 1};
  unsigned counts[27] = {0};
  for (int square = 0; square < 225; square++) {
    char tile = position->board[square];
    if (tile) {
      if (!xs_letter(tile)) {
        return false;
      }
      counts[tile >= 'a' && tile <= 'z' ? 0 : tile - 'A' + 1]++;
    }
  }
  for (int player = 0; player < 2; player++) {
    for (const char *tile = position->racks[player]; *tile; tile++) {
      if (*tile != '?' && (*tile < 'A' || *tile > 'Z')) {
        return false;
      }
      counts[*tile == '?' ? 0 : *tile - 'A' + 1]++;
    }
  }
  for (int tile = 0; tile < 27; tile++) {
    if (counts[tile] > available[tile]) {
      return false;
    }
  }
  return position->zeros <= 7 && strings_equal(position->language, "en");
}
bool xs_parse_cgp(const char *text, XSPosition *position) {
  *position = (XSPosition){0};
  strcpy(position->language, "en");
  strcpy(position->lexicon, "CSW24");
  StringSplitter *parts = split_string_by_whitespace(text, true);
  int count = string_splitter_get_number_of_items(parts);
  bool valid = count >= 4 && count % 2 == 0;
  if (valid) {
    StringSplitter *rows =
        split_string(string_splitter_get_item(parts, 0), '/', false);
    valid = string_splitter_get_number_of_items(rows) == 15;
    for (int row = 0; valid && row < 15; row++) {
      unsigned col = 0;
      const char *cursor = string_splitter_get_item(rows, row);
      while (*cursor && valid) {
        if (*cursor >= '0' && *cursor <= '9') {
          unsigned empty = 0;
          while (*cursor >= '0' && *cursor <= '9' && empty <= 15) {
            empty = empty * 10 + (unsigned)(*cursor++ - '0');
          }
          valid = empty > 0 && col + empty <= 15;
          col += empty;
        } else if (xs_letter(*cursor) && col < 15) {
          position->board[(row * 15) + col++] = *cursor++;
        } else {
          valid = false;
        }
      }
      valid = valid && col == 15;
    }
    string_splitter_destroy(rows);
    StringSplitter *racks =
        split_string(string_splitter_get_item(parts, 1), '/', false);
    StringSplitter *scores =
        split_string(string_splitter_get_item(parts, 2), '/', false);
    valid = valid && string_splitter_get_number_of_items(racks) == 2 &&
            string_splitter_get_number_of_items(scores) == 2;
    for (int player = 0; valid && player < 2; player++) {
      const char *rack = string_splitter_get_item(racks, player);
      valid = strlen(rack) <= 7 &&
              xs_int32(string_splitter_get_item(scores, player),
                       &position->scores[player]);
      if (valid) {
        strcpy(position->racks[player], rack);
      }
    }
    int32_t zeros = 0;
    valid = valid && xs_int32(string_splitter_get_item(parts, 3), &zeros) &&
            zeros >= 0 && zeros <= 7;
    position->zeros = (unsigned)zeros;
    string_splitter_destroy(racks);
    string_splitter_destroy(scores);
    for (int index = 4; valid && index < count; index += 2) {
      const char *option = string_splitter_get_item(parts, index);
      const char *value = string_splitter_get_item(parts, index + 1);
      if (strings_equal(option, "-lex") &&
          strlen(value) < sizeof(position->lexicon)) {
        strcpy(position->lexicon, value);
      } else if (strings_equal(option, "-lang") &&
                 strlen(value) < sizeof(position->language)) {
        strcpy(position->language, value);
      } else {
        valid = false;
      }
    }
  }
  string_splitter_destroy(parts);
  return valid && xs_inventory(position);
}
char *xs_format_cgp(const XSPosition *position) {
  StringBuilder *text = string_builder_create();
  for (int row = 0; row < 15; row++) {
    if (row) {
      string_builder_add_char(text, '/');
    }
    int empty = 0;
    for (int col = 0; col < 15; col++) {
      char tile = position->board[(row * 15) + col];
      if (!tile) {
        empty++;
      } else {
        if (empty) {
          string_builder_add_int(text, empty);
          empty = 0;
        }
        string_builder_add_char(text, tile);
      }
    }
    if (empty) {
      string_builder_add_int(text, empty);
    }
  }
  string_builder_add_formatted_string(text, " %s/%s %d/%d %u -lex %s",
                                      position->racks[0], position->racks[1],
                                      position->scores[0], position->scores[1],
                                      position->zeros, position->lexicon);
  return string_builder_dump_and_destroy(text, NULL);
}
typedef struct XSCandidate {
  int start;
  int step;
  const XSWord *word;
} XSCandidate;
typedef struct XSCover {
  XSCandidate candidates[XS_CANDIDATES];
  int count;
  bool best[XS_CANDIDATES];
  int best_cost;
  int64_t deadline;
  unsigned visits;
} XSCover;
static int xs_cover_gain(const XSCandidate *candidate, const bool *remaining) {
  int count = 0;
  for (size_t index = 0; index < strlen(candidate->word->word); index++) {
    count += remaining[candidate->start + ((int)index * candidate->step)];
  }
  return (count * 14) - (int)candidate->word->length - 10;
}
static void xs_cover_search(XSCover *cover, const bool *remaining,
                            bool *selected, bool *excluded, int cost) {
  if ((cover->visits++ & 63) == 0 && ctimer_monotonic_ns() >= cover->deadline) {
    return;
  }
  if (cost < cover->best_cost) {
    cover->best_cost = cost;
    memcpy(cover->best, selected, sizeof(cover->best));
  }
  int choice = -1;
  int gain = 0;
  int bound = 0;
  for (int index = 0; index < cover->count; index++) {
    if (!excluded[index] && !selected[index]) {
      int saving = xs_cover_gain(&cover->candidates[index], remaining);
      if (saving > 0) {
        bound += saving;
        if (saving > gain) {
          gain = saving;
          choice = index;
        }
      }
    }
  }
  if (choice < 0 || cost - bound >= cover->best_cost) {
    return;
  }
  bool next[225];
  memcpy(next, remaining, 225 * sizeof(*next));
  const XSCandidate *candidate = &cover->candidates[choice];
  for (size_t index = 0; index < strlen(candidate->word->word); index++) {
    next[candidate->start + ((int)index * candidate->step)] = false;
  }
  selected[choice] = true;
  xs_cover_search(cover, next, selected, excluded, cost - gain);
  selected[choice] = false;
  if (ctimer_monotonic_ns() < cover->deadline) {
    excluded[choice] = true;
    xs_cover_search(cover, remaining, selected, excluded, cost);
    excluded[choice] = false;
  }
}
static void xs_word_board(const XSPosition *position, const XSCodebook *book,
                          unsigned search_ms, XSBuffer *out) {
  XSCover *cover = calloc_or_die(1, sizeof(*cover));
  bool remaining[225], selected[XS_CANDIDATES] = {false},
                       excluded[XS_CANDIDATES] = {false};
  int occupied = 0;
  for (int square = 0; square < 225; square++) {
    remaining[square] = position->board[square] != 0;
    occupied += remaining[square];
  }
  for (int orientation = 0; orientation < 2; orientation++) {
    int step = orientation ? 15 : 1;
    for (int start = 0; start < 225; start++) {
      if (!remaining[start] || ((orientation ? start >= 15 : start % 15 > 0) &&
                                remaining[start - step])) {
        continue;
      }
      char word[16];
      int length = 0;
      for (int square = start; square < 225 && remaining[square] &&
                               (orientation || square / 15 == start / 15);
           square += step) {
        word[length++] = xs_upper(position->board[square]);
      }
      word[length] = '\0';
      const XSWord *entry = xs_codebook_find(book, word);
      if (entry) {
        cover->candidates[cover->count++] =
            (XSCandidate){.start = start, .step = step, .word = entry};
      }
    }
  }
  cover->best_cost = occupied * 14;
  while (true) {
    int choice = -1;
    int gain = 0;
    for (int index = 0; index < cover->count; index++) {
      int saving = xs_cover_gain(&cover->candidates[index], remaining);
      if (saving > gain) {
        gain = saving;
        choice = index;
      }
    }
    if (choice < 0) {
      break;
    }
    cover->best[choice] = true;
    cover->best_cost -= gain;
    const XSCandidate *candidate = &cover->candidates[choice];
    for (size_t index = 0; index < strlen(candidate->word->word); index++) {
      remaining[candidate->start + ((int)index * candidate->step)] = false;
    }
  }
  for (int square = 0; square < 225; square++) {
    remaining[square] = position->board[square] != 0;
  }
  if (search_ms) {
    cover->deadline = ctimer_monotonic_ns() + (int64_t)search_ms * 1000000;
    xs_cover_search(cover, remaining, selected, excluded, occupied * 14);
  }
  int tokens = 0;
  for (int index = 0; index < cover->count; index++) {
    if (cover->best[index]) {
      tokens++;
      const XSCandidate *candidate = &cover->candidates[index];
      for (size_t letter = 0; letter < strlen(candidate->word->word);
           letter++) {
        remaining[candidate->start + ((int)letter * candidate->step)] = false;
      }
    }
  }
  for (int square = 0; square < 225; square++) {
    tokens += remaining[square];
  }
  xs_put(out, (uint32_t)tokens, 8);
  for (int index = 0; index < cover->count; index++) {
    if (cover->best[index]) {
      const XSCandidate *candidate = &cover->candidates[index];
      xs_put(out, 1, 1);
      xs_put(out, (uint32_t)candidate->start, 8);
      xs_put(out, candidate->step == 15, 1);
      xs_put(out, candidate->word->code, candidate->word->length);
    }
  }
  for (int square = 0; square < 225; square++) {
    if (remaining[square]) {
      xs_put(out, 0, 1);
      xs_put(out, (uint32_t)square, 8);
      xs_put(out, (uint32_t)(xs_upper(position->board[square]) - 'A'), 5);
    }
  }
  free(cover);
}
static void xs_put_score(XSBuffer *bits, int32_t score) {
  if (score >= 0 && score <= 1022) {
    xs_put(bits, (uint32_t)score, 10);
  } else {
    xs_put(bits, 1023, 10);
    xs_put_signed(bits, score);
  }
}
static int32_t xs_get_score(XSBuffer *bits) {
  uint32_t score = xs_get(bits, 10);
  return score == 1023 ? xs_get_signed(bits) : (int32_t)score;
}
char *xs_position_encode(const char *cgp, const char *data_paths,
                         unsigned codebook, unsigned search_ms,
                         ErrorStack *errors) {
  XSPosition position;
  if (codebook > 2 || search_ms > 60000 || !xs_parse_cgp(cgp, &position)) {
    xs_error(errors, "invalid English CGP or codec options");
    return NULL;
  }
  XSCodebook *book =
      codebook ? xs_codebook_load(data_paths, codebook, errors) : NULL;
  if (codebook && !book) {
    return NULL;
  }
  XSBuffer bits;
  XSBuffer packed;
  XSBuffer words;
  xs_buffer_init(&bits);
  xs_buffer_init(&packed);
  xs_buffer_init(&words);
  for (int square = 0; square < 225; square++) {
    xs_put(&packed, position.board[square] != 0, 1);
  }
  for (int square = 0; square < 225; square++) {
    if (position.board[square]) {
      xs_put(&packed, (uint32_t)(xs_upper(position.board[square]) - 'A'), 5);
    }
  }
  if (book) {
    xs_word_board(&position, book, search_ms, &words);
  }
  bool use_words = book && words.bits + 2 < packed.bits;
  xs_put(&bits, XS_POSITION_VERSION, 4);
  xs_write_metadata(&bits, position.language, position.lexicon);
  xs_put_score(&bits, position.scores[0]);
  xs_put_score(&bits, position.scores[1]);
  xs_put(&bits, position.zeros, 3);
  for (int player = 0; player < 2; player++) {
    xs_put(&bits, (uint32_t)strlen(position.racks[player]), 3);
  }
  xs_put(&bits, use_words, 1);
  if (use_words) {
    xs_put(&bits, codebook, 2);
    xs_append(&bits, &words);
  } else {
    xs_append(&bits, &packed);
  }
  unsigned blanks = 0;
  for (int square = 0; square < 225; square++) {
    blanks += position.board[square] >= 'a' && position.board[square] <= 'z';
  }
  xs_put(&bits, blanks, 2);
  unsigned occupied = 0;
  for (int square = 0; square < 225; square++) {
    if (position.board[square]) {
      if (position.board[square] >= 'a' && position.board[square] <= 'z') {
        xs_put(&bits, occupied, 7);
      }
      occupied++;
    }
  }
  for (int player = 0; player < 2; player++) {
    for (const char *tile = position.racks[player]; *tile; tile++) {
      xs_put(&bits, *tile == '?' ? 26 : (uint32_t)(*tile - 'A'), 5);
    }
  }
  char *token = xs_token(&bits, "XSP");
  xs_buffer_destroy(&bits);
  xs_buffer_destroy(&packed);
  xs_buffer_destroy(&words);
  xs_codebook_destroy(book);
  return token;
}
char *xs_position_decode(const char *token, const char *data_paths,
                         ErrorStack *errors) {
  XSBuffer bits;
  xs_buffer_init(&bits);
  XSPosition position = {0};
  bool valid = xs_untoken(&bits, token, "XSP") &&
               xs_get(&bits, 4) == XS_POSITION_VERSION;
  XSCodebook *book = NULL;
  if (valid) {
    xs_read_metadata(&bits, position.language, position.lexicon);
    position.scores[0] = xs_get_score(&bits);
    position.scores[1] = xs_get_score(&bits);
    position.zeros = xs_get(&bits, 3);
    unsigned rack_lengths[2];
    rack_lengths[0] = xs_get(&bits, 3);
    rack_lengths[1] = xs_get(&bits, 3);
    if (xs_get(&bits, 1)) {
      unsigned id = xs_get(&bits, 2);
      book = xs_codebook_load(data_paths, id, errors);
      valid = book != NULL;
      unsigned count = xs_get(&bits, 8);
      for (unsigned index = 0; valid && bits.valid && index < count; index++) {
        unsigned kind = xs_get(&bits, 1);
        unsigned start = xs_get(&bits, 8);
        unsigned step = 1;
        char literal[2] = {0};
        const char *word = literal;
        if (kind) {
          step = xs_get(&bits, 1) ? 15 : 1;
          word = xs_codebook_read(book, &bits);
        } else {
          unsigned tile = xs_get(&bits, 5);
          if (tile >= 26) {
            bits.valid = false;
          }
          literal[0] = (char)('A' + tile);
        }
        for (size_t letter = 0; valid && letter < strlen(word); letter++) {
          size_t square = start + (letter * step);
          valid = square < 225 && (step == 15 || square / 15 == start / 15) &&
                  (!position.board[square] ||
                   position.board[square] == word[letter]);
          if (valid) {
            position.board[square] = word[letter];
          }
        }
      }
    } else {
      for (int square = 0; square < 225; square++) {
        position.board[square] = xs_get(&bits, 1) ? 'A' : 0;
      }
      for (int square = 0; square < 225; square++) {
        if (position.board[square]) {
          unsigned tile = xs_get(&bits, 5);
          if (tile >= 26) {
            bits.valid = false;
          }
          position.board[square] = (char)('A' + tile);
        }
      }
    }
    unsigned occupied[225];
    unsigned count = 0;
    for (unsigned square = 0; square < 225; square++) {
      if (position.board[square]) {
        occupied[count++] = square;
      }
    }
    unsigned blanks = xs_get(&bits, 2);
    unsigned previous = 0;
    if (blanks > 2) {
      valid = false;
    }
    for (unsigned index = 0; valid && index < blanks; index++) {
      unsigned blank = xs_get(&bits, 7);
      valid = blank < count && (!index || blank > previous);
      previous = blank;
      if (valid) {
        unsigned square = occupied[blank];
        position.board[square] = (char)(position.board[square] + 'a' - 'A');
      }
    }
    for (int player = 0; player < 2; player++) {
      for (unsigned index = 0; index < rack_lengths[player]; index++) {
        unsigned tile = xs_get(&bits, 5);
        if (tile > 26) {
          valid = false;
        }
        position.racks[player][index] = (char)(tile == 26 ? '?' : 'A' + tile);
      }
    }
  }
  valid = valid && xs_finish(&bits) && xs_inventory(&position);
  char *cgp = valid ? xs_format_cgp(&position) : NULL;
  if (!valid && error_stack_is_empty(errors)) {
    xs_error(errors, "invalid, damaged, or unsupported XSP");
  }
  xs_codebook_destroy(book);
  xs_buffer_destroy(&bits);
  return cgp;
}
