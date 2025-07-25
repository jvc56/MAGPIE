#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../src/compat/ctime.h"
#include "../../src/ent/move.h"
#include "../../src/ent/thread_control.h"
#include "../../src/impl/config.h"
#include "../../src/impl/exec.h"

#include "../../src/compat/cpthread.h"

#include "../../src/util/io_util.h"
#include "../../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

#define DEFAULT_NAP_TIME 0.01

typedef struct ProcessArgs {
  const char *arg_string;
  int expected_output_line_count;
  const char *output_substr;
  int expected_outerror_line_count;
  const char *outerror_substr;
  bool finished;
  cpthread_mutex_t finished_mutex;
} ProcessArgs;

typedef struct MainArgs {
  int argc;
  char **argv;
} MainArgs;

ProcessArgs *process_args_create(const char *arg_string,
                                 int expected_output_line_count,
                                 const char *output_substr,
                                 int expected_outerror_line_count,
                                 const char *outerror_substr) {
  ProcessArgs *process_args = malloc_or_die(sizeof(ProcessArgs));
  process_args->arg_string = arg_string;
  process_args->expected_output_line_count = expected_output_line_count;
  process_args->output_substr = output_substr;
  process_args->expected_outerror_line_count = expected_outerror_line_count;
  process_args->outerror_substr = outerror_substr;
  process_args->finished = false;
  cpthread_mutex_init(&process_args->finished_mutex);
  return process_args;
}

void process_args_destroy(ProcessArgs *process_args) {
  if (!process_args) {
    return;
  }
  // All strings are owned by the caller
  free(process_args);
}

bool get_process_args_finished(ProcessArgs *process_args) {
  bool finished;
  cpthread_mutex_lock(&process_args->finished_mutex);
  finished = process_args->finished;
  cpthread_mutex_unlock(&process_args->finished_mutex);
  return finished;
}

void set_process_args_finished(ProcessArgs *process_args, bool finished) {
  cpthread_mutex_lock(&process_args->finished_mutex);
  process_args->finished = finished;
  cpthread_mutex_unlock(&process_args->finished_mutex);
}

MainArgs *get_main_args_from_string(const char *arg_string) {
  StringSplitter *arg_string_splitter =
      split_string_by_whitespace(arg_string, true);
  MainArgs *main_args = malloc_or_die(sizeof(MainArgs));
  main_args->argc = string_splitter_get_number_of_items(arg_string_splitter);
  main_args->argv = malloc_or_die(sizeof(char *) * main_args->argc);
  for (int i = 0; i < main_args->argc; i++) {
    main_args->argv[i] = get_formatted_string(
        "%s", string_splitter_get_item(arg_string_splitter, i));
  }
  string_splitter_destroy(arg_string_splitter);
  return main_args;
}

void main_args_destroy(MainArgs *main_args) {
  if (!main_args) {
    return;
  }
  for (int i = 0; i < main_args->argc; i++) {
    free(main_args->argv[i]);
  }
  free(main_args->argv);
  free(main_args);
}

void block_for_search(Config *config, int max_seconds) {
  // Poll for the end of the command
  double seconds_elapsed = 0;
  ThreadControl *thread_control = config_get_thread_control(config);
  while (true) {
    if (thread_control_is_finished(thread_control)) {
      break;
    }
    ctime_nap(DEFAULT_NAP_TIME);
    seconds_elapsed += DEFAULT_NAP_TIME;
    if (seconds_elapsed >= (double)max_seconds) {
      log_fatal("Test aborted after searching for %d seconds\n", max_seconds);
    }
  }
}

void block_for_process_command(ProcessArgs *process_args, int max_seconds) {
  // Poll for the end of the process
  double seconds_elapsed = 0;
  while (1) {
    if (get_process_args_finished(process_args)) {
      break;
    }
    ctime_nap(DEFAULT_NAP_TIME);
    seconds_elapsed += DEFAULT_NAP_TIME;
    if (seconds_elapsed >= (double)max_seconds) {
      log_fatal("Test aborted after processing for %d seconds\n", max_seconds);
    }
  }
}

