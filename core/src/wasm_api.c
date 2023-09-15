#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "game.h"
#include "move.h"
#include "ucgi_command.h"
#include "ucgi_formats.h"
#include "words.h"

static UCGICommandVars *ucgi_command_vars = NULL;

// tiles must contain 0 for play-through tiles!
char *score_play(char *cgpstr, int move_type, int row, int col, int vertical,
                 uint8_t *tiles, uint8_t *leave, int ntiles, int nleave) {

  char lexicon[20] = "";
  char ldname[20] = "";
  lexicon_ld_from_cgp(cgpstr, lexicon, ldname);
  Config *config = NULL;
  load_config_from_lexargs(&config, cgpstr, lexicon, ldname);

  Game *game = create_game(config);
  load_cgp(game, cgpstr);

  int tiles_played = 0;
  for (int i = 0; i < ntiles; i++) {
    if (tiles[i] != 0) {
      tiles_played++;
    }
  }
  // score_move assumes the play is always horizontal.
  if (vertical) {
    transpose(game->gen->board);
    int ph = row;
    row = col;
    col = ph;
  }
  int points = 0;
  double leave_value = 0.0;
  FormedWords *fw = NULL;
  if (move_type == MOVE_TYPE_PLAY) {
    // Assume that that kwg is shared
    points =
        score_move(game->gen->board, tiles, 0, ntiles - 1, row, col,
                   tiles_played, !vertical, 0, game->gen->letter_distribution);

    if (vertical) {
      // transpose back.
      transpose(game->gen->board);
      int ph = row;
      row = col;
      col = ph;
    }

    fw = words_played(game->gen->board, tiles, 0, ntiles - 1, row, col,
                      vertical);
    // Assume that that kwg is shared
    populate_word_validities(fw, game->players[0]->strategy_params->kwg);
  }

  char *retstr = malloc(sizeof(char) * 400);
  Rack *leave_rack = NULL;
  int phonies_exist = 0;
  char phonies[200];

  if (nleave > 0) {
    leave_rack = create_rack(game->gen->letter_distribution->size);
    for (int i = 0; i < nleave; i++) {
      add_letter_to_rack(leave_rack, leave[i]);
    }
    leave_value =
        get_leave_value(game->players[0]->strategy_params->klv, leave_rack);
  }

  if (move_type == MOVE_TYPE_PLAY) {
    char *pp = phonies;
    char tile[MAX_LETTER_CHAR_LENGTH];
    for (int i = 0; i < fw->num_words; i++) {
      if (!fw->words[i].valid) {
        phonies_exist = 1;
        for (int mli = 0; mli < fw->words[i].word_length; mli++) {
          machine_letter_to_human_readable_letter(
              game->gen->letter_distribution, fw->words[i].word[mli], tile);
          pp += sprintf(pp, "%s", tile);
        }
        if (i < fw->num_words - 1) {
          pp += sprintf(pp, ",");
        }
      }
    }
    free(fw);
  }

  // Return a simple string
  // result <scored|error> valid <true|false> invalid_words FU,BARZ
  // eq 123.45 sc 100 currmove f3.FU etc

  char *tp = retstr;
  char move_placeholder[30];

  Move *move = create_move();
  set_move(move, tiles, 0, ntiles - 1, points, row, col, tiles_played, vertical,
           move_type);

  store_move_ucgi(move, game->gen->board, move_placeholder,
                  game->gen->letter_distribution);
  destroy_move(move);

  tp += sprintf(tp, "currmove %s", move_placeholder);
  tp += sprintf(tp, "result %s valid %s", "scored",
                phonies_exist ? "false" : "true");
  if (phonies_exist) {
    tp += sprintf(tp, " invalid_words %s", phonies);
  }
  tp += sprintf(tp, " sc %d eq %.3f", points, (double)points + leave_value);

  destroy_config(config);
  destroy_game(game);
  if (leave_rack != NULL) {
    destroy_rack(leave_rack);
  }

  // Caller can use UTF8ToString on the returned pointer but it MUST FREE
  // this string after it's done with it!
  return retstr;
}

int process_ucgi_command_wasm(char *cmd) {
  if (ucgi_command_vars == NULL) {
    ucgi_command_vars = create_ucgi_command_vars(NULL);
  }
  return process_ucgi_command_async(cmd, ucgi_command_vars);
}

char *ucgi_search_status_wasm() {
  return ucgi_search_status(ucgi_command_vars);
}

char *ucgi_stop_search_wasm() { return ucgi_stop_search(ucgi_command_vars); }