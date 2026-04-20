#include "opening_racks_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/letter_distribution_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void exec_config_quiet(Config *config, const char *cmd) {
  (void)fflush(stdout);
  int saved_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
  (void)dup2(devnull, STDOUT_FILENO);
  close(devnull);

  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_load_command(config, cmd, error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);

  (void)fflush(stdout);
  (void)dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

typedef struct OpeningRackCtx {
  Game *game;
  MoveList *move_list;
  const LetterDistribution *ld;
  int ld_size;
  MachineLetter j_ml;
  FILE *fp;
  uint64_t racks_processed;
} OpeningRackCtx;

static void rack_to_cstr(const Rack *rack, const LetterDistribution *ld,
                         char *out, size_t out_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, rack, ld, false);
  const char *s = string_builder_peek(sb);
  size_t len = strlen(s);
  if (len >= out_size) {
    len = out_size - 1;
  }
  memcpy(out, s, len);
  out[len] = '\0';
  string_builder_destroy(sb);
}

static void move_to_cstr(const Move *move, const Game *game, char *out,
                         size_t out_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          false);
  const char *s = string_builder_peek(sb);
  size_t len = strlen(s);
  if (len >= out_size) {
    len = out_size - 1;
  }
  memcpy(out, s, len);
  out[len] = '\0';
  string_builder_destroy(sb);
}

static void move_leave_to_cstr(const Rack *rack, const Move *move,
                               const LetterDistribution *ld, char *out,
                               size_t out_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move_leave(sb, rack, move, ld);
  const char *s = string_builder_peek(sb);
  size_t len = strlen(s);
  if (len >= out_size) {
    len = out_size - 1;
  }
  memcpy(out, s, len);
  out[len] = '\0';
  string_builder_destroy(sb);
}

// Classify top move.
typedef enum {
  OPENING_TOP_TILE_PLACEMENT,
  OPENING_TOP_EXCH1,
  OPENING_TOP_EXCH_MULTI,
  OPENING_TOP_PASS,
} opening_top_kind_t;

static opening_top_kind_t classify_move(const Move *m) {
  switch (move_get_type(m)) {
  case GAME_EVENT_EXCHANGE:
    return move_get_tiles_played(m) == 1 ? OPENING_TOP_EXCH1
                                         : OPENING_TOP_EXCH_MULTI;
  case GAME_EVENT_PASS:
    return OPENING_TOP_PASS;
  default:
    return OPENING_TOP_TILE_PLACEMENT;
  }
}

static const char *top_kind_str(opening_top_kind_t kind) {
  switch (kind) {
  case OPENING_TOP_TILE_PLACEMENT:
    return "tile";
  case OPENING_TOP_EXCH1:
    return "exch1";
  case OPENING_TOP_EXCH_MULTI:
    return "exch";
  case OPENING_TOP_PASS:
    return "pass";
  }
  return "?";
}

// Returns the machine letter that an exchange-1 move trades.
static MachineLetter exch1_tile(const Move *m) {
  MachineLetter t = move_get_tile(m, 0);
  if (get_is_blanked(t)) {
    return BLANK_MACHINE_LETTER;
  }
  return t;
}

static void ml_to_cstr(const LetterDistribution *ld, MachineLetter ml,
                       char *out, size_t out_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_user_visible_letter(sb, ld, ml);
  const char *s = string_builder_peek(sb);
  size_t len = strlen(s);
  if (len >= out_size) {
    len = out_size - 1;
  }
  memcpy(out, s, len);
  out[len] = '\0';
  string_builder_destroy(sb);
}

