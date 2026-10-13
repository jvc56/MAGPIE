#include "xs_test.h"

#include "../src/compat/xs_compress.h"
#include "../src/ent/conversion_results.h"
#include "../src/impl/convert.h"
#include "../src/impl/xs_codebook.h"
#include "../src/impl/xs_game.h"
#include "../src/impl/xs_position.h"
#include "../src/impl/xs_rank.h"
#include "../src/util/hash.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "../src/util/xs_bits.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char xs_fixture[] =
    "6DOWN5/7U7/6STEPSONs1/1OCCUR1F3META/4MUrAENAS2V/5B1B5QI/7L6A/6KEX5N/"
    "2F2OIDIA3N1/1PLATY3HELLO1/2U10T1/ZOEAE8A1/E1D10T1/R9ROGER/O10WIDE "
    "AIGGHRS/ 306/413 0 -lex CSW24";
static const char xs_gcg[] =
    "#lexicon CSW24\n#player1 alice Alice  Example\n#player2 bob Bób "
    "Test\n>alice: AEINRST 8D RETAINS +66 66\n>bob: A 7D AA +8 8\n>alice: ? 9D "
    "zZ +20 86\n>alice: ? -- -20 66\n>bob: A -1 +0 8\n>alice: ? -? +0 "
    "66\n>bob: - +0 8\n>alice: (A) +2 68\n#rack2 E\n";
static void xs_assert_ok(ErrorStack *errors) {
  if (!error_stack_is_empty(errors)) {
    error_stack_print_and_reset(errors);
    assert(0);
  }
}
static void xs_assert_position(const char *cgp, const char *paths,
                               unsigned book) {
  ErrorStack *errors = error_stack_create();
  char *token = xs_position_encode(cgp, paths, book, 0, errors);
  xs_assert_ok(errors);
  assert(token);
  char *decoded = xs_position_decode(token, paths, errors);
  xs_assert_ok(errors);
  assert(decoded);
  XSPosition original;
  assert(xs_parse_cgp(cgp, &original));
  char *canonical = xs_format_cgp(&original);
  assert(!strcmp(decoded, canonical));
  char *again = xs_position_encode(decoded, paths, book, 0, errors);
  xs_assert_ok(errors);
  assert(!strcmp(again, token));
  free(token);
  free(decoded);
  free(canonical);
  free(again);
  error_stack_destroy(errors);
}
static void xs_assert_game(const char *gcg, const char *paths, unsigned profile,
                           bool mixed) {
  ErrorStack *errors = error_stack_create();
  XSGameStats stats;
  char *token = xs_game_encode(gcg, paths, profile, &stats, errors);
  xs_assert_ok(errors);
  assert(token);
  if (mixed) {
    assert(stats.ranked > 0);
    assert(stats.explicit_moves > 0);
  }
  char *decoded = xs_game_decode(token, paths, errors);
  xs_assert_ok(errors);
  assert(decoded);
  assert(strstr(decoded, "Alice  Example"));
  assert(strstr(decoded, "Bób Test"));
  assert(strstr(decoded, "9D zZ"));
  assert(strstr(decoded, "#rack2 E\n"));
  char *again = xs_game_encode(decoded, paths, profile, NULL, errors);
  xs_assert_ok(errors);
  assert(!strcmp(again, token));
  // Every truncation, and a payload mutation with stale CRC, must fail cleanly.
  for (size_t size = 0; size < strlen(token); size++) {
    char *bad = string_duplicate(token);
    bad[size] = '\0';
    assert(!xs_game_decode(bad, paths, errors));
    assert(!error_stack_is_empty(errors));
    error_stack_reset(errors);
    free(bad);
  }
  token[10] = token[10] == 'A' ? 'B' : 'A';
  assert(!xs_game_decode(token, paths, errors));
  error_stack_reset(errors);
  free(token);
  free(decoded);
  free(again);
  error_stack_destroy(errors);
}
// Freeze complete top-1024 candidate lists, including tie ordering and blank
// designations, independently of round trips through the current
// implementation.
static void xs_assert_rank_profile(const char *paths, unsigned profile) {
  static const char *const hashes[2][3] = {
      {"c461f2b499d5acf1b667b447dc9ab779749aa2efd197d461d5be17130a1595eb",
       "38bf9fae30672fd871e7ad9dc3b3647ab553543b6188886792fbfff797ad632d",
       "ebdd2d6f0aeac0e9c12ea1bdcbbb5c4ce2db0633377ab1c8f2922a7f0cc7c8de"},
      {"883d1358e89ae86c250f607f77a1a5e20f739ca2c12b2cd78270f275e7201d7e",
       "893c9a4afa45da3414da55e4ecf154fc5863d7d48d396cb3f0c03bc63fc6b520",
       "b22fda33182edadf69a24612a5e87d0517daaf1ef4652e891b5494a080785775"}};
  const char *racks[] = {"AEINRST", "?AEINRS", "AIGGHRS"};
  ErrorStack *errors = error_stack_create();
  XSRanker *ranker = xs_ranker_create(paths, profile, errors);
  xs_assert_ok(errors);
  assert(ranker);
  for (int fixture = 0; fixture < 3; fixture++) {
    XSPosition position = {0};
    if (fixture == 2) {
      assert(xs_parse_cgp(xs_fixture, &position));
    }
    int count = 0;
    XSMove *moves =
        xs_rank_moves(ranker, position.board, racks[fixture], &count, errors);
    xs_assert_ok(errors);
    assert(moves && count > 0);
    StringBuilder *text = string_builder_create();
    for (int index = 0; index < count; index++) {
      string_builder_add_formatted_string(
          text, "%u:%u:%s:%lld\n", moves[index].start, moves[index].down,
          moves[index].word, (long long)moves[index].equity);
    }
    const char *bytes = string_builder_peek(text);
    char *digest = sha256_hash_bytes(bytes, strlen(bytes));
    assert(strcmp(digest, hashes[profile - 1][fixture]) == 0);
    free(digest);
    string_builder_destroy(text);
    free(moves);
  }
  xs_ranker_destroy(ranker);
  error_stack_destroy(errors);
}
static void xs_assert_golden_position(const char *paths, unsigned book) {
  static const char *const tokens[] = {
      "WFNQUQpkzo4AeABAAf59Hh_kFBgIEDgk-"
      "J8fEAL4BUAKAPwA8brNpKZHyc2ThCpErCTAykQEaCVQhghYFEu0rkDQBr1gng5FrdSeXEAQB"
      "BzjFxiRdZAyQwyBBjHjIIUe31Y",
      "WFNQUQpkzo4onBlSAkj_MZZc4WznDAEqA722W9FrvqfKxDilzIM3caVLN5rj6zuqPWD_"
      "kW6d6Y_euZLML4Up0uk8KASxCGGQIMY8ZDupapk",
      "WFNQUQpkzo4wxBk3zJH-vBy5sBucLS3bzzt9SJMQ6RoyDD99KhT5rqjvuqeeD_"
      "tMeZbgRO9UivXTVuF6_L0uYKQGEKJEIRWkYBHkWIQwyBBjHjI4aLt6"};
  ErrorStack *errors = error_stack_create();
  char *decoded = xs_position_decode(tokens[book], paths, errors);
  xs_assert_ok(errors);
  XSPosition position;
  assert(xs_parse_cgp(xs_fixture, &position));
  char *canonical = xs_format_cgp(&position);
  assert(decoded && strcmp(decoded, canonical) == 0);
  free(canonical);
  free(decoded);
  error_stack_destroy(errors);
}

