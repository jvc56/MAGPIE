#include "data_filepaths.h"

#include <glob.h>
#include <stdlib.h>
#include <unistd.h>

#include "../util/io_util.h"
#include "../util/string_util.h"

#define KWG_EXTENSION ".kwg"
#define WORDMAP_EXTENSION ".wmp"
#define KLV_EXTENSION ".klv2"
#define TXT_EXTENSION ".txt"
#define CSV_EXTENSION ".csv"
#define GCG_EXTENSION ".gcg"
#define LEXICON_EXTENSION ".txt"

static const char *const filepath_type_names[] = {
    "kwg", "klv",    "board layout", "win percentage", "letter distribution",
    "gcg", "leaves", "lexicon",      "wordmap"};

void string_builder_add_directory_for_data_type(StringBuilder *sb,
                                                const char *data_path,
                                                data_filepath_t type) {
  switch (type) {
  case DATA_FILEPATH_TYPE_KWG:
  case DATA_FILEPATH_TYPE_KLV:
  case DATA_FILEPATH_TYPE_LEXICON:
  case DATA_FILEPATH_TYPE_WORDMAP:
  case DATA_FILEPATH_TYPE_LEAVES:
    string_builder_add_formatted_string(sb, "%s/lexica/", data_path);
    break;
  case DATA_FILEPATH_TYPE_LAYOUT:
    string_builder_add_formatted_string(sb, "%s/layouts/", data_path);
    break;
  case DATA_FILEPATH_TYPE_WIN_PCT:
    string_builder_add_formatted_string(sb, "%s/strategy/", data_path);
    break;
  case DATA_FILEPATH_TYPE_LD:
    string_builder_add_formatted_string(sb, "%s/letterdistributions/",
                                        data_path);
    break;
  case DATA_FILEPATH_TYPE_GCG:
    string_builder_add_formatted_string(sb, "%s/gcgs/", data_path);
    break;
  }
}

char *get_filepath(const char *data_path, const char *data_name,
                   data_filepath_t type) {
  StringBuilder *filepath_sb = string_builder_create();
  string_builder_add_directory_for_data_type(filepath_sb, data_path, type);
  string_builder_add_string(filepath_sb, data_name);
  const char *file_ext = NULL;
  switch (type) {
  case DATA_FILEPATH_TYPE_KWG:
    file_ext = KWG_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_WORDMAP:
    file_ext = WORDMAP_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_KLV:
    file_ext = KLV_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_LAYOUT:
    file_ext = TXT_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_WIN_PCT:
  case DATA_FILEPATH_TYPE_LD:
  case DATA_FILEPATH_TYPE_LEAVES:
    file_ext = CSV_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_GCG:
    file_ext = GCG_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_LEXICON:
    file_ext = LEXICON_EXTENSION;
    break;
  }
  string_builder_add_string(filepath_sb, file_ext);
  char *filepath = string_builder_dump(filepath_sb, NULL);
  string_builder_destroy(filepath_sb);
  return filepath;
}

char *data_filepaths_get_first_valid_filename(const char *data_paths,
                                              const char *data_name,
                                              data_filepath_t type,
                                              bool data_path_only,
                                              ErrorStack *error_stack) {
  if (!data_paths) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_NULL_PATH,
        get_formatted_string("data path is unexpectedly empty when trying to "
                             "get filepath of type %s",
                             filepath_type_names[type]));
    return NULL;
  }
  StringSplitter *split_data_paths = split_string(data_paths, ':', true);
  int number_of_data_paths =
      string_splitter_get_number_of_items(split_data_paths);
  char *ret_val = NULL;
  for (int i = 0; i < number_of_data_paths; i++) {
    const char *data_path_i = string_splitter_get_item(split_data_paths, i);
    char *full_filename_i = get_filepath(data_path_i, data_name, type);
    if (access(full_filename_i, F_OK | R_OK) == 0) {
      if (data_path_only) {
        ret_val = string_duplicate(data_path_i);
        free(full_filename_i);
      } else {
        ret_val = full_filename_i;
      }
      break;
    }
    free(full_filename_i);
  }
  string_splitter_destroy(split_data_paths);
  if (!ret_val) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
        get_formatted_string("file '%s' not found for data type %s", data_name,
                             filepath_type_names[type]));
    return NULL;
  }
  return ret_val;
}