void assert_command_status_and_output(Config *config, const char *command,
                                      bool should_exit, int seconds_to_wait,
                                      int expected_output_line_count,
                                      int expected_outerror_line_count) {
  char *test_output_filename = get_test_filename("output");
  char *test_outerror_filename = get_test_filename("outerror");

  // Reset the contents of output
  fclose_or_die(fopen_or_die(test_output_filename, "w"));

  FILE *output_fh = fopen_or_die(test_output_filename, "w");
  FILE *errorout_fh = fopen_or_die(test_outerror_filename, "w");

  io_set_stream_out(output_fh);
  io_set_stream_err(errorout_fh);

  ErrorStack *error_stack = error_stack_create();

  execute_command_async(config, error_stack, command);

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
  }

  // Let the async command start up
  ctime_nap(DEFAULT_NAP_TIME);

  if (should_exit) {
    char *status_string = command_search_status(config, true);
    free(status_string);
  }
  block_for_search(config, seconds_to_wait);

  fclose_or_die(errorout_fh);

  char *test_output = get_string_from_file_or_die(test_output_filename);
  int newlines_in_output = count_newlines(test_output);
  bool fail_test = false;
  if (newlines_in_output != expected_output_line_count) {
    printf("%s\nassert output: output counts do not match %d != %d\n", command,
           newlines_in_output, expected_output_line_count);
    printf("got:>%s<\n", test_output);
    fail_test = true;
  }

  char *test_outerror = get_string_from_file_or_die(test_outerror_filename);
  int newlines_in_outerror = count_newlines(test_outerror);
  if (newlines_in_outerror != expected_outerror_line_count) {
    printf(
        "assert output: error counts do not match %d != %d\nfor command: %s\n",
        newlines_in_outerror, expected_outerror_line_count, command);
    printf("got: >%s<\n", test_outerror);
    fail_test = true;
  }

  if (fail_test) {
    abort();
  }

  free(test_output);
  free(test_outerror);
  free(test_output_filename);
  free(test_outerror_filename);
  io_reset_stream_out();
  io_reset_stream_err();
  error_stack_destroy(error_stack);
}