static void process_opening_rack(OpeningRackCtx *ctx, const Rack *rack) {
  Game *game = ctx->game;
  MoveList *move_list = ctx->move_list;

  draw_rack_from_bag(game, 0, rack);

  const MoveGenArgs args = {.game = game,
                            .move_list = move_list,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .thread_index = 0,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .target_leave_size_for_exchange_cutoff =
                                UNSET_LEAVE_SIZE};
  generate_moves(&args);
  move_list_sort_moves(move_list);

  const int count = move_list_get_count(move_list);
  assert(count >= 1);

  const Move *top = move_list_get_move(move_list, 0);
  const Equity top_eq = move_get_equity(top);
  const Equity next_eq = count >= 2
                             ? move_get_equity(move_list_get_move(move_list, 1))
                             : EQUITY_MIN_VALUE;

  char rack_str[32];
  char top_move_str[128];
  char top_leave_str[32];
  char top_exch_tile_str[8] = "";
  rack_to_cstr(rack, ctx->ld, rack_str, sizeof(rack_str));
  move_to_cstr(top, game, top_move_str, sizeof(top_move_str));
  move_leave_to_cstr(rack, top, ctx->ld, top_leave_str, sizeof(top_leave_str));

  const opening_top_kind_t top_kind = classify_move(top);
  if (top_kind == OPENING_TOP_EXCH1) {
    ml_to_cstr(ctx->ld, exch1_tile(top), top_exch_tile_str,
               sizeof(top_exch_tile_str));
  }

  const bool rack_has_j = rack_get_letter(rack, ctx->j_ml) > 0;

  // Among all exchange-1 moves whose exchanged tile is NOT J, find the best
  // one (highest equity). Its leave keeps the J and is the 6-tile leave for
  // the "nearest miss" computation.
  Equity j_exch1_keep_eq = EQUITY_MIN_VALUE;
  char j_exch1_keep_leave_str[32] = "";
  char j_exch1_keep_tile_str[8] = "";
  if (rack_has_j) {
    for (int i = 0; i < count; i++) {
      const Move *m = move_list_get_move(move_list, i);
      if (classify_move(m) != OPENING_TOP_EXCH1) {
        continue;
      }
      if (exch1_tile(m) == ctx->j_ml) {
        continue;
      }
      j_exch1_keep_eq = move_get_equity(m);
      move_leave_to_cstr(rack, m, ctx->ld, j_exch1_keep_leave_str,
                         sizeof(j_exch1_keep_leave_str));
      ml_to_cstr(ctx->ld, exch1_tile(m), j_exch1_keep_tile_str,
                 sizeof(j_exch1_keep_tile_str));
      break;
    }
  }

  const int64_t top_eq_v = (int64_t)top_eq;
  const int64_t next_eq_v = (int64_t)next_eq;
  const int64_t margin_v = top_eq_v - next_eq_v;

  // TSV columns:
  //  1. rack
  //  2. top_move
  //  3. top_eq
  //  4. top_kind (tile|exch1|exch|pass)
  //  5. top_exch_tile (exchanged letter if exch1)
  //  6. top_leave
  //  7. next_eq
  //  8. margin  (= top_eq - next_eq)
  //  9. j_exch1_keep_eq  (empty if rack has no J or no valid move)
  // 10. j_exch1_keep_tile
  // 11. j_exch1_keep_leave
  // 12. j_miss_margin  (= top_eq - j_exch1_keep_eq)
  if (rack_has_j && j_exch1_keep_eq != EQUITY_MIN_VALUE) {
    const int64_t j_eq_v = (int64_t)j_exch1_keep_eq;
    const int64_t j_miss_v = top_eq_v - j_eq_v;
    (void)fprintf(ctx->fp,
                  "%s\t%s\t%" PRId64 "\t%s\t%s\t%s\t%" PRId64 "\t%" PRId64
                  "\t%" PRId64 "\t%s\t%s\t%" PRId64 "\n",
                  rack_str, top_move_str, top_eq_v, top_kind_str(top_kind),
                  top_exch_tile_str, top_leave_str, next_eq_v, margin_v, j_eq_v,
                  j_exch1_keep_tile_str, j_exch1_keep_leave_str, j_miss_v);
  } else {
    (void)fprintf(ctx->fp,
                  "%s\t%s\t%" PRId64 "\t%s\t%s\t%s\t%" PRId64 "\t%" PRId64
                  "\t\t\t\t\n",
                  rack_str, top_move_str, top_eq_v, top_kind_str(top_kind),
                  top_exch_tile_str, top_leave_str, next_eq_v, margin_v);
  }

  return_rack_to_bag(game, 0);
  ctx->racks_processed++;
  if (ctx->racks_processed % 100000 == 0) {
    (void)fprintf(stderr, "  processed %" PRIu64 " racks\n",
                  ctx->racks_processed);
    (void)fflush(stderr);
  }
}

static void enumerate_racks(OpeningRackCtx *ctx, Rack *rack, MachineLetter ml) {
  while (ml < ctx->ld_size &&
         ld_get_dist(ctx->ld, ml) - rack_get_letter(rack, ml) == 0) {
    ml++;
  }
  const int total = rack_get_total_letters(rack);
  if (ml == ctx->ld_size) {
    if (total == (RACK_SIZE)) {
      process_opening_rack(ctx, rack);
    }
    return;
  }
  // Option A: add no more of this letter; advance.
  enumerate_racks(ctx, rack, ml + 1);

  int max_add = ld_get_dist(ctx->ld, ml) - (int)rack_get_letter(rack, ml);
  const int remaining_capacity = (RACK_SIZE)-total;
  if (max_add > remaining_capacity) {
    max_add = remaining_capacity;
  }
  for (int i = 0; i < max_add; i++) {
    rack_add_letter(rack, ml);
    enumerate_racks(ctx, rack, ml + 1);
  }
  rack_take_letters(rack, ml, max_add);
}

void test_opening_racks_static_eval(void) {
  log_set_level(LOG_FATAL);
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 10000 -threads 1");

  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  MoveList *move_list = move_list_create(10000);

  // Clear any randomly-drawn starting rack so the bag is full.
  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);

  const char *outfile = "/tmp/opening_racks.tsv";
  FILE *fp = fopen(outfile, "we");
  assert(fp);
  (void)fprintf(fp, "rack\ttop_move\ttop_eq\ttop_kind\ttop_exch_tile\ttop_leave"
                    "\tnext_eq\tmargin"
                    "\tj_exch1_keep_eq\tj_exch1_keep_tile\tj_exch1_keep_leave"
                    "\tj_miss_margin\n");

  OpeningRackCtx ctx = {.game = game,
                        .move_list = move_list,
                        .ld = ld,
                        .ld_size = ld_size,
                        .j_ml = ld_hl_to_ml(ld, "J"),
                        .fp = fp,
                        .racks_processed = 0};

  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_size);
  enumerate_racks(&ctx, &rack, 0);

  (void)fclose(fp);

  (void)fprintf(stderr, "\nopening_racks: %" PRIu64 " racks written to %s\n",
                ctx.racks_processed, outfile);
  (void)fflush(stderr);

  move_list_destroy(move_list);
  config_destroy(config);
}
