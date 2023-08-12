#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/game.h"
#include "../src/go_params.h"
#include "../src/infer.h"
#include "../src/log.h"
#include "../src/sim.h"
#include "../src/thread_control.h"
#include "../src/ucgi_command.h"

#include "test_constants.h"

void test_ucgi_command() {
  UCGICommandVars *ucgi_command_vars = create_ucgi_command_vars();

  size_t len;
  size_t prev_len = 0;
  char *buf;
  FILE *file_handler = open_memstream(&buf, &len);

  set_outfile(ucgi_command_vars, file_handler);
  char test_stdin_input[256];

  // Test the ucgi command
  int result = process_ucgi_command_async("ucgi", ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  assert(!strcmp("id name MAGPIE 0.1\nucgiok\n", buf + prev_len));
  prev_len = len;

  // Test the quit command
  result = process_ucgi_command_async("quit", ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_QUIT);
  prev_len = len;

  // Test the position cgp command
  snprintf(test_stdin_input, sizeof(test_stdin_input), "%s%s", "position cgp ",
           ION_OPENING_CGP);
  result = process_ucgi_command_async(test_stdin_input, ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  assert(!strcmp("CSW21", ucgi_command_vars->last_lexicon_name));
  assert(!strcmp("english", ucgi_command_vars->last_ld_name));
  assert(ucgi_command_vars->loaded_game->gen->bag->last_tile_index + 1 == 83);
  prev_len = len;
  memset(test_stdin_input, 0, 256);

  // Test go parse failures
  // invalid stop cond
  snprintf(test_stdin_input, sizeof(test_stdin_input), "%s",
           "go stopcondition 93");
  result = process_ucgi_command_async(test_stdin_input, ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_PARSE_FAILED);
  prev_len = len;
  memset(test_stdin_input, 0, 256);

  // Test go parse failures
  // stop cond and infinite
  snprintf(test_stdin_input, sizeof(test_stdin_input), "%s",
           "go stopcondition 95 infinite");
  result = process_ucgi_command_async(test_stdin_input, ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_PARSE_FAILED);
  prev_len = len;
  memset(test_stdin_input, 0, 256);

  // Test go parse failures
  // sim + depth of 0
  snprintf(test_stdin_input, sizeof(test_stdin_input), "%s",
           "go sim stopcondition 95 depth 0");
  result = process_ucgi_command_async(test_stdin_input, ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_PARSE_FAILED);
  prev_len = len;
  memset(test_stdin_input, 0, 256);

  // Test go parse failures
  // nonpositive threads
  snprintf(test_stdin_input, sizeof(test_stdin_input), "%s",
           "go sim stopcondition 95 depth 1 threads 0");
  result = process_ucgi_command_async(test_stdin_input, ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_PARSE_FAILED);
  prev_len = len;
  memset(test_stdin_input, 0, 256);

  // Test go parse failures
  // nonpositive threads
  snprintf(test_stdin_input, sizeof(test_stdin_input), "%s", "go sim infer");
  result = process_ucgi_command_async(test_stdin_input, ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_PARSE_FAILED);
  prev_len = len;
  memset(test_stdin_input, 0, 256);

  // Test sim finishing normally
  snprintf(test_stdin_input, sizeof(test_stdin_input), "%s%s", "position cgp ",
           ZILLION_OPENING_CGP);
  result = process_ucgi_command_async(test_stdin_input, ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  snprintf(test_stdin_input, sizeof(test_stdin_input), "%s",
           "go sim depth 2 stopcondition 99 threads 8 plays 3 i 200");
  result = process_ucgi_command_async(test_stdin_input, ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // Check the go params
  assert(ucgi_command_vars->go_params->depth == 2);
  assert(ucgi_command_vars->go_params->stop_condition ==
         SIM_STOPPING_CONDITION_99PCT);
  assert(ucgi_command_vars->go_params->threads == 8);
  assert(ucgi_command_vars->go_params->num_plays == 15);
  assert(ucgi_command_vars->go_params->max_iterations == 100);
  // Poll for the end of the command
  while (1) {
    if (get_mode(ucgi_command_vars->thread_control) == MODE_STOPPED) {
      break;
    } else {
      sleep(1);
    }
  }
  // Command is done

  prev_len = len;
  memset(test_stdin_input, 0, 256);

  //   printf(">%s<\n>%s<\n>%s<\n", buf + prev_len,
  //          ucgi_command_vars->last_lexicon_name,
  //          ucgi_command_vars->last_ld_name);
  // Test sim halted
  fclose(file_handler);
  free(buf);
  destroy_ucgi_command_vars(ucgi_command_vars);
}