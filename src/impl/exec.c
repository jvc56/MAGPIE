#include "exec.h"

#include "../compat/cfile.h"
#include "../compat/cpath.h"
#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/cterm.h"
#include "../compat/linenoise.h"
#include "../def/config_defs.h"
#include "../def/exec_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/thread_control.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "command_input.h"
#include "config.h"
#include "data_fetch.h"
#include "move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *command_search_status(Config *config, bool should_exit) {
  if (!config) {
    log_fatal("config is unexpectedly null");
    // unreachable, but to silence static analyzer warnings
    return NULL;
  }

  ThreadControl *thread_control = config_get_thread_control(config);

  if (should_exit) {
    thread_control_stop(thread_control);
  }

  return config_get_execute_status(config);
}

void execute_command_and_set_status_finished(Config *config,
                                             ErrorStack *error_stack) {
  config_execute_command(config, error_stack);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);
  error_stack_print_and_reset(error_stack);
}

void *execute_async_command_worker(void *uncasted_args) {
  Config *config = (Config *)uncasted_args;
  // Create another error stack so this asynchronous command doesn't
  // interfere with the synchronous error stack on the main thread.
  ErrorStack *error_stack_async = error_stack_create();
  execute_command_and_set_status_finished(config, error_stack_async);
  error_stack_destroy(error_stack_async);
  // The command loop waits on input until the command is no longer running.
  command_input_wake();
  return NULL;
}

bool load_command_sync(Config *config, ErrorStack *error_stack,
                       const char *command) {
  ThreadControl *thread_control = config_get_thread_control(config);
  const bool reset_result =
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  assert(reset_result);
  (void)reset_result; // Suppress unused warning when NDEBUG is defined
  // Loading the config should always be done synchronously and then start the
  // execution asynchronously (if enabled)
  config_load_command(config, command, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    thread_control_set_status(config_get_thread_control(config),
                              THREAD_CONTROL_STATUS_FINISHED);
    return false;
  }

  return true;
}

void execute_command_sync(Config *config, ErrorStack *error_stack,
                          const char *command) {
  if (load_command_sync(config, error_stack, command)) {
    execute_command_and_set_status_finished(config, error_stack);
  }
}

bool run_str_api_command(Config *config, ErrorStack *error_stack,
                         const char *command, char **output) {
  if (load_command_sync(config, error_stack, command)) {
    return config_run_str_api_command(config, error_stack, output);
  }
  return false;
}

// Returns the async token of the command, or NUMBER_OF_ASYNC_COMMAND_TOKENS
// if the command was not recognized.
// Clears the string builder at the beginning of the function
async_token_t parse_async_command(const char *command, StringBuilder *sb) {
  async_token_t token = NUMBER_OF_ASYNC_COMMAND_TOKENS;
  string_builder_clear(sb);
  for (async_token_t i = 0; i < NUMBER_OF_ASYNC_COMMAND_TOKENS; i++) {
    const char *token_to_eval_string = NULL;
    switch (i) {
    case ASYNC_STOP_COMMAND_TOKEN:
      token_to_eval_string = ASYNC_STOP_COMMAND_STRING;
      break;
    case ASYNC_STATUS_COMMAND_TOKEN:;
      token_to_eval_string = ASYNC_STATUS_COMMAND_STRING;
      break;
    case NUMBER_OF_ASYNC_COMMAND_TOKENS:
      log_fatal(
          "encountered unexpected async command token when processing command");
      break;
    }
    if (has_iprefix(command, token_to_eval_string)) {
      if (token == NUMBER_OF_ASYNC_COMMAND_TOKENS) {
        token = i;
      } else if (string_builder_length(sb) == 0) {
        // This only works because there are two valid async commands, if we add
        // more, this whole scheme will need to be refactored
        string_builder_add_formatted_string(
            sb, "ambiguous async command '%s' could be: %s, %s", command,
            ASYNC_STOP_COMMAND_STRING, ASYNC_STATUS_COMMAND_STRING);
      } else {
        string_builder_add_formatted_string(sb, ", %s", token_to_eval_string);
      }
    }
  }
  return token;
}