// Returns a filename string that exists and is readable.
// Dies if no such file can be found.
// If data_name looks like a filepath, then data_name is just returned
// as is.
char *data_filepaths_get_readable_filename(const char *data_paths,
                                           const char *data_name,
                                           data_filepath_t type,
                                           ErrorStack *error_stack) {
  if (!data_name) {
    error_stack_push(error_stack, ERROR_STATUS_FILEPATH_NULL_FILENAME,
                     get_formatted_string("data name is null for data type %s",
                                          filepath_type_names[type]));
    return NULL;
  }
  return data_filepaths_get_first_valid_filename(data_paths, data_name, type,
                                                 false, error_stack);
}

// Returns a filename string for writing. This filename might not
// currently exist.
// If data_name looks like a filepath, then data_name is just returned
// as is.
// If data paths has multiple paths delimited by ':', then the
// first path is used.
char *data_filepaths_get_writable_filename(const char *data_paths,
                                           const char *data_name,
                                           data_filepath_t type,
                                           ErrorStack *error_stack) {
  if (!data_name) {
    error_stack_push(error_stack, ERROR_STATUS_FILEPATH_NULL_FILENAME,
                     get_formatted_string("data name is null for data type %s",
                                          filepath_type_names[type]));
    return NULL;
  }

  if (!data_paths) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_NULL_PATH,
        get_formatted_string("data path is unexpectedly empty when trying to "
                             "get filepath of type %s",
                             filepath_type_names[type]));
    return NULL;
  }
  char *first_data_path = cut_off_after_first_char(data_paths, ':');
  char *writable_filepath = get_filepath(first_data_path, data_name, type);
  free(first_data_path);
  // File already exists and is not writable
  if (access(writable_filepath, F_OK) == 0 &&
      access(writable_filepath, W_OK) != 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_WRITABLE,
        get_formatted_string(
            "file %s exists but does not have required write permissions",
            writable_filepath));
    return NULL;
  }
  char *dir_path = get_dirpath_from_filepath(writable_filepath);
  if (access(dir_path, F_OK) != 0 || access(dir_path, W_OK) != 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_WRITABLE,
        get_formatted_string("directory %s does not exist or is not writable",
                             dir_path));
  }
  free(dir_path);
  return writable_filepath;
}

StringList *data_filepaths_get_all_data_path_names(const char *data_paths,
                                                   data_filepath_t type,
                                                   ErrorStack *error_stack) {
  if (!data_paths) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_NULL_PATH,
        get_formatted_string("data path is unexpectedly empty when trying to "
                             "get filepath of type %s",
                             filepath_type_names[type]));
    return NULL;
  }
  StringList *file_path_list = string_list_create();
  StringSplitter *split_data_paths = split_string(data_paths, ':', true);
  const int number_of_data_paths =
      string_splitter_get_number_of_items(split_data_paths);
  for (int path_idx = 0; path_idx < number_of_data_paths; path_idx++) {
    const char *data_path =
        string_splitter_get_item(split_data_paths, path_idx);
    char *glob_pattern = get_filepath(data_path, "*", type);
    glob_t glob_results;
    const int glob_status = glob(glob_pattern, 0, NULL, &glob_results);
    if (glob_status == 0) {
      for (size_t result_idx = 0; result_idx < glob_results.gl_pathc;
           result_idx++) {
        string_list_add_string(file_path_list,
                               glob_results.gl_pathv[result_idx]);
      }
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_FILEPATH_NO_MATCHING_FILES,
          get_formatted_string("no files matched pattern %s for type %s",
                               glob_pattern, filepath_type_names[type]));
    }
    free(glob_pattern);
    globfree(&glob_results);
  }
  string_splitter_destroy(split_data_paths);
  return file_path_list;
}