void test_command_execution(void) {
  Config *config = config_create_default_test();

  assert_command_status_and_output(config, "sim -lex CSW21 -it 1000 -plies 2h3",
                                   false, 5, 0, 2);

  assert_command_status_and_output(
      config,
      "cgp 15/15/15/15/15/15/15/15/3ABCDEFG5/15/15/15/15/15/15 "
      "ABC5DF/YXZ 0/0 0 -lex CSW21",
      false, 5, 1, 1);

  // Test load cgp
  assert_command_status_and_output(config, "cgp " ION_OPENING_CGP, false, 5, 1,
                                   0);

  // Sim finishing probabilistically
  // Get moves from just user input
  assert_command_status_and_output(config, "cgp " ZILLION_OPENING_CGP, false, 5,
                                   1, 0);
  assert_command_status_and_output(
      config, "addmoves 8F.LIN,8D.ZILLION,8F.ZILLION", false, 5, 1, 0);

  const MoveList *ml = config_get_move_list(config);
  assert(move_list_get_count(ml) == 3);

  assert_command_status_and_output(
      config,
      "sim -plies 2 -scond 95 -threads 8 -it 100000 -minp 50 -pfreq 5000000",
      false, 60, 6, 0);

  assert(move_list_get_count(ml) == 3);

  assert_command_status_and_output(config, "cgp " ZILLION_OPENING_CGP, false, 5,
                                   1, 0);
  // Confirm that loading a cgp resets the movelist
  assert(move_list_get_count(ml) == 0);

  // Add moves before generating to confirm that the gen command
  // resets the movelist
  assert_command_status_and_output(
      config, "addmoves 8f.NIL,8F.LIN,8D.ZILLION,8F.ZILLION", false, 5, 1, 0);

  assert_command_status_and_output(config, "cgp " ZILLION_OPENING_CGP, false, 5,
                                   1, 0);

  // Sim a single iterations
  // Get 18 moves from move gen and confirm movelist was reset.
  assert_command_status_and_output(config, "gen -numplays 18 ", false, 5, 20,
                                   0);
  // Add 4 more moves:
  // 2 already exist
  // 2 are new
  // To get 20 total moves
  assert_command_status_and_output(
      config, "addmoves 8f.NIL,8F.LIN,8D.ZILLION,8F.ZILLION", false, 5, 1, 0);
  assert_command_status_and_output(
      config, "sim -plies 2 -scond none -threads 8 -it 1 -pfreq 70", false, 60,
      331, 0);

  // Sim finishes with max iterations
  // Add user input moves that will be
  // cleared by the subsequent movegen command.
  assert_command_status_and_output(config, "addmoves ex.SOI,ex.IO,ex.S", false,
                                   5, 1, 0);
  assert_command_status_and_output(config, "cgp " DELDAR_VS_HARSHAN_CGP, false,
                                   5, 1, 0);
  // Get all moves through move gen
  assert_command_status_and_output(config, "gen -numplays 15", false, 5, 17, 0);
  assert_command_status_and_output(
      config, "sim -plies 2 -threads 10 -it 200 -pfreq 60 -scond none ", false,
      60, 222, 0);

  assert_command_status_and_output(config, "cgp " DELDAR_VS_HARSHAN_CGP, false,
                                   5, 1, 0);
  // Sim interrupted by user
  assert_command_status_and_output(config, "gen -numplays 15", false, 5, 17, 0);
  assert_command_status_and_output(
      config, "sim -plies 2 -threads 10 -it 1000000 -pfreq 1000000", true, 5,
      18, 0);

  // Infer finishes normally
  assert_command_status_and_output(config, "cgp " EMPTY_CGP, false, 5, 1, 0);
  assert_command_status_and_output(
      config, "infer 1 MUZAKY 58 -numplays 20 -threads 4 ", false, 60, 53, 0);

  // Infer interrupted
  assert_command_status_and_output(config, "cgp " EMPTY_CGP, false, 5, 1, 0);
  assert_command_status_and_output(config, "infer 1 3 -numplays 20 -threads 3 ",
                                   true, 5, 2, 0);

  // Autoplay finishes normally
  assert_command_status_and_output(
      config,
      "autoplay game 10 -lex CSW21 -s1 equity -s2 equity "
      "-r1 best -r2 best -numplays 1 -threads 3 -gp true",
      false, 30, 3, 0);

  // Autoplay interrupted
  assert_command_status_and_output(
      config,
      "autoplay game 10000000 -lex CSW21 -s1 equity -s2 equity "
      "-r1 best -r2 best -threads 5 -hr false -gp false",
      true, 5, 2, 0);

  assert_command_status_and_output(
      config,
      "autoplay game 10 -lex CSW21 -s1 equity -s2 equity "
      "-r1 best -r2 best -threads 1 -hr false -gp true -pfreq 4",
      false, 30, 8, 0);

  assert_command_status_and_output(
      config,
      "autoplay game 10 -lex CSW21 -s1 equity -s2 equity "
      "-r1 best -r2 best -threads 1 -hr true -gp false -pfreq 0",
      false, 30, 21, 0);

  assert_command_status_and_output(
      config,
      "autoplay game 50 -l1 CSW21 -l2 NWL20 -s1 equity -s2 equity "
      "-r1 best -r2 best -threads 1 -hr true -gp true",
      false, 30, 41, 0);

  // Catalan
  assert_command_status_and_output(config, "cgp " CATALAN_CGP, false, 5, 1, 0);
  assert_command_status_and_output(config, "gen -r1 all -r2 all -numplays 15",
                                   false, 5, 17, 0);
  assert_command_status_and_output(
      config, "sim -plies 2 -threads 10 -it 200 -pfreq 60 -scond none ", false,
      60, 222, 0);
  assert_command_status_and_output(config, "cgp " EMPTY_CATALAN_CGP, false, 5,
                                   1, 0);
  assert_command_status_and_output(
      config, "infer 1 AIMSX 52 -numplays 20 -threads 4 -pfreq 1000000", false,
      60, 53, 0);

  assert_command_status_and_output(
      config,
      "autoplay game 10 -s1 equity -s2 equity -r1 "
      "best -r2 best -numplays 1  -hr false -gp false ",
      false, 30, 2, 0);
  // CSW
  assert_command_status_and_output(config, "cgp " DELDAR_VS_HARSHAN_CGP, false,
                                   5, 1, 0);
  assert_command_status_and_output(config, "gen -r1 all -r2 all -numplays 15",
                                   false, 5, 17, 0);
  assert_command_status_and_output(
      config, "sim -plies 2 -threads 10 -it 200 -pfreq 60 -scond none ", false,
      60, 222, 0);

  assert_command_status_and_output(config, "cgp " EMPTY_CGP, false, 5, 1, 0);
  assert_command_status_and_output(
      config, "infer 1 DGINR 18 -numplays 20 -threads 4 -pfreq 1000000 ", false,
      60, 53, 0);

  assert_command_status_and_output(
      config,
      "autoplay game 10 -lex CSW21 -s1 equity -s2 equity "
      "-r1 best -r2 best -numplays 1 -gp false ",
      false, 30, 2, 0);
  // Polish
  assert_command_status_and_output(config, "cgp " POLISH_CGP, false, 5, 1, 0);
  assert_command_status_and_output(config, "gen -r1 all -r2 all -numplays 15",
                                   false, 5, 17, 0);
  assert_command_status_and_output(
      config, "sim -plies 2 -threads 10 -it 200 -pfreq 60 -scond none ", false,
      60, 222, 0);

  assert_command_status_and_output(config, "cgp " EMPTY_POLISH_CGP, false, 5, 1,
                                   0);
  assert_command_status_and_output(config,
                                   "infer 1 HUJA 20 -numplays 20 -pfreq "
                                   "1000000 -threads 4",
                                   false, 60, 59, 0);

  assert_command_status_and_output(
      config,
      "autoplay game 10 -s1 equity -s2 equity -r1 best "
      "-r2 best -numplays 1 -lex OSPS49 -hr false -gp false",
      false, 30, 2, 0);
  config_destroy(config);
}