// Whether the running async command has stopped running (finished or been
// stopped), for command_input_read_line_until.
static bool async_command_is_done(void *uncasted_thread_control) {
  ThreadControl *thread_control = (ThreadControl *)uncasted_thread_control;
  return thread_control_get_status(thread_control) !=
         THREAD_CONTROL_STATUS_STARTED;
}

// Handles the input that arrives while an async command runs, until it stops
// running: async commands (stop, status) run at once; other lines are
// appended to `queued_commands` when it is non-NULL and rejected otherwise.
static void handle_async_command_input(Config *config,
                                       StringList *queued_commands) {
  ThreadControl *thread_control = config_get_thread_control(config);
  ErrorStack *error_stack = error_stack_create();
  StringBuilder *err_msg_sb = string_builder_create();
  while (true) {
    bool at_end = false;
    char *input = command_input_read_line_until(async_command_is_done,
                                                thread_control, &at_end);
    if (input == NULL) {
      break;
    }
    if (is_string_empty_or_whitespace(input)) {
      free(input);
      continue;
    }
    trim_whitespace(input);

    async_token_t input_token = parse_async_command(input, err_msg_sb);
    if (input_token == NUMBER_OF_ASYNC_COMMAND_TOKENS &&
        string_builder_length(err_msg_sb) == 0 && queued_commands != NULL) {
      string_list_add_string(queued_commands, input);
    } else if (string_builder_length(err_msg_sb) > 0) {
      error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_AMBIGUOUS_COMMAND,
                       string_builder_dump(err_msg_sb, NULL));
      error_stack_print_and_reset(error_stack);
    } else if (input_token == NUMBER_OF_ASYNC_COMMAND_TOKENS) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
          get_formatted_string("unrecognized async command '%s'", input));
      error_stack_print_and_reset(error_stack);
    } else {
      switch (input_token) {
      case ASYNC_STOP_COMMAND_TOKEN:
        thread_control_stop(thread_control);
        break;
      case ASYNC_STATUS_COMMAND_TOKEN:;
        char *status_str = config_get_execute_status(config);
        thread_control_print(thread_control, status_str);
        free(status_str);
        break;
      case NUMBER_OF_ASYNC_COMMAND_TOKENS:
        log_fatal("found unexpected async command token");
        break;
      }
    }
    free(input);
  }
  string_builder_destroy(err_msg_sb);
  error_stack_destroy(error_stack);
}

// Blocks until the async command is finished. Lines other than async
// commands that arrive meanwhile are appended to `queued_commands` when
// it is non-NULL, and rejected with an error otherwise.
static void execute_command_async_queueing(Config *config,
                                           ErrorStack *error_stack,
                                           const char *command,
                                           StringList *queued_commands) {
  if (!load_command_sync(config, error_stack, command)) {
    return;
  }
  cpthread_t cmd_execution_thread;
  cpthread_create(&cmd_execution_thread, execute_async_command_worker, config);
  // Returns when the command stops running or the input ends, whichever is
  // first; the join then waits for the command itself.
  handle_async_command_input(config, queued_commands);
  cpthread_join(cmd_execution_thread);
}

void execute_command_async(Config *config, ErrorStack *error_stack,
                           const char *command) {
  execute_command_async_queueing(config, error_stack, command, NULL);
}

void load_config_settings(Config *config, ErrorStack *error_stack) {
  // if file does not exist, just return
  if (!cfile_access(config_get_settings_filename(config),
                    CFILE_ACCESS_EXISTS)) {
    return;
  }
  char *settings_string =
      get_string_from_file(config_get_settings_filename(config), error_stack);
  StringSplitter *settings_split_by_newline =
      split_string_by_newline(settings_string, true);
  const int num_lines =
      string_splitter_get_number_of_items(settings_split_by_newline);
  for (int i = 0; i < num_lines; i++) {
    char *line = config_upgrade_settings_line(
        config, string_splitter_get_item(settings_split_by_newline, i));
    execute_command_sync(config, error_stack, line);
    free(line);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }
  }
  string_splitter_destroy(settings_split_by_newline);
  free(settings_string);
}

