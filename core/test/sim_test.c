#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // for sleep

#include "../src/move.h"
#include "../src/sim.h"
#include "../src/winpct.h"

#include "superconfig.h"
#include "test_util.h"

void test_win_pct(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  assert(within_epsilon(win_pct(config->win_pcts, 118, 90), 0.844430));
}

void test_sim_single_iteration(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);
  Game *game = create_game(config);
  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "AAADERW",
                      game->gen->letter_distribution);
  int sorting_type = game->players[0]->strategy_params->move_sorting;
  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  sort_moves(ml);
  Simmer *simmer = create_simmer(config, game);
  prepare_simmer(simmer, 2, 1, ml->moves, 15, NULL);
  sim_single_iteration(simmer, 2, 0);

  assert(game->gen->board->tiles_played == 0);
  assert(simmer->game_copies[0]->gen->board->tiles_played == 0);
  game->players[0]->strategy_params->move_sorting = sorting_type;

  destroy_game(game);
  destroy_simmer(simmer);
}

void test_more_iterations(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);
  Game *game = create_game(config);
  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "AEIQRST",
                      game->gen->letter_distribution);
  int sorting_type = game->players[0]->strategy_params->move_sorting;
  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;

  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  sort_moves(ml);
  Simmer *simmer = create_simmer(config, game);
  prepare_simmer(simmer, 2, 1, ml->moves, 15, NULL);
  for (int i = 0; i < 200; i++) {
    sim_single_iteration(simmer, 2, 0);
  }

  assert(simmer->game_copies[0]->gen->board->tiles_played == 0);
  //   print_sim_stats(simmer);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  game->players[0]->strategy_params->move_sorting = sorting_type;
  char placeholder[80];
  store_move_description(simmer->simmed_plays[0]->move, placeholder,
                         simmer->game->gen->letter_distribution);

  assert(strcmp(placeholder, "8G QI") == 0);

  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_sim(Config *config) {
  Game *game = create_game(config);

  load_cgp(game, config->cgp);

  int sorting_type = game->players[0]->strategy_params->move_sorting;
  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;

  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  sort_moves(ml);
  Simmer *simmer = create_simmer(config, game);
  prepare_simmer(simmer, 2, 1, ml->moves, 5, NULL);
  clock_t begin = clock();
  int iters = 10000;
  for (int i = 0; i < iters; i++) {
    sim_single_iteration(simmer, 2, 0);
  }
  clock_t end = clock();
  printf("%d iters took %0.6f seconds\n", iters,
         (double)(end - begin) / CLOCKS_PER_SEC);
  print_sim_stats(simmer);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  game->players[0]->strategy_params->move_sorting = sorting_type;
  char placeholder[80];
  store_move_description(simmer->simmed_plays[0]->move, placeholder,
                         simmer->game->gen->letter_distribution);

  assert(strcmp(placeholder, "14F ZI.E") == 0);

  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_multithread_sim(Config *config) {
  Game *game = create_game(config);
  int num_threads = config->number_of_threads;
  printf("Using %d threads\n", num_threads);
  load_cgp(game, config->cgp);

  int sorting_type = game->players[0]->strategy_params->move_sorting;
  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;

  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  sort_moves(ml);
  Simmer *simmer = create_simmer(config, game);
  prepare_simmer(simmer, 2, num_threads, ml->moves, 15, NULL);
  simulate(simmer);
  sleep(5);
  stop_simming(simmer);
  print_sim_stats(simmer);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  game->players[0]->strategy_params->move_sorting = sorting_type;
  char placeholder[80];
  store_move_description(simmer->simmed_plays[0]->move, placeholder,
                         simmer->game->gen->letter_distribution);

  assert(strcmp(placeholder, "14F ZI.E") == 0);
  destroy_game(game);
  destroy_simmer(simmer);
}

void perf_test_multithread_blocking_sim(Config *config) {
  Game *game = create_game(config);
  int num_threads = config->number_of_threads;
  printf("Using %d threads\n", num_threads);
  load_cgp(game, config->cgp);
  /* ... */

  int sorting_type = game->players[0]->strategy_params->move_sorting;
  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;

  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  sort_moves(ml);
  Simmer *simmer = create_simmer(config, game);
  // Sim 80 plays 5 plies, and stop when we're 99% sure we have the right play.
  prepare_simmer(simmer, 5, num_threads, ml->moves, 80, NULL);
  set_stopping_condition(simmer, SIM_STOPPING_CONDITION_99PCT);
  blocking_simulate(simmer);

  print_sim_stats(simmer);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  game->players[0]->strategy_params->move_sorting = sorting_type;
  char placeholder[80];
  store_move_description(simmer->simmed_plays[0]->move, placeholder,
                         simmer->game->gen->letter_distribution);

  assert(strcmp(placeholder, "14F ZI.E") == 0);
  destroy_game(game);
  destroy_simmer(simmer);
}

void test_play_similarity(SuperConfig *superconfig) {
  Config *config = superconfig->nwl_config;
  Game *game = create_game(config);

  draw_rack_to_string(game->gen->bag, game->players[0]->rack, "ACEIRST",
                      game->gen->letter_distribution);

  int sorting_type = game->players[0]->strategy_params->move_sorting;
  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;

  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  sort_moves(ml);
  Simmer *simmer = create_simmer(config, game);
  prepare_simmer(simmer, 2, 2, ml->moves, 30, NULL);

  // The first four plays all score 74. Only
  // 8F ATRESIC and 8F STEARIC should show up as similar, though.
  // These are play indexes 1 and 2.

  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 4; j++) {
      char p1[80];
      char p2[80];

      store_move_description(simmer->simmed_plays[i]->move, p1,
                             simmer->game->gen->letter_distribution);
      store_move_description(simmer->simmed_plays[j]->move, p2,
                             simmer->game->gen->letter_distribution);

      if (strcmp(p1, "8F ATRESIC") == 0 && strcmp(p2, "8F STEARIC") == 0) {
        assert(plays_are_similar(simmer, simmer->simmed_plays[i],
                                 simmer->simmed_plays[j]));
      } else if (strcmp(p2, "8F ATRESIC") == 0 &&
                 strcmp(p1, "8F STEARIC") == 0) {
        assert(plays_are_similar(simmer, simmer->simmed_plays[i],
                                 simmer->simmed_plays[j]));
      } else {
        assert(!plays_are_similar(simmer, simmer->simmed_plays[i],
                                  simmer->simmed_plays[j]));
      }
    }
  }

  assert(!plays_are_similar(simmer, simmer->simmed_plays[3],
                            simmer->simmed_plays[4]));

  game->players[0]->strategy_params->move_sorting = sorting_type;

  destroy_game(game);
  destroy_simmer(simmer);
}

void test_sim(SuperConfig *superconfig) {
  test_win_pct(superconfig);
  test_sim_single_iteration(superconfig);
  test_more_iterations(superconfig);
  test_play_similarity(superconfig);

  // And run a perf test.
  int threads = superconfig->nwl_config->number_of_threads;
  char *backup_cgp = superconfig->nwl_config->cgp;
  superconfig->nwl_config->number_of_threads = 4;
  superconfig->nwl_config->cgp =
      "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/"
      "7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ "
      "336/298 0 lex NWL20;";
  perf_test_multithread_sim(superconfig->nwl_config);
  // restore superconfig
  superconfig->nwl_config->cgp = backup_cgp;
  superconfig->nwl_config->number_of_threads = threads;
}