void test_process_command(const char *arg_string,
                          int expected_output_line_count,
                          const char *output_substr,
                          int expected_outerror_line_count,
                          const char *outerror_substr) {

  char *test_output_filename = get_test_filename("output");
  char *test_outerror_filename = get_test_filename("outerror");

  char *arg_string_with_exec =
      get_formatted_string("./bin/magpie %s", arg_string);

  // Reset the contents of output
  fclose_or_die(fopen_or_die(test_output_filename, "w"));

  FILE *output_fh = fopen_or_die(test_output_filename, "w");
  FILE *errorout_fh = fopen_or_die(test_outerror_filename, "w");

  io_set_stream_out(output_fh);
  io_set_stream_err(errorout_fh);

  MainArgs *main_args = get_main_args_from_string(arg_string_with_exec);

  process_command_with_data_paths(main_args->argc, main_args->argv,
                                  DEFAULT_TEST_DATA_PATH);
  main_args_destroy(main_args);

  char *test_output = get_string_from_file_or_die(test_output_filename);
  char *test_outerror = get_string_from_file_or_die(test_outerror_filename);

  if (!has_substring(test_output, output_substr)) {
    printf("pattern not found in output:\n%s\n***\n%s\n", test_output,
           output_substr);
    assert(0);
  }

  int newlines_in_output = count_newlines(test_output);
  if (newlines_in_output != expected_output_line_count) {
    printf("test process command: counts do not match %d != %d\n",
           newlines_in_output, expected_output_line_count);
    printf("got:\n%s\n", test_output);
    assert(0);
  }

  if (!has_substring(test_outerror, outerror_substr)) {
    printf("pattern not found in error:\n%s\n***\n%s\n", test_outerror,
           outerror_substr);
    assert(0);
  }

  int newlines_in_outerror = count_newlines(test_outerror);
  if (newlines_in_outerror != expected_outerror_line_count) {
    printf("test process command: error counts do not match %d != %d\nfor "
           "command: %s\n",
           newlines_in_outerror, expected_outerror_line_count, arg_string);
    printf("got:\n%s\n", test_outerror);
    assert(0);
  }

  free(test_output);
  free(test_outerror);
  delete_file(test_output_filename);
  delete_file(test_outerror_filename);
  free(test_output_filename);
  free(test_outerror_filename);
  free(arg_string_with_exec);
  io_reset_stream_out();
  io_reset_stream_err();
}

