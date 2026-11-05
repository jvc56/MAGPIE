#include "play_feature_registry.h"

#include "../def/play_feature_defs.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "play_feature_board.h"
#include "play_feature_equity.h"
#include "play_feature_rack.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

void play_feature_ctx_init(PlayFeatureCtx *ctx, const Game *game) {
  const Player *player =
      game_get_player(game, game_get_player_on_turn_index(game));
  ctx->game = game;
  ctx->ld = game_get_ld(game);
  ctx->klv = player_get_klv(player);
  rack_copy(&ctx->rack, player_get_rack(player));
  ctx->bag_tiles = bag_get_letters(game_get_bag(game));
  ctx->num_premium_squares = 0;
}

const char *play_feature_get_name(const play_feature_t feature) {
  switch (feature) {
  case PLAY_FEATURE_SCORE:
    return "score";
  case PLAY_FEATURE_LEAVE_VALUE:
    return "leave";
  case PLAY_FEATURE_EQUITY:
    return "equity";
  case PLAY_FEATURE_TILES_PLAYED:
    return "tiles";
  case PLAY_FEATURE_MOVE_TYPE:
    return "type";
  case PLAY_FEATURE_BAG_AFTER:
    return "bagafter";
  case PLAY_FEATURE_LEAVE_SHAPE:
    return "leaveshape";
  case PLAY_FEATURE_LEAVE_DUPLICATES:
    return "leavedups";
  case PLAY_FEATURE_LOCATION:
    return "location";
  case PLAY_FEATURE_DIRECTION:
    return "direction";
  case PLAY_FEATURE_PREMIUM_ACCESS:
    return "premium";
  case PLAY_FEATURE_PERIMETER:
    return "perimeter";
  case NUMBER_OF_PLAY_FEATURES:
    break;
  }
  log_fatal("unknown play feature %d", (int)feature);
  return NULL;
}

int play_feature_get_num_columns(const play_feature_t feature) {
  switch (feature) {
  case PLAY_FEATURE_SCORE:
  case PLAY_FEATURE_LEAVE_VALUE:
  case PLAY_FEATURE_EQUITY:
  case PLAY_FEATURE_TILES_PLAYED:
  case PLAY_FEATURE_BAG_AFTER:
  case PLAY_FEATURE_LEAVE_DUPLICATES:
  case PLAY_FEATURE_PERIMETER:
    return 1;
  case PLAY_FEATURE_LOCATION:
  case PLAY_FEATURE_DIRECTION:
  case PLAY_FEATURE_PREMIUM_ACCESS:
    return 2;
  case PLAY_FEATURE_MOVE_TYPE:
  case PLAY_FEATURE_LEAVE_SHAPE:
    return 3;
  case NUMBER_OF_PLAY_FEATURES:
    break;
  }
  log_fatal("unknown play feature %d", (int)feature);
  return 0;
}

play_feature_kind_t play_feature_get_kind(const play_feature_t feature) {
  switch (feature) {
  case PLAY_FEATURE_SCORE:
  case PLAY_FEATURE_LEAVE_VALUE:
  case PLAY_FEATURE_EQUITY:
  case PLAY_FEATURE_TILES_PLAYED:
  case PLAY_FEATURE_BAG_AFTER:
  case PLAY_FEATURE_LEAVE_SHAPE:
  case PLAY_FEATURE_LEAVE_DUPLICATES:
  case PLAY_FEATURE_LOCATION:
  case PLAY_FEATURE_PREMIUM_ACCESS:
  case PLAY_FEATURE_PERIMETER:
    return PLAY_FEATURE_KIND_CONTINUOUS;
  case PLAY_FEATURE_MOVE_TYPE:
  case PLAY_FEATURE_DIRECTION:
    return PLAY_FEATURE_KIND_CATEGORICAL;
  case NUMBER_OF_PLAY_FEATURES:
    break;
  }
  log_fatal("unknown play feature %d", (int)feature);
  return PLAY_FEATURE_KIND_CONTINUOUS;
}