static void xs_assert_golden_game(const char *paths, unsigned profile) {
  static const char *const tokens[] = {
      "WFNHIQgKwtjSxsoOQWxpY2UgIEV4YW1wbGUDYm9iBKFh2bEQKjK5ugQBsMiImAQakEIAoLoQ"
      "AAEAHsObIFBBAJ1kQQARAAAkAAoGUIJSAIQFFFK1pbE",
      "WFNHIQoKwtjSxsoOQWxpY2UgIEV4YW1wbGUDYm9iBKFh2bEQKjK5ugQFwSpdKdD8IAolYpEA"
      "HsObIFBBAJ1kQQARAAAkAAoGUIJSAIQFFNKsu24",
      "WFNHIQwKwtjSxsoOQWxpY2UgIEV4YW1wbGUDYm9iBKFh2bEQKjK5ugQFwSpdKdDUIAolYpEA"
      "HsObIFBBAJ1kQQARAAAkAAoGUIJSAIQFFG9fyWU"};
  ErrorStack *errors = error_stack_create();
  char *decoded = xs_game_decode(tokens[profile], paths, errors);
  xs_assert_ok(errors);
  const char *expected =
      "#character-encoding UTF-8\n#language en\n#lexicon CSW24\n"
      "#player1 alice Alice  Example\n#player2 bob Bób Test\n"
      ">alice: AEINRST 8D RETAINS +66 66\n>bob: AA 7D AA +8 8\n"
      ">alice: ?Z 9D zZ +20 86\n>alice: ? -- -20 66\n"
      ">bob: A -1 +0 8\n>alice: ? -? +0 66\n>bob:  - +0 8\n"
      ">alice:  (A) +2 68\n#rack2 E\n";
  assert(decoded && strcmp(decoded, expected) == 0);
  char *encoded = xs_game_encode(xs_gcg, paths, profile, NULL, errors);
  xs_assert_ok(errors);
  assert(encoded && strcmp(encoded, tokens[profile]) == 0);
  free(encoded);
  free(decoded);
  error_stack_destroy(errors);
}

