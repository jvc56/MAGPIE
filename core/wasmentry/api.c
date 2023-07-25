#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#include "../src/config.h"
#include "../src/game.h"
#include "../src/log.h"
#include "../src/sim.h"

EMSCRIPTEN_KEEPALIVE
void sim_position(char *cgp, char *lexicon_name, int threads) {
  // The CGP already contains the lexicon name, but this is easier so
  // we don't have to re-parse it.

  char dist[20] = "english.csv";
  char leaves[20] = "english.klv2";
  char winpct[20] = "winpct.csv";
  char lexicon_file[20];
  if (strcmp(lexicon_name, "CSW21") == 0) {
    strcpy(leaves, "CSW21.klv2");
  }
  strcpy(lexicon_file, lexicon_name);
  strcat(lexicon_file, ".kwg");

  Config *config =
      create_config(lexicon_file, dist, cgp, leaves, SORT_BY_EQUITY,
                    PLAY_RECORDER_TYPE_ALL, "", SORT_BY_EQUITY,
                    PLAY_RECORDER_TYPE_ALL, 0, 0, "", 0, 0, 0, 0, 1, winpct);

  Game *game = create_game(config);
  log_debug("created game. loading cgp: %s", config->cgp);
  load_cgp(game, config->cgp);
  log_debug("loaded game");
  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;

  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  int nmoves = ml->count;
  sort_moves(ml);
  Simmer *simmer = create_simmer(config, game);
  log_debug("nmoves: %d", nmoves);
  int limit = 50;
  if (nmoves < limit) {
    limit = nmoves;
  }

  prepare_simmer(simmer, 5, threads, ml->moves, limit, NULL);
  set_stopping_condition(simmer, SIM_STOPPING_CONDITION_99PCT);
  // clock_t begin = clock();
  // int iters = 10000;
  // for (int i = 0; i < iters; i++) {
  //   sim_single_iteration(simmer, 2, 0);
  // }
  // clock_t end = clock();
  // printf("%d iters took %0.6f seconds\n", iters,
  //        (double)(end - begin) / CLOCKS_PER_SEC);

  blocking_simulate(simmer);

  print_sim_stats(simmer);

  destroy_game(game);
  destroy_simmer(simmer);
}

int main() {
  log_set_level(LOG_DEBUG);

  // sim_position("C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/"
  //              "6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/"
  //              "7S7 EEEIILZ/ 336/298 0 lex NWL20;");
  return 0;
}