void play_feature_prepare(const play_feature_t feature, PlayFeatureCtx *ctx) {
  switch (feature) {
  case PLAY_FEATURE_PREMIUM_ACCESS:
    play_feature_prepare_premium_access(ctx);
    return;
  case PLAY_FEATURE_SCORE:
  case PLAY_FEATURE_LEAVE_VALUE:
  case PLAY_FEATURE_EQUITY:
  case PLAY_FEATURE_TILES_PLAYED:
  case PLAY_FEATURE_MOVE_TYPE:
  case PLAY_FEATURE_BAG_AFTER:
  case PLAY_FEATURE_LEAVE_SHAPE:
  case PLAY_FEATURE_LEAVE_DUPLICATES:
  case PLAY_FEATURE_LOCATION:
  case PLAY_FEATURE_DIRECTION:
  case PLAY_FEATURE_PERIMETER:
    return;
  case NUMBER_OF_PLAY_FEATURES:
    break;
  }
  log_fatal("unknown play feature %d", (int)feature);
}

void play_feature_extract(const play_feature_t feature,
                          const PlayFeatureCtx *ctx, const MoveList *moves,
                          const int row_stride, double *out) {
  play_feature_extract_func_t extract = NULL;
  switch (feature) {
  case PLAY_FEATURE_SCORE:
    extract = play_feature_extract_score;
    break;
  case PLAY_FEATURE_LEAVE_VALUE:
    extract = play_feature_extract_leave_value;
    break;
  case PLAY_FEATURE_EQUITY:
    extract = play_feature_extract_equity;
    break;
  case PLAY_FEATURE_TILES_PLAYED:
    extract = play_feature_extract_tiles_played;
    break;
  case PLAY_FEATURE_MOVE_TYPE:
    extract = play_feature_extract_move_type;
    break;
  case PLAY_FEATURE_BAG_AFTER:
    extract = play_feature_extract_bag_after;
    break;
  case PLAY_FEATURE_LEAVE_SHAPE:
    extract = play_feature_extract_leave_shape;
    break;
  case PLAY_FEATURE_LEAVE_DUPLICATES:
    extract = play_feature_extract_leave_duplicates;
    break;
  case PLAY_FEATURE_LOCATION:
    extract = play_feature_extract_location;
    break;
  case PLAY_FEATURE_DIRECTION:
    extract = play_feature_extract_direction;
    break;
  case PLAY_FEATURE_PREMIUM_ACCESS:
    extract = play_feature_extract_premium_access;
    break;
  case PLAY_FEATURE_PERIMETER:
    extract = play_feature_extract_perimeter;
    break;
  case NUMBER_OF_PLAY_FEATURES:
    log_fatal("unknown play feature %d", (int)feature);
    return;
  }
  const int num_moves = move_list_get_count(moves);
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    extract(ctx, move_list_get_move(moves, move_idx),
            out + (ptrdiff_t)move_idx * row_stride);
  }
}

play_feature_t play_feature_from_name(const char *name) {
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    if (strings_iequal(name,
                       play_feature_get_name((play_feature_t)feature_idx))) {
      return (play_feature_t)feature_idx;
    }
  }
  return NUMBER_OF_PLAY_FEATURES;
}

static char *play_feature_known_names(void) {
  StringBuilder *sb = string_builder_create();
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    string_builder_add_formatted_string(
        sb, "%s%s", feature_idx == 0 ? "" : ", ",
        play_feature_get_name((play_feature_t)feature_idx));
  }
  return string_builder_dump_and_destroy(sb, NULL);
}

void play_feature_parse_set(const char *names, bool *active,
                            ErrorStack *error_stack) {
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    active[feature_idx] = false;
  }
  StringSplitter *splitter = split_string(names, ',', true);
  const int num_items = string_splitter_get_number_of_items(splitter);
  for (int item_idx = 0; item_idx < num_items; item_idx++) {
    const char *name = string_splitter_get_item(splitter, item_idx);
    const play_feature_t feature = play_feature_from_name(name);
    if (feature == NUMBER_OF_PLAY_FEATURES || active[feature]) {
      char *known = play_feature_known_names();
      error_stack_push(
          error_stack, ERROR_STATUS_LAMC_BAD_FEATURE_SET,
          get_formatted_string("%s play feature '%s'; the features are: %s",
                               feature == NUMBER_OF_PLAY_FEATURES ? "unknown"
                                                                  : "repeated",
                               name, known));
      free(known);
      string_splitter_destroy(splitter);
      return;
    }
    active[feature] = true;
  }
  string_splitter_destroy(splitter);
  if (num_items == 0) {
    error_stack_push(error_stack, ERROR_STATUS_LAMC_BAD_FEATURE_SET,
                     string_duplicate("the play feature list is empty"));
  }
}