// Runs one line of input from the command loop. Returns false when the
// line asks to terminate. `queued_commands` is passed through to
// execute_command_async_queueing.
static bool run_input_line(Config *config, ErrorStack *error_stack,
                           const char *raw_input, StringList *queued_commands) {
  char *input = string_duplicate(raw_input);
  trim_whitespace(input);
  if (is_string_empty_or_null(input)) {
    free(input);
    return true;
  }
  if (strings_iequal(TERMINATE_KEYWORD, input) ||
      strings_iequal(TERMINATE_KEYWORD_ALIAS_EXIT, input) ||
      strings_iequal(TERMINATE_KEYWORD_ALIAS_SHORT, input)) {
    free(input);
    return false;
  }
  if (strings_iequal(input, ASYNC_STOP_COMMAND_STRING)) {
    error_stack_push(error_stack, ERROR_STATUS_COMMAND_NOTHING_TO_STOP,
                     string_duplicate("no currently running command to stop"));
  } else {
    linenoiseHistoryAdd(input);
    switch (config_get_exec_mode(config)) {
    case EXEC_MODE_SYNC:
      execute_command_sync(config, error_stack, input);
      break;
    case EXEC_MODE_ASYNC:
      execute_command_async_queueing(config, error_stack, input,
                                     queued_commands);
      break;
    case EXEC_MODE_UNKNOWN:
      log_fatal("attempted to execute command in unknown mode");
      break;
    }
  }
  if (error_stack_is_empty(error_stack)) {
    save_config_settings(config, error_stack);
  }
  error_stack_print_and_reset(error_stack);
  free(input);
  return true;
}

// Runs the lines queued while earlier commands were executing, in order,
// including any queued while these run. Returns false when one asks to
// terminate.
static bool run_queued_commands(Config *config, ErrorStack *error_stack,
                                StringList **queued_commands) {
  if (*queued_commands == NULL) {
    return true;
  }
  while (string_list_get_count(*queued_commands) > 0) {
    StringList *batch = *queued_commands;
    *queued_commands = string_list_create();
    const int count = string_list_get_count(batch);
    for (int line_idx = 0; line_idx < count; line_idx++) {
      if (!run_input_line(config, error_stack,
                          string_list_get_string(batch, line_idx),
                          *queued_commands)) {
        string_list_destroy(batch);
        return false;
      }
    }
    string_list_destroy(batch);
  }
  return true;
}

void sync_command_scan_loop(Config *config, ErrorStack *error_stack,
                            const char *initial_command_string) {
  // The settings file is left alone unless the command line asks for it with
  // -savesettings true, so a settings.txt in the working directory cannot
  // change what a run does on its own.
  if (config_command_turns_on_save_settings(config, initial_command_string)) {
    // To suppress 'finished' for internal load settings commands
    config_set_loaded_settings(config, false);
    load_config_settings(config, error_stack);
    config_set_loaded_settings(config, true);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      return;
    }
  }
  execute_command_sync(config, error_stack, initial_command_string);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    return;
  }
  if (!config_continue_on_coldstart(config)) {
    return;
  }
  // At a terminal, a line typed while a command runs is rejected with a
  // warning. From a pipe or file (a script), it is queued and run in
  // order afterwards, since nobody is there to see the warning.
  StringList *queued_commands = NULL;
  if (!cterm_is_terminal(get_stream_in())) {
    queued_commands = string_list_create();
  }
  char *input = NULL;
  linenoiseHistorySetMaxLen(1000);
  while (1) {
    const char *prompt_text = "";
    if (config_get_show_prompt(config)) {
      prompt_text = MAGPIE_PROMPT " ";
    }
    free(input);
    input = command_input_read_line(prompt_text);
    if (!input) {
      // NULL input indicates an EOF
      break;
    }

    if (!run_input_line(config, error_stack, input, queued_commands)) {
      break;
    }
    if (!run_queued_commands(config, error_stack, &queued_commands)) {
      break;
    }
  }
  string_list_destroy(queued_commands);
  free(input);
}

char *create_command_from_args(int argc, char *argv[]) {
  StringBuilder *command_string_builder = string_builder_create();
  for (int i = 1; i < argc; i++) {
    string_builder_add_formatted_string(command_string_builder, "%s ", argv[i]);
  }
  char *command_string =
      string_builder_dump_and_destroy(command_string_builder, NULL);
  return command_string;
}

