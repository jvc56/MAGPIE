// Pass-relative check diagnostics. Both branches use the same sampled
// opponent rack and the same candidate leave/refill for our next turn.
static void pcd_pass_relative_checks(
    const Game *game, const PTGSettings *settings, const Move *const *cands,
    int count, const Rack *racks, int num_racks, MoveList *reply_list,
    const MachineLetter *pool, int pool_size, uint64_t seed, long pos,
    double *threat, double *setup) {
  const int our_idx = game_get_player_on_turn_index(game);
  const int opponent_idx = 1 - our_idx;
  const bool conditioned = ptg_env_long("PCD_CONDITION_DRAWS", 1) != 0;
  Game *pass_replied[PTG_MAX_R];
  double pass_scores[PTG_MAX_R];
  MachineLetter draws[PTG_MAX_R][PTG_POOL_CAP];
  int draw_sizes[PTG_MAX_R];
  uint64_t draw_rng = seed * UINT64_C(2654435761) + UINT64_C(12345);
  Move *pass_move = move_create();
  move_set_as_pass(pass_move);
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    Rack excluded;
    rack_copy(&excluded, &racks[rack_idx]);
    int draw_size = 0;
    for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
      const MachineLetter tile = pool[tile_idx];
      if (conditioned && rack_get_letter(&excluded, tile) > 0) {
        rack_take_letter(&excluded, tile);
      } else {
        draws[rack_idx][draw_size++] = tile;
      }
    }
    assert(!conditioned || rack_get_total_letters(&excluded) == 0);
    draw_sizes[rack_idx] = draw_size;
    for (int draw_idx = 0; draw_idx < draw_size - 1; draw_idx++) {
      const int pick = draw_idx + (int)(ptg_next(&draw_rng) %
                                        (uint64_t)(draw_size - draw_idx));
      const MachineLetter tile = draws[rack_idx][pick];
      draws[rack_idx][pick] = draws[rack_idx][draw_idx];
      draws[rack_idx][draw_idx] = tile;
    }
    Game *baseline = game_duplicate(game);
    play_move(pass_move, baseline, NULL);
    rack_copy(player_get_rack(game_get_player(baseline, opponent_idx)),
              &racks[rack_idx]);
    ptg_generate(baseline, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                 NULL, false, 0);
    pass_scores[rack_idx] =
        equity_to_double(ptg_best_placement_score(reply_list));
    const Move *reply =
        move_list_get_count(reply_list) > 0 &&
                move_get_type(move_list_get_move(reply_list, 0)) ==
                    GAME_EVENT_TILE_PLACEMENT_MOVE
            ? move_list_get_move(reply_list, 0)
            : pass_move;
    play_move(reply, baseline, NULL);
    pass_replied[rack_idx] = baseline;
  }
  const char *check_path = getenv("PCD_CHECK_OUT");
  FILE *check_out = check_path != NULL ? fopen(check_path, "a+") : NULL;
  if (check_path != NULL) {
    assert(check_out != NULL);
    fseek(check_out, 0, SEEK_END);
    if (ftell(check_out) == 0) {
      fprintf(check_out,
              "pos,pool_index,move,static_eq,pass_reply_mean,"
              "candidate_reply_mean,blocking_delta,blocking_adjustment,"
              "pass_followup_mean,candidate_followup_mean,setup_delta,"
              "setup_adjustment,racks,conditioned\n");
    }
  }
  for (int cand_idx = 0; cand_idx < count; cand_idx++) {
    Rack leave;
    rack_copy(&leave, player_get_rack(game_get_player(game, our_idx)));
    for (int tile_idx = 0; tile_idx < move_get_tiles_length(cands[cand_idx]);
         tile_idx++) {
      const MachineLetter tile = move_get_tile(cands[cand_idx], tile_idx);
      if (tile != PLAYED_THROUGH_MARKER ||
          move_get_type(cands[cand_idx]) == GAME_EVENT_EXCHANGE) {
        rack_take_letter(&leave,
                         get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile);
      }
    }
    double reply_sum = 0.0;
    double pass_reply_sum = 0.0;
    double after_followup_sum = 0.0;
    double pass_followup_sum = 0.0;
    for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
      Game *after = game_duplicate(game);
      play_move(cands[cand_idx], after, NULL);
      rack_copy(player_get_rack(game_get_player(after, opponent_idx)),
                &racks[rack_idx]);
      ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                   NULL, false, 0);
      reply_sum += equity_to_double(ptg_best_placement_score(reply_list));
      pass_reply_sum += pass_scores[rack_idx];
      const Move *reply =
          move_list_get_count(reply_list) > 0 &&
                  move_get_type(move_list_get_move(reply_list, 0)) ==
                      GAME_EVENT_TILE_PLACEMENT_MOVE
              ? move_list_get_move(reply_list, 0)
              : pass_move;
      play_move(reply, after, NULL);
      const int missing = RACK_SIZE - rack_get_total_letters(&leave);
      const int draw_count =
          missing < draw_sizes[rack_idx] ? missing : draw_sizes[rack_idx];
      for (int draw_no = 0; draw_no < settings->off_draws; draw_no++) {
        Rack *after_rack = player_get_rack(game_get_player(after, our_idx));
        Rack *base_rack =
            player_get_rack(game_get_player(pass_replied[rack_idx], our_idx));
        rack_copy(after_rack, &leave);
        rack_copy(base_rack, &leave);
        for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
          const MachineLetter tile =
              draws[rack_idx]
                   [(draw_no * draw_count + draw_idx) % draw_sizes[rack_idx]];
          rack_add_letter(after_rack, tile);
          rack_add_letter(base_rack, tile);
        }
        ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                     NULL, false, 0);
        after_followup_sum +=
            equity_to_double(ptg_best_placement_score(reply_list));
        ptg_generate(pass_replied[rack_idx], reply_list, MOVE_RECORD_BEST,
                     MOVE_SORT_SCORE, 0, NULL, false, 0);
        pass_followup_sum +=
            equity_to_double(ptg_best_placement_score(reply_list));
      }
      game_destroy(after);
    }
    const double equity = equity_to_double(move_get_equity(cands[cand_idx]));
    const double blocking_delta = (pass_reply_sum - reply_sum) / num_racks;
    const double samples = (double)num_racks * settings->off_draws;
    const double setup_delta =
        (after_followup_sum - pass_followup_sum) / samples;
    threat[cand_idx] = equity + settings->weight * blocking_delta;
    setup[cand_idx] = equity + settings->off_w * setup_delta;
    if (check_out != NULL) {
      StringBuilder *move_text = string_builder_create();
      string_builder_add_ucgi_move(move_text, cands[cand_idx],
                                   game_get_board(game), game_get_ld(game));
      fprintf(check_out,
              "%ld,%d,%s,%.6f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%d,%d\n",
              pos, cand_idx + 1, string_builder_peek(move_text), equity,
              pass_reply_sum / num_racks, reply_sum / num_racks, blocking_delta,
              2.0 * settings->weight * blocking_delta,
              pass_followup_sum / samples, after_followup_sum / samples,
              setup_delta, 3.0 * settings->off_w * setup_delta, num_racks,
              conditioned);
      string_builder_destroy(move_text);
    }
  }
  if (check_out != NULL) {
    fclose(check_out);
  }
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    game_destroy(pass_replied[rack_idx]);
  }
  move_destroy(pass_move);
}
