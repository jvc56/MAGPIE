#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/command.h"
#include "../src/game.h"
#include "../src/infer.h"
#include "../src/log.h"
#include "../src/sim.h"
#include "../src/thread_control.h"
#include "../src/util.h"

#include "test_constants.h"
#include "test_util.h"

typedef struct ProcessArgs {
  const char *arg_string;
  int expected_output_line_count;
  const char *output_substr;
  int expected_outerror_line_count;
  const char *outerror_substr;
  bool finished;
  pthread_mutex_t finished_mutex;
} ProcessArgs;

typedef struct MainArgs {
  int argc;
  char **argv;
} MainArgs;

ProcessArgs *create_process_args(const char *arg_string,
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
  pthread_mutex_init(&process_args->finished_mutex, NULL);
  return process_args;
}

void destroy_process_args(ProcessArgs *process_args) {
  // All strings are owned by the caller
  free(process_args);
}

bool get_process_args_finished(ProcessArgs *process_args) {
  bool finished;
  pthread_mutex_lock(&process_args->finished_mutex);
  finished = process_args->finished;
  pthread_mutex_unlock(&process_args->finished_mutex);
  return finished;
}

void set_process_args_finished(ProcessArgs *process_args, bool finished) {
  pthread_mutex_lock(&process_args->finished_mutex);
  process_args->finished = finished;
  pthread_mutex_unlock(&process_args->finished_mutex);
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
  destroy_string_splitter(arg_string_splitter);
  return main_args;
}

void destroy_main_args(MainArgs *main_args) {
  for (int i = 0; i < main_args->argc; i++) {
    free(main_args->argv[i]);
  }
  free(main_args->argv);
  free(main_args);
}

void block_for_search(CommandVars *command_vars, int max_seconds) {
  // Poll for the end of the command
  int seconds_elapsed = 0;
  while (1) {
    if (get_mode(command_vars->config->thread_control) == MODE_STOPPED) {
      break;
    } else {
      sleep(1);
    }
    seconds_elapsed++;
    if (seconds_elapsed >= max_seconds) {
      log_fatal("Test aborted after searching for %d seconds", max_seconds);
    }
  }
}

void block_for_process_command(ProcessArgs *process_args, int max_seconds) {
  // Poll for the end of the process
  int seconds_elapsed = 0;
  while (1) {
    if (get_process_args_finished(process_args)) {
      break;
    } else {
      sleep(1);
    }
    seconds_elapsed++;
    printf("%d seconds elapsed\n", seconds_elapsed);
    if (seconds_elapsed >= max_seconds) {
      log_fatal("Test aborted after processing for %d seconds", max_seconds);
    }
  }
}

void assert_command_status_and_output(CommandVars *command_vars,
                                      const char *command_without_io,
                                      bool should_halt, int seconds_to_wait,
                                      error_status_t expected_error_status_type,
                                      int expected_error_code,
                                      int expected_output_line_count,
                                      int expected_outerror_line_count) {
  char *test_output_filename = get_test_filename("output");
  char *test_outerror_filename = get_test_filename("outerror");

  char *command = get_formatted_string("%s outfile %s", command_without_io,
                                       test_output_filename);

  // Reset the contents of output
  fclose(fopen(test_output_filename, "w"));

  FILE *errorout_fh = fopen(test_outerror_filename, "w");

  log_set_error_out(errorout_fh);

  execute_command_async(command_vars, command);
  if (should_halt) {
    // If halting, let the search start
    sleep(2);
    halt(command_vars->config->thread_control, HALT_STATUS_USER_INTERRUPT);
  }
  block_for_search(command_vars, seconds_to_wait);

  fclose(errorout_fh);

  if (command_vars->error_status->type != expected_error_status_type) {
    printf("expected error status != actual error status (%d != %d)\n",
           expected_error_status_type, command_vars->error_status->type);
  }
  if (command_vars->error_status->code != expected_error_code) {
    printf("expected error code != actual error code (%d != %d)\n",
           expected_error_code, command_vars->error_status->code);
  }
  assert(command_vars->error_status->type == expected_error_status_type);
  assert(command_vars->error_status->code == expected_error_code);
  assert(get_mode(command_vars->config->thread_control) == MODE_STOPPED);

  char *test_output = get_string_from_file(test_output_filename);
  printf("output for %s:\n%s\n", command, test_output);
  int newlines_in_output = count_newlines(test_output);
  if (newlines_in_output != expected_output_line_count) {
    printf("output counts do not match %d != %d\n", newlines_in_output,
           expected_output_line_count);
    assert(0);
  }

  char *test_outerror = get_string_from_file(test_outerror_filename);
  int newlines_in_outerror = count_newlines(test_outerror);
  if (newlines_in_outerror != expected_outerror_line_count) {
    printf("error counts do not match %d != %d\n", newlines_in_outerror,
           expected_outerror_line_count);
    assert(0);
  }

  free(test_output);
  free(test_outerror);
  free(test_output_filename);
  free(test_outerror_filename);
  free(command);
}

