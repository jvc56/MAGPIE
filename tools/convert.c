#include "../src/ent/conversion_results.h"
#include "../src/impl/convert.h"
#include "../src/util/io_util.h"
#include <stdio.h>
#include <stdlib.h>

// A batch conversion must fail the build when conversion fails. The
// interactive CLI deliberately keeps processing after command errors.
int main(int argc, char *argv[]) {
  if (argc < 3 || argc > 5) {
    (void)fprintf(stderr,
                  "Usage: %s TYPE LEXICON [DATA_PATHS [LETTER_DISTRIBUTION]]\n",
                  argv[0]);
    return EXIT_FAILURE;
  }
  const ConversionArgs args = {
      .conversion_type_string = argv[1],
      .input_and_output_name = argv[2],
      .data_paths = argc >= 4 ? argv[3] : "./data",
      .ld_name = argc == 5 ? argv[4] : NULL,
      .num_threads = 1,
  };
  ErrorStack *error_stack = error_stack_create();
  ConversionResults *results = conversion_results_create();
  convert(&args, results, error_stack);
  const int status =
      error_stack_is_empty(error_stack) ? EXIT_SUCCESS : EXIT_FAILURE;
  if (status != EXIT_SUCCESS) {
    (void)fprintf(stderr, "Data conversion failed for %s (%s):\n", argv[2],
                  argv[1]);
    error_stack_print_and_reset(error_stack);
  }
  conversion_results_destroy(results);
  error_stack_destroy(error_stack);
  return status;
}
