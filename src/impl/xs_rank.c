#include "xs_rank.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../ent/board_layout.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/players_data.h"
#include "../ent/rack.h"
#include "../util/hash.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "../util/xs_bits.h"
#include "cgp.h"
#include "move_gen.h"
#include "xs_position.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { XS_RANK_CAPACITY = 100000, XS_RANK_LIMIT = 1024 };
// Profile 1/2 freezes the standard English rules, not the user's settings.
static const char xs_layout[] =
    "7, 7\n=  '   =   '  =\n -   \"   \"   - \n  -   ' '   -  \n'  -   '   -  "
    "'\n    -     -    \n \"   \"   \"   \" \n  '   ' '   '  \n=  '   -   '  "
    "=\n  '   ' '   '  \n \"   \"   \"   \" \n    -     -    \n'  -   '   -  "
    "'\n  -   ' '   -  \n -   \"   \"   - \n=  '   =   '  =";
static const char xs_distribution[] =
    "?,?,2,0,0\nA,a,9,1,1\nB,b,2,3,0\nC,c,2,3,0\nD,d,4,2,0\nE,e,12,1,1\nF,f,2,"
    "4,0\nG,g,3,2,0\nH,h,2,4,0\nI,i,9,1,1\nJ,j,1,8,0\nK,k,1,5,0\nL,l,4,1,0\nM,"
    "m,2,3,0\nN,n,6,1,0\nO,o,8,1,1\nP,p,2,3,0\nQ,q,1,10,0\nR,r,6,1,0\nS,s,4,1,"
    "0\nT,t,6,1,0\nU,u,4,1,1\nV,v,2,4,0\nW,w,2,4,0\nX,x,1,8,0\nY,y,2,4,0\nZ,z,"
    "1,10,0\n";