void caches_destroy(void) {
  gen_destroy_cache();
  fileproxy_destroy_cache();
}

// Returns false if MAGPIE could not start: its configuration, which loads
// the default board layout from the data, failed.
static bool process_command_internal(int argc, char *argv[],
                                     const ConfigArgs *config_args) {
  log_set_level(LOG_FATAL);
  ErrorStack *error_stack = error_stack_create();
  Config *config = config_create(config_args, error_stack);
  const bool started = error_stack_is_empty(error_stack);
  if (started) {
    char *initial_command_string = create_command_from_args(argc, argv);
    sync_command_scan_loop(config, error_stack, initial_command_string);
    free(initial_command_string);
    caches_destroy();
  } else {
    error_stack_print_and_reset(error_stack);
  }
  config_destroy(config);
  error_stack_destroy(error_stack);
  return started;
}

// The data path a run with no -path uses: data in the working directory, as
// always; failing that, data beside the executable (for one run from a
// terminal elsewhere), then the user's own (%LOCALAPPDATA%\MAGPIE\data on
// Windows, ~/.magpie/data on macOS). With none of them there, where `data
// fetch` puts it: the user's own on macOS, where the working directory is
// wherever MAGPIE was started from, and the working directory's elsewhere.
// (Started from Explorer, the working directory is the executable's.)
static char *locate_default_data_path(void) {
  if (data_fetch_get_state(DEFAULT_DATA_PATHS) != DATA_FETCH_STATE_MISSING) {
    return string_duplicate(DEFAULT_DATA_PATHS);
  }
  char *directories[] = {cpath_executable_dir(), cpath_user_data_dir()};
  char *found = NULL;
  for (int directory_idx = 0; directory_idx < 2; directory_idx++) {
    char *directory = directories[directory_idx];
    if (!found && directory) {
      char *candidate = get_formatted_string("%s/data", directory);
      if (data_fetch_get_state(candidate) != DATA_FETCH_STATE_MISSING) {
        found = candidate;
      } else {
        free(candidate);
      }
    }
    free(directory);
  }
  if (found) {
    return found;
  }
  char *user_dir =
      cpath_fetches_to_user_data_dir() ? cpath_user_data_dir() : NULL;
  char *path = user_dir ? get_formatted_string("%s/data", user_dir)
                        : string_duplicate(DEFAULT_DATA_PATHS);
  free(user_dir);
  return path;
}

static void print_data_fetch_progress(void *context, const char *message) {
  (void)context;
  write_to_stream_out("%s\n", message);
}

// Fetches the data into `data_path`, reporting progress. Returns false, having
// printed why, if it failed.
static bool fetch_data(const char *data_path, const char *base_url) {
  ErrorStack *error_stack = error_stack_create();
  data_fetch(base_url, data_path, print_data_fetch_progress, NULL, error_stack);
  const bool fetched = error_stack_is_empty(error_stack);
  error_stack_print_and_reset(error_stack);
  error_stack_destroy(error_stack);
  return fetched;
}

// The first directory of the command line's `-path`, or NULL if it has none.
// Only the full name is recognized: every shorter one ("-p", "-pa", "-pat")
// is ambiguous among the options anyway.
static char *command_line_data_path(int argc, char *argv[]) {
  for (int arg_idx = 1; arg_idx + 1 < argc; arg_idx++) {
    if (strings_equal(argv[arg_idx], "-path")) {
      return cpath_list_first(argv[arg_idx + 1]);
    }
  }
  return NULL;
}

static bool is_data_fetch_command(int argc, char *argv[]) {
  return argc >= 3 && strings_iequal(argv[1], "data") &&
         strings_iequal(argv[2], "fetch");
}

