#include "preview_game.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/board_layout.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/players_data.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
  PREVIEW_DATA_PATH_CANDIDATES = 3,
  PREVIEW_LEXICA_NAME_MAX = 64,
  // Hard cap so a pathological position can't loop forever; 100 plies is
  // far more than any normal game reaches.
  PREVIEW_MAX_PLIES = 100,
};

// Probe the same data/ candidates the rest of the TUI uses, looking for a
// .kwg file matching `lexicon`. Returns the candidate path that resolved,
// or NULL if none.
static const char *find_data_paths(const char *lexicon) {
  static const char *candidates[PREVIEW_DATA_PATH_CANDIDATES] = {
      "data",
      "../data",
      "./data",
  };
  for (int idx = 0; idx < PREVIEW_DATA_PATH_CANDIDATES; idx++) {
    char path[512];
    const int written = snprintf(path, sizeof(path), "%s/lexica/%s.kwg",
                                 candidates[idx], lexicon);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
      continue;
    }
    if (access(path, R_OK) == 0) {
      return candidates[idx];
    }
  }
  return NULL;
}

// Pick any installed lexicon for the preview. CSW21 is the de-facto
// default; if it's not present we scan data/lexica for the first .kwg we
// can find. Caller-provided `out_lexicon` is populated on success.
static bool resolve_default_lexicon(char *out_lexicon, size_t out_size,
                                    const char **out_data_paths) {
  const char *probe = find_data_paths("CSW21");
  if (probe != NULL) {
    snprintf(out_lexicon, out_size, "CSW21");
    *out_data_paths = probe;
    return true;
  }
  static const char *candidates[PREVIEW_DATA_PATH_CANDIDATES] = {
      "data/lexica",
      "../data/lexica",
      "./data/lexica",
  };
  for (int idx = 0; idx < PREVIEW_DATA_PATH_CANDIDATES; idx++) {
    DIR *dir = opendir(candidates[idx]);
    if (dir == NULL) {
      continue;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      const size_t name_len = strlen(entry->d_name);
      if (name_len <= 4 || strcmp(entry->d_name + name_len - 4, ".kwg") != 0) {
        continue;
      }
      const size_t base_len = name_len - 4;
      if (base_len == 0 || base_len >= out_size ||
          base_len >= PREVIEW_LEXICA_NAME_MAX) {
        continue;
      }
      memcpy(out_lexicon, entry->d_name, base_len);
      out_lexicon[base_len] = '\0';
      closedir(dir);
      // Translate "data/lexica" candidate back to its data root.
      static const char *roots[PREVIEW_DATA_PATH_CANDIDATES] = {
          "data", "../data", "./data"};
      *out_data_paths = roots[idx];
      return true;
    }
    closedir(dir);
  }
  return false;
}

// Synchronous max-score autoplay. Mutates pg->game until game_over or no
// moves are generated.
static void play_out_max_score(TuiPreviewGame *pg) {
  MoveList *move_list = move_list_create(1);
  for (int ply = 0; ply < PREVIEW_MAX_PLIES; ply++) {
    if (game_over(pg->game)) {
      break;
    }
    const MoveGenArgs args = {
        .game = pg->game,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .move_list = move_list,
        .tiles_played_bv = NULL,
        .initial_tiles_bv = 0,
    };
    generate_moves_for_game(&args);
    if (move_list_get_count(move_list) <= 0) {
      break;
    }
    const Move *best = move_list_get_move(move_list, 0);
    play_move(best, pg->game, NULL);
  }
  move_list_destroy(move_list);
}

bool tui_preview_game_init(TuiPreviewGame *out_pg) {
  memset(out_pg, 0, sizeof(*out_pg));

  char lexicon[PREVIEW_LEXICA_NAME_MAX];
  const char *data_paths = NULL;
  if (!resolve_default_lexicon(lexicon, sizeof(lexicon), &data_paths)) {
    return false;
  }

  ErrorStack *err = error_stack_create();

  char *ld_name = ld_get_default_name_from_lexicon_name(lexicon, err);
  if (!error_stack_is_empty(err)) {
    goto fail;
  }
  out_pg->ld = ld_create(data_paths, ld_name, err);
  free(ld_name);
  if (!error_stack_is_empty(err)) {
    goto fail;
  }

  out_pg->players_data = players_data_create(false);
  players_data_set(out_pg->players_data, PLAYERS_DATA_TYPE_KWG, data_paths,
                   lexicon, lexicon, false, err);
  if (!error_stack_is_empty(err)) {
    goto fail;
  }
  // KLV is required by move_gen even when sorting purely by score (the
  // generator still consults the leave value to break ties); load the
  // lexicon's matching KLV.
  players_data_set(out_pg->players_data, PLAYERS_DATA_TYPE_KLV, data_paths,
                   lexicon, lexicon, false, err);
  if (!error_stack_is_empty(err)) {
    goto fail;
  }
  players_data_set_move_sort_type(out_pg->players_data, 0, MOVE_SORT_SCORE);
  players_data_set_move_sort_type(out_pg->players_data, 1, MOVE_SORT_SCORE);
  players_data_set_move_record_type(out_pg->players_data, 0, MOVE_RECORD_BEST);
  players_data_set_move_record_type(out_pg->players_data, 1, MOVE_RECORD_BEST);

  out_pg->board_layout = board_layout_create_default(data_paths, err);
  if (!error_stack_is_empty(err)) {
    goto fail;
  }

  // Fixed seed: the preview should stay visually stable across renders
  // (theme changes, resizes), and a pre-baked board feels more polished
  // than one that flickers between possibilities.
  const GameArgs args = {
      .players_data = out_pg->players_data,
      .board_layout = out_pg->board_layout,
      .ld = out_pg->ld,
      .game_variant = GAME_VARIANT_CLASSIC,
      .bingo_bonus = 50,
      .seed = (uint64_t)0x4d41475049450001ULL, // "MAGPIE\0\1"
  };
  out_pg->game = game_create(&args);
  if (out_pg->game == NULL) {
    goto fail;
  }
  draw_starting_racks(out_pg->game);
  play_out_max_score(out_pg);

  error_stack_destroy(err);
  return true;

fail:
  error_stack_destroy(err);
  tui_preview_game_destroy(out_pg);
  return false;
}

void tui_preview_game_destroy(TuiPreviewGame *pg) {
  if (pg == NULL) {
    return;
  }
  if (pg->game != NULL) {
    game_destroy(pg->game);
  }
  if (pg->board_layout != NULL) {
    board_layout_destroy(pg->board_layout);
  }
  if (pg->players_data != NULL) {
    players_data_destroy(pg->players_data);
  }
  if (pg->ld != NULL) {
    ld_destroy(pg->ld);
  }
  memset(pg, 0, sizeof(*pg));
}