void xs_ranker_destroy(XSRanker *ranker) {
  if (!ranker) {
    return;
  }
  if (ranker->game) {
    game_destroy(ranker->game);
  }
  if (ranker->players) {
    players_data_destroy(ranker->players);
  }
  if (ranker->layout) {
    board_layout_destroy(ranker->layout);
  }
  ld_destroy(ranker->distribution);
  free(ranker);
}
XSRanker *xs_ranker_create(const char *paths, unsigned profile,
                           ErrorStack *errors) {
  static const char *const names[2][2] = {{"CSW24.kwg", "CSW24.klv2"},
                                          {"NWL23.kwg", "NWL23.klv2"}};
  static const char *const hashes[2][2] = {
      {"62ca7a84f07429a9976f77a4f74b94911aca5d49cce050dce72dd0e032c0566f",
       "b0ef5f6637cca0cd8e0962d6fb7d35b2d22f9bf648015f48fa8f80e24a60610b"},
      {"3e74af981fdd974e107283f686da0fe4b7ec84ad0d825d444330c338c33b91ba",
       "37dea945c29c3773eb4cd5a4117f3d3256c8b548cd3bf3b5ce8a219cb5e0a3fa"}};
  // These constants differ in super-board builds.
  // NOLINTNEXTLINE(misc-redundant-expression)
  if (BOARD_DIM != 15 || RACK_SIZE != 7 || profile < 1 || profile > 2) {
    xs_error(errors, "rank profile requires a 15-square board, seven-tile "
                     "racks, and profile 1 or 2");
    return NULL;
  }
  // Resolve each asset once; load from the same root that was verified.
  char *roots[2] = {NULL, NULL};
  for (int index = 0; index < 2 && error_stack_is_empty(errors); index++) {
    char *path = xs_asset_path(paths, names[profile - 1][index], errors);
    if (path) {
      char *hash = sha256_hash_file(path, errors);
      if (!hash || strcmp(hash, hashes[profile - 1][index]) != 0) {
        xs_error(errors, "rank profile asset checksum mismatch");
      }
      free(hash);
      char *suffix = strstr(path, "/xs/english-v1/");
      if (!suffix) {
        xs_error(errors, "invalid rank asset path");
      } else {
        *suffix = '\0';
        roots[index] = string_duplicate(path);
      }
      free(path);
    }
  }
  XSRanker *ranker = calloc_or_die(1, sizeof(*ranker));
  if (error_stack_is_empty(errors)) {
    ranker->players = players_data_create(false);
    const char *name =
        profile == 1 ? "../xs/english-v1/CSW24" : "../xs/english-v1/NWL23";
    players_data_set(ranker->players, PLAYERS_DATA_TYPE_KWG, roots[0], name,
                     name, false, errors);
    if (error_stack_is_empty(errors)) {
      players_data_set(ranker->players, PLAYERS_DATA_TYPE_KLV, roots[1], name,
                       name, false, errors);
    }
    ranker->distribution = calloc_or_die(1, sizeof(*ranker->distribution));
    StringSplitter *lines = split_string_by_newline(xs_distribution, true);
    ld_create_internal("english", lines, ranker->distribution, errors);
    string_splitter_destroy(lines);
    ranker->layout = board_layout_create();
    lines = split_string_by_newline(xs_layout, false);
    board_layout_parse_split_file(ranker->layout, "xs-standard15-v1", lines,
                                  errors);
    string_splitter_destroy(lines);
    if (error_stack_is_empty(errors)) {
      GameArgs args = {.players_data = ranker->players,
                       .board_layout = ranker->layout,
                       .ld = ranker->distribution,
                       .game_variant = GAME_VARIANT_CLASSIC,
                       .bingo_bonus = 50,
                       .seed = 0};
      ranker->game = game_create(&args);
    }
  }
  free(roots[0]);
  free(roots[1]);
  if (!error_stack_is_empty(errors)) {
    xs_ranker_destroy(ranker);
    return NULL;
  }
  return ranker;
}
static int xs_move_order(const void *left, const void *right) {
  const XSMove *first = left;
  const XSMove *second = right;
  if (first->equity != second->equity) {
    return first->equity > second->equity ? -1 : 1;
  }
  if (first->start != second->start) {
    return first->start < second->start ? -1 : 1;
  }
  if (first->down != second->down) {
    return first->down < second->down ? -1 : 1;
  }
  return strcmp(first->word, second->word);
}
XSMove *xs_rank_moves(XSRanker *ranker, const char board[225], const char *rack,
                      int *count, ErrorStack *errors) {
  *count = 0;
  XSPosition position = {.language = "en", .lexicon = "CSW24"};
  memcpy(position.board, board, 225);
  if (!*rack || strlen(rack) > 7) {
    return NULL;
  }
  strcpy(position.racks[0], rack);
  char *cgp = xs_format_cgp(&position);
  XSPosition checked;
  if (!xs_parse_cgp(cgp, &checked)) {
    free(cgp);
    return NULL;
  }
  game_load_cgp(ranker->game, cgp, errors);
  free(cgp);
  if (!error_stack_is_empty(errors)) {
    return NULL;
  }
  MoveList *moves = move_list_create(XS_RANK_CAPACITY);
  MoveGenArgs args = {.game = ranker->game,
                      .move_record_type = MOVE_RECORD_ALL,
                      .move_sort_type = MOVE_SORT_SCORE,
                      .target_equity = EQUITY_MAX_VALUE,
                      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
                      .move_list = moves,
                      .disable_pat = true};
  generate_moves(&args);
  int generated = move_list_get_count(moves);
  if (generated == XS_RANK_CAPACITY) {
    move_list_destroy(moves);
    xs_error(errors, "rank candidate limit reached");
    return NULL;
  }
  XSMove *result = calloc_or_die((size_t)generated + 1, sizeof(*result));
  const KLV *klv = players_data_get_klv(ranker->players, 0);
  for (int index = 0; index < generated; index++) {
    const Move *move = move_list_get_move(moves, index);
    if (move->move_type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    XSMove candidate = {.start = (move->row_start * 15U) + move->col_start,
                        .down = move->dir};
    Rack leave;
    rack_set_dist_size_and_reset(&leave, 27);
    for (const char *letter = rack; *letter; letter++) {
      rack_add_letter(&leave, *letter == '?' ? 0 : *letter - 'A' + 1);
    }
    for (int tile = 0; tile < move->tiles_length; tile++) {
      MachineLetter ml = move->tiles[tile];
      candidate.word[tile] =
          (char)(!ml ? '.'
                     : (ml & BLANK_MASK ? 'a' : 'A') + (ml & UNBLANK_MASK) - 1);
      if (ml) {
        rack_take_letter(&leave, ml & BLANK_MASK ? 0 : ml);
      }
    }
    candidate.equity = (int64_t)move->score + klv_get_leave_value(klv, &leave);
    result[(*count)++] = candidate;
  }
  move_list_destroy(moves);
  qsort(result, (size_t)*count, sizeof(*result), xs_move_order);
  int unique = 0;
  for (int index = 0; index < *count && unique < XS_RANK_LIMIT; index++) {
    if (!unique || xs_move_order(&result[index], &result[unique - 1]) != 0) {
      result[unique++] = result[index];
    }
  }
  *count = unique;
  return result;
}
