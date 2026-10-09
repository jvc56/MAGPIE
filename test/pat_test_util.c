#include "pat_test_util.h"

#include "../src/def/equity_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_eval.h"
#include "../src/ent/pat_lexicon.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>

char *create_temp_pat_data_dir(void) {
  char tmp_template[] = "/tmp/magpie_pat_XXXXXX";
  const char *tmp_dir = mkdtemp(tmp_template);
  assert(tmp_dir);
  char *strategy_dir = get_formatted_string("%s/strategy", tmp_dir);
  assert(mkdir(strategy_dir, 0755) == 0);
  free(strategy_dir);
  return string_duplicate(tmp_dir);
}

void write_pat_file_contents(const char *data_dir, const char *pat_name,
                             const char *contents) {
  ErrorStack *error_stack = error_stack_create();
  char *filename =
      get_formatted_string("%s/strategy/%s.pat", data_dir, pat_name);
  write_string_to_file(filename, "w", contents, error_stack);
  assert(error_stack_is_empty(error_stack));
  free(filename);
  error_stack_destroy(error_stack);
}

PATWeights *pat_test_create_prepared(const char *name, const Game *game) {
  PATWeights *pat = pat_create_zeroed(name);
  pat_prepare_hook_flex(pat, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  return pat;
}

void pat_test_assert_capped_penalty(const PATEvalContext *pat_eval_ctx,
                                    const Move *move, const Rack *leave,
                                    Equity exact) {
  const Equity floors[] = {EQUITY_MIN_VALUE, exact - 1000, exact - 1, exact,
                           exact + 1,        exact + 1000, 0,         1,
                           EQUITY_MAX_VALUE};
  const int num_floors = (int)(sizeof(floors) / sizeof(floors[0]));
  for (int floor_idx = 0; floor_idx < num_floors; floor_idx++) {
    const Equity floor = floors[floor_idx];
    const Equity capped =
        pat_eval_move_penalty_capped(pat_eval_ctx, move, leave, floor);
    if (exact >= floor) {
      assert(capped == exact);
    } else {
      assert(capped >= exact);
      assert(capped < floor);
    }
  }
}
