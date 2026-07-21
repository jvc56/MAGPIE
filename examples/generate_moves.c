// Example of embedding magpie via the cmd_api string API.
//
// Build and run:
//   make examples
//   ./bin/generate_moves [data-paths]
//
// Sets up an empty board with the rack AQRTUYZ and prints the generated
// move list in machine-readable form.
#include "../src/impl/cmd_api.h"
#include <stdio.h>

int main(int argc, char *argv[]) {
  const char *data_paths = "./data";
  if (argc > 1) {
    data_paths = argv[1];
  }

  Magpie *mp = magpie_create(data_paths);
  if (magpie_has_error(mp)) {
    char *error = magpie_get_and_clear_error(mp);
    (void)fprintf(stderr, "failed to create magpie: %s", error);
    magpie_free_string(error);
    magpie_destroy(mp);
    return 1;
  }

  const char *commands[] = {
      "set -lex CSW21",
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AQRTUYZ/ 0/0 0",
      "generate",
  };
  const int num_commands = (int)(sizeof(commands) / sizeof(commands[0]));
  for (int cmd_idx = 0; cmd_idx < num_commands; cmd_idx++) {
    if (magpie_run_sync(mp, commands[cmd_idx]) != MAGPIE_SUCCESS) {
      char *error = magpie_get_and_clear_error(mp);
      (void)fprintf(stderr, "command '%s' failed: %s", commands[cmd_idx],
                    error);
      magpie_free_string(error);
      magpie_destroy(mp);
      return 1;
    }
  }

  char *moves = magpie_get_last_command_output(mp);
  printf("%s", moves);
  magpie_free_string(moves);
  magpie_destroy(mp);
  return 0;
}