void test_command_execution() {
  CommandVars *command_vars = create_command_vars();

  assert_command_status_and_output(
      command_vars, "go sim lex CSW21 i 1000 plies 2h3", false, 5,
      ERROR_STATUS_TYPE_CONFIG_LOAD, (int)CONFIG_LOAD_STATUS_MALFORMED_PLIES, 0,
      1);

  assert_command_status_and_output(
      command_vars,
      "position cgp 15/15/15/15/15/15/15/15/3ABCDEFG5/15/15/15/15/15/15 "
      "ABC5DF/YXZ 0/0 0 lex CSW21",
      false, 5, ERROR_STATUS_TYPE_CGP_LOAD,
      (int)CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS, 0, 1);

  // Test load cgp
  assert_command_status_and_output(command_vars,
                                   "position cgp " ION_OPENING_CGP, false, 5,
                                   ERROR_STATUS_TYPE_NONE, 0, 0, 0);

  // Sim finishing probabilistically
  assert_command_status_and_output(
      command_vars,
      "go sim plies 2 stop 95 threads 8 numplays 3 i 100000 check 300 "
      "info 500 cgp " ZILLION_OPENING_CGP,
      false, 60, ERROR_STATUS_TYPE_NONE, 0, 5, 0);

  // Sim statically
  assert_command_status_and_output(
      command_vars,
      "go sim plies 2 stop 95 threads 8 numplays 20 i 100000 check 300 "
      "info 70 static cgp " ZILLION_OPENING_CGP,
      false, 60, ERROR_STATUS_TYPE_NONE, 0, 21, 0);

  // Sim finishes with max iterations
  assert_command_status_and_output(
      command_vars,
      "go sim plies 2 threads 10 numplays 15 i "
      "200 info 60 nostatic cgp " DELDAR_VS_HARSHAN_CGP,
      false, 60, ERROR_STATUS_TYPE_NONE, 0, 68, 0);

  // Sim interrupted by user
  assert_command_status_and_output(
      command_vars,
      "go sim plies 2 threads 10 numplays 15 i "
      "1000000 info 1000000 cgp " DELDAR_VS_HARSHAN_CGP,
      true, 5, ERROR_STATUS_TYPE_NONE, 0, 17, 0);

  // Infer finishes normally
  assert_command_status_and_output(
      command_vars,
      "go infer rack MUZAKY pindex 0 score 58 exch 0 numplays 20 threads 4 "
      "cgp " EMPTY_CGP,
      false, 60, ERROR_STATUS_TYPE_NONE, 0, 52, 0);

  // Infer interrupted
  assert_command_status_and_output(
      command_vars,
      "go infer rack " EMPTY_RACK_STRING
      " pindex 0 score 0 exch 3 numplays 20 threads 3 "
      "cgp " EMPTY_CGP,
      true, 5, ERROR_STATUS_TYPE_NONE, 0, 1, 0);

  // Autoplay finishes normally
  assert_command_status_and_output(command_vars,
                                   "go autoplay lex CSW21 s1 equity s2 equity "
                                   "r1 best r2 best i 10 numplays 1 threads 3",
                                   false, 30, ERROR_STATUS_TYPE_NONE, 0, 1, 0);

  // Autoplay interrupted
  assert_command_status_and_output(command_vars,
                                   "go autoplay lex CSW21 s1 equity s2 equity "
                                   "r1 best r2 best i 10000000 threads 5",
                                   true, 5, ERROR_STATUS_TYPE_NONE, 0, 1, 0);

  for (int i = 0; i < 3; i++) {
    // Catalan
    assert_command_status_and_output(command_vars, "position cgp " CATALAN_CGP,
                                     false, 5, ERROR_STATUS_TYPE_NONE, 0, 0, 0);
    assert_command_status_and_output(command_vars,
                                     "go sim plies 2 threads 10 numplays 15 i "
                                     "200 info 60 cgp " CATALAN_CGP,
                                     false, 60, ERROR_STATUS_TYPE_NONE, 0, 68,
                                     0);
    assert_command_status_and_output(
        command_vars,
        "go infer rack AIMSX pindex 0 score 52 exch "
        "0 numplays 20 threads 4 info 1000000 cgp " EMPTY_CATALAN_CGP,
        false, 60, ERROR_STATUS_TYPE_NONE, 0, 52, 0);

    assert_command_status_and_output(
        command_vars,
        "go autoplay s1 equity s2 equity "
        "r1 best r2 best i 10 numplays 1 cgp " CATALAN_CGP,
        false, 30, ERROR_STATUS_TYPE_NONE, 0, 1, 0);
    // CSW
    assert_command_status_and_output(command_vars,
                                     "position cgp " ION_OPENING_CGP, false, 5,
                                     ERROR_STATUS_TYPE_NONE, 0, 0, 0);

    assert_command_status_and_output(command_vars,
                                     "go sim plies 2 threads 10 numplays 15 i "
                                     "200 info 60 cgp " DELDAR_VS_HARSHAN_CGP,
                                     false, 60, ERROR_STATUS_TYPE_NONE, 0, 68,
                                     0);

    assert_command_status_and_output(
        command_vars,
        "go infer rack DGINR pindex 0 score 18 exch 0 numplays 20 threads 4 "
        "info 1000000 "
        "cgp " EMPTY_CGP,
        false, 60, ERROR_STATUS_TYPE_NONE, 0, 52, 0);

    assert_command_status_and_output(
        command_vars,
        "go autoplay lex CSW21 s1 equity s2 equity "
        "r1 best r2 best i 10 numplays 1",
        false, 30, ERROR_STATUS_TYPE_NONE, 0, 1, 0);
    // Polish
    assert_command_status_and_output(command_vars, "position cgp " POLISH_CGP,
                                     false, 5, ERROR_STATUS_TYPE_NONE, 0, 0, 0);

    assert_command_status_and_output(command_vars,
                                     "go sim plies 2 threads 10 numplays 15 i "
                                     "200 info 60 cgp " POLISH_CGP,
                                     false, 60, ERROR_STATUS_TYPE_NONE, 0, 68,
                                     0);

    assert_command_status_and_output(
        command_vars,
        "go infer rack HUJA pindex 0 score 20 exch 0 "
        "numplays 20 info 1000000  threads 4 cgp " EMPTY_POLISH_CGP,
        false, 60, ERROR_STATUS_TYPE_NONE, 0, 58, 0);

    assert_command_status_and_output(
        command_vars,
        "go autoplay s1 equity s2 equity "
        "r1 best r2 best i 10 numplays 1 lex OSPS44",
        false, 30, ERROR_STATUS_TYPE_NONE, 0, 1, 0);
  }
  destroy_command_vars(command_vars);
}