// Recompute CRC after mutations to exercise parsers beyond the transport check.
static void xs_mutate_payload(const char *token, bool game) {
  ErrorStack *errors = error_stack_create();
  for (unsigned trial = 0; trial < 96; trial++) {
    XSBuffer payload;
    xs_buffer_init(&payload);
    const char *magic = game ? "XSG" : "XSP";
    assert(xs_untoken(&payload, token, magic));
    size_t bit = ((size_t)trial * 97U) % payload.bits;
    payload.bytes[bit / 8] ^= (unsigned char)(1U << (7 - bit % 8));
    char *mutated = xs_token(&payload, magic);
    assert(mutated);
    char *decoded = game ? xs_game_decode(mutated, "/missing", errors)
                         : xs_position_decode(mutated, "/missing", errors);
    if (decoded) {
      xs_assert_ok(errors);
      if (!game) {
        XSPosition position;
        assert(xs_parse_cgp(decoded, &position));
      }
      free(decoded);
    } else {
      assert(!error_stack_is_empty(errors));
      error_stack_reset(errors);
    }
    free(mutated);
    xs_buffer_destroy(&payload);
  }
  error_stack_destroy(errors);
}

void test_xs(void) {
  ErrorStack *errors = error_stack_create();
  XSBuffer bits;
  xs_buffer_init(&bits);
  const int32_t values[] = {INT32_MIN, -1024, -1,   0,        899,
                            1022,      1023,  9000, INT32_MAX};
  for (unsigned index = 0; index < sizeof(values) / sizeof(*values); index++) {
    xs_put_signed(&bits, values[index]);
  }
  for (unsigned index = 0; index < sizeof(values) / sizeof(*values); index++) {
    assert(xs_get_signed(&bits) == values[index]);
  }
  assert(bits.valid);
  xs_buffer_destroy(&bits);
  xs_assert_position(xs_fixture, "/missing", 0);
  XSPosition position;
  assert(xs_parse_cgp(xs_fixture, &position));
  position.scores[0] = INT32_MIN;
  position.scores[1] = INT32_MAX;
  strcpy(position.lexicon, "custom-lexicon");
  char *cgp = xs_format_cgp(&position);
  xs_assert_position(cgp, "/missing", 0);
  free(cgp);
  char *token = xs_position_encode(xs_fixture, "/missing", 0, 0, errors);
  assert(token);
  for (size_t size = 0; size < strlen(token); size++) {
    char *bad = string_duplicate(token);
    bad[size] = '\0';
    assert(!xs_position_decode(bad, "/missing", errors));
    error_stack_reset(errors);
    free(bad);
  }
  xs_mutate_payload(token, false);
  free(token);
  // CRC-valid packets still have to satisfy version and structural checks.
  for (unsigned version = 0; version < 16; version++) {
    xs_buffer_init(&bits);
    xs_put(&bits, version, 4);
    token = xs_token(&bits, "XSP");
    assert(!xs_position_decode(token, "/missing", errors));
    error_stack_reset(errors);
    free(token);
    xs_buffer_destroy(&bits);
  }
  xs_assert_game(xs_gcg, "/missing", 0, false);
  xs_assert_golden_game("/missing", 0);
  xs_assert_golden_position("/missing", 0);
  token = xs_game_encode(xs_gcg, "/missing", 0, NULL, errors);
  xs_assert_ok(errors);
  assert(token);
  xs_mutate_payload(token, true);
  free(token);
  assert(xs_utf8("Bób \xf0\x9f\x90\xa6"));
  assert(!xs_utf8("\xc0\xaf"));
  assert(!xs_utf8("\xed\xa0\x80"));
  assert(!xs_utf8("\xf4\x90\x80\x80"));
  assert(!xs_utf8("\xe2\x82"));
  xs_buffer_init(&bits);
  xs_put_text(&bits, "\xc0\xaf");
  char *invalid_text = xs_get_text(&bits);
  assert(!bits.valid);
  free(invalid_text);
  xs_buffer_destroy(&bits);
  assert(!xs_game_encode("#player1 a a\n#player2 b b\n>a: A O15 AA +2 2\n",
                         "/missing", 0, NULL, errors));
  error_stack_reset(errors);
  assert(!xs_game_encode("#player1 a a\n#player2 b b\n>a: -- -2 0\n",
                         "/missing", 0, NULL, errors));
  error_stack_reset(errors);
  // Huffman prefix validation, including a fractional length and
  // oversubscription.
  const char *prefix =
      "{\"format\":\"xs-word-codebook\",\"version\":1,\"id\":\"en-CSW24-v1\","
      "\"language\":\"en\",\"lexicon\":\"CSW24\",\"words\":";
  const char *bad_words[] = {"[[\"AA\",1],[\"AB\",1],[\"AC\",1]]}",
                             "[[\"AA\",1.5]]}",
                             "[[\"AA\",33]]}",
                             "[[\"AA\",0]]}",
                             "[[\"AA\",2],[\"AB\",2]]}",
                             "[[\"AB\",1],[\"AA\",1]]}"};
  for (unsigned index = 0; index < sizeof(bad_words) / sizeof(*bad_words);
       index++) {
    char *json = get_formatted_string("%s%s", prefix, bad_words[index]);
    assert(!xs_codebook_parse(json, errors));
    error_stack_reset(errors);
    free(json);
  }
  // conv uses explicit paths and accepts compressed or plain count inputs.
  char input[] = "/tmp/magpie-xs-counts-XXXXXX";
  int descriptor = mkstemp(input);
  assert(descriptor >= 0);
  close(descriptor);
  char *output = get_formatted_string("%s.gz", input);
  const char counts[] = "AB\t2\nAA\t4\nAC\t1\nAD\t1\n";
  assert(xs_write_file(input, counts, strlen(counts), errors));
  ConversionArgs args = {.conversion_type_string = "counts2xsw",
                         .input_and_output_name = input,
                         .ld_name = output,
                         .klv_name = "CSW24"};
  ConversionResults *results = conversion_results_create();
  convert(&args, results, errors);
  xs_assert_ok(errors);
  size_t size = 0;
  size_t expanded = 0;
  char *raw = xs_read_file(output, &size, errors);
  char *json = xs_gunzip(raw, size, &expanded);
  assert(json);
  XSCodebook *book = xs_codebook_parse(json, errors);
  xs_assert_ok(errors);
  assert(book && book->count == 4);
  assert(xs_codebook_find(book, "AA")->length == 1);
  assert(xs_codebook_find(book, "AD")->length == 3);
  xs_codebook_destroy(book);
  free(raw);
  free(json);
  conversion_results_destroy(results);
  int removed_input = remove(input);
  int removed_output = remove(output);
  assert(removed_input == 0 && removed_output == 0);
  free(output);
  error_stack_destroy(errors);
}
void test_xs_data(void) {
  const char *paths = getenv("MAGPIE_XS_DATA");
  assert(paths && "Set MAGPIE_XS_DATA to the MAGPIE-DATA #17 data directory");
  xs_assert_position(xs_fixture, paths, 1);
  xs_assert_position(xs_fixture, paths, 2);
  // Cyclically permute ceil(10% of occupied squares), preserving the physical
  // inventory and designated blanks while disrupting dictionary words.
  XSPosition position;
  assert(xs_parse_cgp(xs_fixture, &position));
  int occupied[225];
  int count = 0;
  for (int index = 0; index < 225; index++) {
    if (position.board[index]) {
      occupied[count++] = index;
    }
  }
  for (int trial = 0; trial < 30; trial++) {
    int selected = (count + 9) / 10;
    int offset = trial % count;
    char first = position.board[occupied[offset]];
    for (int index = 0; index < selected - 1; index++) {
      position.board[occupied[(offset + index) % count]] =
          position.board[occupied[(offset + index + 1) % count]];
    }
    position.board[occupied[(offset + selected - 1) % count]] = first;
    char *cgp = xs_format_cgp(&position);
    xs_assert_position(cgp, paths, 1);
    xs_assert_position(cgp, paths, 2);
    free(cgp);
  }
  xs_assert_game(xs_gcg, paths, 1, true);
  xs_assert_game(xs_gcg, paths, 2, true);
  for (unsigned profile = 1; profile <= 2; profile++) {
    xs_assert_rank_profile(paths, profile);
    xs_assert_golden_game(paths, profile);
    xs_assert_golden_position(paths, profile);
  }
}