// Runs a command-line `data fetch [<base url>] [-path <dir>]` before there is
// any data to start MAGPIE with, so before the options parser, which needs
// the data. Returns whether the data was fetched, having said why not.
static bool run_data_fetch_command(const char *data_path, int argc,
                                   char *argv[]) {
  const char *base_url = NULL;
  for (int arg_idx = 3; arg_idx < argc; arg_idx++) {
    if (strings_equal(argv[arg_idx], "-path")) {
      if (arg_idx + 1 == argc) {
        write_to_stream_err("-path needs a directory\n");
        return false;
      }
      arg_idx++;
    } else if (argv[arg_idx][0] != '-' && !base_url) {
      base_url = argv[arg_idx];
    } else {
      write_to_stream_err("data fetch takes a base URL and -path; '%s' can "
                          "only be used once the data is there\n",
                          argv[arg_idx]);
      return false;
    }
  }
  if (!data_path[0]) {
    write_to_stream_err("data fetch needs a directory to put the data in\n");
    return false;
  }
  return fetch_data(data_path, base_url);
}

// With no data where MAGPIE looks, or what an unfinished fetch left, either
// runs the `data fetch` it was given, offers to fetch it (when someone is at a
// terminal to answer), or says how. Returns whether the command line was
// `data fetch`, which has then been run.
static bool get_missing_data(const char *data_path, data_fetch_state_t state,
                             int argc, char *argv[], bool *fetched) {
  if (is_data_fetch_command(argc, argv)) {
    *fetched = run_data_fetch_command(data_path, argc, argv);
    return true;
  }
  if (state == DATA_FETCH_STATE_INCOMPLETE) {
    write_to_stream_err("MAGPIE's data files in %s are incomplete: a "
                        "download of them did not finish.\n",
                        data_path);
  } else {
    write_to_stream_err("MAGPIE's data files are not in %s.\n", data_path);
  }
  if (!cterm_is_terminal(get_stream_in())) {
    write_to_stream_err("Run 'magpie data fetch' to download them.\n");
    return false;
  }
  write_to_stream_out("Download them now (about 195 MB)? [Y/n] ");
  char *answer = read_line_from_stream_in();
  if (answer) {
    trim_whitespace(answer);
  }
  const bool yes =
      answer && (answer[0] == '\0' || strings_iequal(answer, "y") ||
                 strings_iequal(answer, "yes"));
  free(answer);
  if (yes) {
    (void)fetch_data(data_path, NULL);
  } else {
    write_to_stream_out("You can download them later with: data fetch\n");
  }
  return false;
}

void process_command_default(int argc, char *argv[]) {
  if (cterm_owns_console()) {
    write_to_stream_out("MAGPIE %s. Type help for the commands, quit to "
                        "leave.\n",
                        config_get_magpie_version());
  }
  // A -path on the command line is where the data is, or is to go.
  char *data_path = command_line_data_path(argc, argv);
  if (!data_path) {
    data_path = locate_default_data_path();
  }
  data_fetch_state_t state = data_fetch_get_state(data_path);
  if (state == DATA_FETCH_STATE_MISSING ||
      state == DATA_FETCH_STATE_INCOMPLETE) {
    bool fetched = false;
    if (get_missing_data(data_path, state, argc, argv, &fetched)) {
      // The command line was the data fetch, and that was all it asked for,
      // as when the data is already there.
      if (!fetched) {
        io_wait_before_close();
      }
      free(data_path);
      return;
    }
    // Fetched at the prompt, or not.
    state = data_fetch_get_state(data_path);
  }
  if (state == DATA_FETCH_STATE_OUTDATED &&
      !is_data_fetch_command(argc, argv)) {
    write_to_stream_err("The data in %s is not the version this MAGPIE goes "
                        "with (%s); 'data fetch' updates it.\n",
                        data_path, DATA_FETCH_VERSION);
  }
  // Data from `data fetch` comes without wordmaps, which are thirty times the
  // size of their lexica, so a lexicon's is built the first time it is used.
  // Data put there otherwise (download_data.sh, then convert_lexica.sh) is
  // left as it is.
  const ConfigArgs config_args = {.data_paths = data_path,
                                  .use_wmp = true,
                                  .build_missing_wordmaps =
                                      state == DATA_FETCH_STATE_CURRENT ||
                                      state == DATA_FETCH_STATE_OUTDATED};
  if (!process_command_internal(argc, argv, &config_args)) {
    io_wait_before_close();
  }
  free(data_path);
}

void process_command_with_config_args(int argc, char *argv[],
                                      const ConfigArgs *config_args) {
  (void)process_command_internal(argc, argv, config_args);
}