void test_process_command(const char *arg_string,
                          int expected_output_line_count,
                          const char *output_substr,
                          int expected_outerror_line_count,
                          const char *outerror_substr) {

  char *test_output_filename = get_test_filename("output");
  char *test_outerror_filename = get_test_filename("outerror");

  char *arg_string_with_outfile = get_formatted_string(
      "./bin/magpie %s outfile %s", arg_string, test_output_filename);

  // Reset the contents of output
  fclose(fopen(test_output_filename, "w"));

  FILE *errorout_fh = fopen(test_outerror_filename, "w");

  log_set_error_out(errorout_fh);

  MainArgs *main_args = get_main_args_from_string(arg_string_with_outfile);

  process_command(main_args->argc, main_args->argv);
  destroy_main_args(main_args);

  char *test_output = get_string_from_file(test_output_filename);
  char *test_outerror = get_string_from_file(test_outerror_filename);

  printf("test out:\n%s\n", test_output);
  printf("error out:\n%s\n", test_outerror);

  if (!has_substring(test_output, output_substr)) {
    printf("pattern not found in output:\n%s\n***\n%s\n", test_output,
           output_substr);
    assert(0);
  }

  int newlines_in_output = count_newlines(test_output);
  if (newlines_in_output != expected_output_line_count) {
    printf("counts do not match %d != %d\n", newlines_in_output,
           expected_output_line_count);
    assert(0);
  }

  if (!has_substring(test_outerror, outerror_substr)) {
    printf("pattern not found in error:\n%s\n***\n%s\n", test_outerror,
           outerror_substr);
    assert(0);
  }

  int newlines_in_outerror = count_newlines(test_outerror);
  if (newlines_in_outerror != expected_outerror_line_count) {
    printf("error counts do not match %d != %d\n", newlines_in_outerror,
           expected_outerror_line_count);
    assert(0);
  }

  free(test_output);
  free(test_outerror);
  free(test_output_filename);
  free(test_outerror_filename);
  free(arg_string_with_outfile);
}