void test_exec_single_command(void) {
  char *plies_error_substr = get_formatted_string(
      "error %d", ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  test_process_command("sim -lex CSW21 -it 10 -plies 2h3", 0, NULL, 2,
                       plies_error_substr);
  free(plies_error_substr);

  test_process_command("infer 1 MUZAKY 58 -numplays 20 -threads 4 -lex CSW21",
                       53, "infertile leave Z", 0, NULL);
}

void *test_process_command_async(void *uncasted_process_args) {
  ProcessArgs *process_args = (ProcessArgs *)uncasted_process_args;
  test_process_command(
      process_args->arg_string, process_args->expected_output_line_count,
      process_args->output_substr, process_args->expected_outerror_line_count,
      process_args->outerror_substr);
  set_process_args_finished(process_args, true);
  return NULL;
}

void test_exec_ucgi_command(void) {
  char *test_input_filename = get_test_filename("input");

  // Reset the contents of input
  unlink(test_input_filename);

  // Create the FIFO if it doesn't exist
  if (mkfifo(test_input_filename, 0666) == -1) {
    perror("mkfifo");
  }

  FILE *input_writer = fopen_or_die(test_input_filename, "w+");
  FILE *input_reader = fopen_or_die(test_input_filename, "r");
  io_set_stream_in(input_reader);

  ProcessArgs *process_args =
      process_args_create("set -mode ucgi", 6, "autoplay", 1, "still running");

  cpthread_t cmd_execution_thread;
  cpthread_create(&cmd_execution_thread, test_process_command_async,
                  process_args);
  cpthread_detach(cmd_execution_thread);

  ctime_nap(1.0);
  fprintf_or_die(input_writer,
                 "set -r1 best -r2 best -it 1 -numplays 1 -threads 1\n");
  fflush_or_die(input_writer);
  ctime_nap(1.0);
  fprintf_or_die(
      input_writer,
      "autoplay game 1 -lex CSW21 -s1 equity -s2 equity -gp false\n");
  fflush_or_die(input_writer);
  ctime_nap(1.0);
  fprintf_or_die(
      input_writer,
      "autoplay game 10000000 -lex CSW21 -s1 equity -s2 equity  -gp false\n");
  fflush_or_die(input_writer);
  // Try to immediately start another command while the previous one
  // is still running. This should give a warning.
  fprintf_or_die(
      input_writer,
      "autoplay game 1 -lex CSW21 -s1 equity -s2 equity  -gp false\n");
  fflush_or_die(input_writer);
  ctime_nap(1.0);
  // Interrupt the autoplay which won't finish in 1 second
  fprintf_or_die(input_writer, "stop\n");
  fflush_or_die(input_writer);
  ctime_nap(1.0);
  fprintf_or_die(input_writer, "quit\n");
  fflush_or_die(input_writer);
  ctime_nap(1.0);

  // Wait for magpie to quit
  block_for_process_command(process_args, 5);

  fclose_or_die(input_writer);
  fclose_or_die(input_reader);
  delete_fifo(test_input_filename);
  process_args_destroy(process_args);
  free(test_input_filename);
  io_reset_stream_in();
}

void test_exec_console_command(void) {
  char *test_input_filename = get_test_filename("input");

  // Reset the contents of input
  unlink(test_input_filename);

  FILE *input_writer = fopen_or_die(test_input_filename, "w+");
  FILE *input_reader = fopen_or_die(test_input_filename, "r");
  io_set_stream_in(input_reader);

  char *initial_command = get_formatted_string("cgp %s", EMPTY_CGP);

  char *config_load_error_substr = get_formatted_string(
      "error %d", ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG);

  ProcessArgs *process_args = process_args_create(
      initial_command, 45, "autoplay games 20", 1, config_load_error_substr);

  cpthread_t cmd_execution_thread;
  cpthread_create(&cmd_execution_thread, test_process_command_async,
                  process_args);
  cpthread_detach(cmd_execution_thread);

  write_to_stream(input_writer,
                  "infer 1 DGINR 18 -numplays 7 -threads 4 -pfreq 1000000\n");
  write_to_stream(input_writer, "set -r1 best -r2 b -nump 1 -threads 4\n");
  write_to_stream(
      input_writer,
      "autoplay game 10 -lex CSW21 -s1 equity -s2 equity -gp true \n");
  // Stop should have no effect and appear as an error
  write_to_stream(input_writer, "stop\n");
  write_to_stream(input_writer, "quit\n");

  // Wait for magpie to quit
  block_for_process_command(process_args, 30);

  fclose_or_die(input_writer);
  fclose_or_die(input_reader);
  delete_fifo(test_input_filename);
  process_args_destroy(process_args);
  free(config_load_error_substr);
  free(test_input_filename);
  free(initial_command);
  io_reset_stream_in();
}

void test_command(void) {
  test_exec_single_command();
  test_command_execution();
  test_exec_ucgi_command();
  test_exec_console_command();
  io_reset_stream_out();
  io_reset_stream_err();
  io_reset_stream_in();
}