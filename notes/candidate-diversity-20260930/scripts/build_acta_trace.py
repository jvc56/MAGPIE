import hashlib,json,subprocess
from pathlib import Path
root=Path('/Users/olaugh/Documents/ChatGPT/Magpie')
original=root/'blocking-ahead-40-80-20260930'
output=root/'acta-loss-trace-20260930'
local=Path('/private/tmp/magpie-acta-loss-trace-20260930')
output.mkdir(exist_ok=True);local.mkdir(exist_ok=True)
function=r'''
static void pcd_trace_acta(void) {
  FILE *input = fopen(getenv("PCD_ACTA_TRACE_IN"), "r");
  FILE *out = fopen(getenv("PCD_ACTA_TRACE_OUT"), "w");
  assert(input != NULL && out != NULL);
  char line[4096];
  assert(fgets(line, sizeof(line), input) != NULL);
  fclose(input);
  char *cgp = strchr(line, ',') + 1;
  cgp[strcspn(cgp, "\r\n")] = '\0';
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  char *command = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, command);
  free(command);
  Game *base = config_get_game(config);
  const int our_idx = game_get_player_on_turn_index(base);
  for (int seat = 0; seat < 2; seat++) {
    player_set_pat_usage(game_get_player(base, seat), true, 0);
  }
  ErrorStack *errors = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                  "winpct_english", errors);
  assert(error_stack_is_empty(errors));
  MoveList *list = move_list_create(10000);
  ptg_generate(base, list, MOVE_RECORD_ALL, MOVE_SORT_EQUITY, 0, NULL, false, 0);
  Move *candidate = move_create();
  bool found = false;
  for (int index = 0; index < move_list_get_count(list); index++) {
    StringBuilder *text = string_builder_create();
    string_builder_add_ucgi_move(text, move_list_get_move(list, index),
                                 game_get_board(base), game_get_ld(base));
    if (strcmp(string_builder_peek(text), "7f ACTA") == 0) {
      move_copy(candidate, move_list_get_move(list, index));
      found = true;
    }
    string_builder_destroy(text);
  }
  assert(found && bag_get_letters(game_get_bag(base)) == 5);
  move_list_destroy(list);
  list = move_list_create(1);
  const long trials = ptg_env_long("PCD_ACTA_TRIALS", 10000);
  const uint64_t base_seed = (uint64_t)ptg_env_long("PCD_ACTA_SEED", 202609301042);
  long wins = 0, losses = 0, ties = 0, unfinished = 0;
  double wp_sum = 0.0;
  int logged = 0;
  const uint64_t start = ptg_now_ns();
  for (long trial = 0; trial < trials; trial++) {
    const uint64_t seed = base_seed + (uint64_t)trial;
    Game *game = game_duplicate(base);
    game_seed(game, seed);
    set_random_rack(game, 1 - our_idx, NULL);
    StringBuilder *trace = string_builder_create();
    char *initial = game_get_cgp(game, false);
    string_builder_add_formatted_string(trace, "INITIAL %s\n", initial);
    free(initial);
    play_move(candidate, game, NULL);
    assert(bag_get_letters(game_get_bag(game)) == 1);
    char *after = game_get_cgp(game, false);
    string_builder_add_formatted_string(trace, "AFTER_ACTA %s\n", after);
    free(after);
    Equity leftover = 0;
    int plies = 0;
    for (int ply = 0; ply < 16 && !game_over(game); ply++) {
      const int seat = game_get_player_on_turn_index(game);
      const Player *player = game_get_player(game, seat);
      Rack spare;
      rack_copy(&spare, player_get_rack(player));
      const Move *move = get_top_equity_move(game, list);
      StringBuilder *text = string_builder_create();
      string_builder_add_ucgi_move(text, move, game_get_board(game), game_get_ld(game));
      const double points = equity_to_double(move_get_score(move));
      if (ply >= 14) {
        const Equity value = get_leave_value_for_move(player_get_klv(player), move, &spare);
        leftover += seat == our_idx ? value : -value;
      }
      play_move(move, game, NULL);
      string_builder_add_formatted_string(trace,
          "PLY %d PLAYER %d MOVE %s POINTS %.0f SCORE %.0f/%.0f BAG %d\n",
          ply + 1, seat, string_builder_peek(text), points,
          equity_to_double(player_get_score(game_get_player(game, our_idx))),
          equity_to_double(player_get_score(game_get_player(game, 1 - our_idx))),
          bag_get_letters(game_get_bag(game)));
      string_builder_destroy(text);
      plies++;
    }
    const Equity spread = player_get_score(game_get_player(game, our_idx)) -
                          player_get_score(game_get_player(game, 1 - our_idx));
    double wp = 0.0;
    if (game_over(game)) {
      if (spread > 0) { wins++; wp = 1.0; }
      else if (spread < 0) { losses++; }
      else { ties++; wp = 0.5; }
    } else {
      unfinished++;
      const int seat = game_get_player_on_turn_index(game);
      const double margin_value = equity_to_double(spread + leftover);
      const int spread_leftover = (int)(margin_value + 0.5 - (margin_value < 0));
      wp = 1.0 - (double)win_pct_get(win_pcts, -spread_leftover,
          (unsigned int)bag_get_letters(game_get_bag(game)),
          (unsigned int)rack_get_total_letters(player_get_rack(game_get_player(game, seat))),
          (unsigned int)rack_get_total_letters(player_get_rack(game_get_player(game, 1-seat))));
    }
    wp_sum += wp;
    if (wp < 0.999 && logged < 12) {
      char *final = game_get_cgp(game, false);
      fprintf(out, "CASE trial=%ld seed=%llu terminal=%d spread=%.0f wp=%.9f plies=%d\n%sFINAL %s\nEND_CASE\n",
              trial, (unsigned long long)seed, game_over(game), equity_to_double(spread),
              wp, plies, string_builder_peek(trace), final);
      free(final);
      fflush(out);
      logged++;
    }
    string_builder_destroy(trace);
    game_destroy(game);
  }
  fprintf(out, "SUMMARY trials=%ld wins=%ld losses=%ld ties=%ld unfinished=%ld mean_wp=%.9f seconds=%.3f\n",
          trials,wins,losses,ties,unfinished,wp_sum/trials,
          (double)(ptg_now_ns()-start)/1000000000.0);
  fclose(out);
  move_list_destroy(list);
  move_destroy(candidate);
  win_pct_destroy(win_pcts);
  error_stack_destroy(errors);
  config_destroy(config);
}

'''
source=(original/'harness_snapshot.c').read_text()
trigger='void test_pat_candidate_diversity(void) {'
assert source.count(trigger)==1
source=source.replace(trigger,function+trigger+'\n  if (getenv("PCD_ACTA_TRACE_IN") != NULL) {\n    pcd_trace_acta();\n    return;\n  }')
path=output/'harness_snapshot.c';path.write_text(source)
obj=local/'pat_threat_games_test.o'
repo=Path('/Users/olaugh/sources/magpie-pat-pass-relative')
subprocess.run(['cc','-O3','-flto','-march=native','-Wall','-Wno-trigraphs','-DBOARD_DIM=15','-DRACK_SIZE=7','-I',str(repo/'test'),'-c',str(path),'-o',str(obj)],check=True)
objects=list(json.loads((original/'build.json').read_text())['linked_objects_sha256'])
binary=local/'magpie_test_acta_trace'
subprocess.run(['cc','-pthread','-flto',*objects,str(obj),'-lm','-o',str(binary)],check=True)
line=next(x for x in (original/'positions.cgp').read_text().splitlines() if x.startswith('281,'))
(local/'position.cgp').write_text(line+'\n')
(output/'build.json').write_text(json.dumps({'runtime_binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'original_study_unchanged':True},indent=2)+'\n')
print(binary)