void test_exec_single_command() {
  char *plies_error_substr =
      get_formatted_string("code %d", CONFIG_LOAD_STATUS_MALFORMED_PLIES);
  test_process_command("go sim lex CSW21 i 1000 plies 2h3", 0, NULL, 1,
                       plies_error_substr);
  free(plies_error_substr);

  char *rack_error_substr =
      get_formatted_string("code %d", CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS);
  test_process_command(
      "position cgp 15/15/15/15/15/15/15/15/3ABCDEFG5/15/15/15/15/15/15 "
      "ABC5DF/YXZ 0/0 0 lex CSW21",
      0, NULL, 1, rack_error_substr);
  free(rack_error_substr);

  test_process_command("go infer rack MUZAKY pindex 0 score 58 exch 0 numplays "
                       "20 threads 4 lex CSW21",
                       52, "infertile leave Z", 0, NULL);
}

void test_exec_file_commands() {
  // Run a sim in CSW, then (68 output)
  // run a sim that exits with a warning, then (1 warning)
  // run an inference in Polish, then (58 output)
  // run autoplay in CSW (1 output)
  // total output = 127
  // total error = 1

  // Separate into distinct lines to prove
  // the command file is being read.
  const char *commands_file_content =
      "i 200\n"
      "info 60\n"
      "cgp " DELDAR_VS_HARSHAN_CGP "\ngo sim plies 2 threads 10 numplays 15\n"
      "go sim lex CSW21 i 10h00\n"
      "pindex 0 score 20 exch 0\n"
      "numplays 20 info 1000000\n"
      " threads 4 cgp " EMPTY_POLISH_CGP "\ngo infer rack HUJA\n"
      "r1 best r2 best i 10 numplays 1 threads 3\n"
      "go autoplay lex CSW21 s1 equity s2 equity ";
  char *commands_filename = get_test_filename("test_commands");

  write_string_to_file(commands_filename, "w", commands_file_content);

  char *commands_file_invocation =
      get_formatted_string("infile %s", commands_filename);

  char *iter_error_substr = get_formatted_string(
      "code %d", CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS);

  test_process_command(commands_file_invocation, 127,
                       "info infertotalracks 6145", 1, iter_error_substr);

  free(iter_error_substr);
  free(commands_filename);
  free(commands_file_invocation);
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

void test_exec_ucgi_command() {
  char *test_input_filename = get_test_filename("input");

  create_fifo(test_input_filename);

  char *initial_command =
      get_formatted_string("ucgi infile %s", test_input_filename);
  ProcessArgs *process_args =
      create_process_args(initial_command, 1, "autoplay", 0, "");

  pthread_t cmd_execution_thread;
  pthread_create(&cmd_execution_thread, NULL, test_process_command_async,
                 process_args);
  pthread_detach(cmd_execution_thread);

  FileHandler *input_writer = create_file_handler_from_filename(
      test_input_filename, FILE_HANDLER_MODE_WRITE);

  sleep(1);
  write_to_file(input_writer, "r1 best r2 best i 10 numplays 1 threads 1\n");
  sleep(1);
  write_to_file(input_writer, "go autoplay lex CSW21 s1 equity s2 equity\n");
  sleep(1);
  write_to_file(input_writer, "quit\n");
  sleep(1);

  // Wait for magpie to quit
  block_for_process_command(process_args, 5);

  destroy_file_handler(input_writer);
  delete_fifo(test_input_filename);
  destroy_process_args(process_args);
  free(test_input_filename);
  free(initial_command);
}

void test_exec_console_command() {}

void test_command() {
  // test_command_execution();
  // test_exec_single_command();
  test_exec_file_commands();
  // test_exec_console_command();
  test_exec_ucgi_command();
}