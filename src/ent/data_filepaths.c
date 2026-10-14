#include "data_filepaths.h"

#include "../compat/cdir.h"
#include "../compat/cfile.h"
#include "../compat/cpath.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdlib.h>
#include <string.h>

static const char *const filepath_type_names[] = {"kwg",
                                                  "klv",
                                                  "board layout",
                                                  "win percentage",
                                                  "letter distribution",
                                                  "gcg",
                                                  "leaves",
                                                  "lexicon",
                                                  "wordmap",
                                                  "rack info table",
                                                  "packed dawg",
                                                  "word info table",
                                                  "positional adjustment table",
                                                  "word plus floater"};

void string_builder_add_directory_for_data_type(StringBuilder *sb,
                                                const char *data_path,
                                                data_filepath_t type) {
  switch (type) {
  case DATA_FILEPATH_TYPE_KWG:
  case DATA_FILEPATH_TYPE_KLV:
  case DATA_FILEPATH_TYPE_LEXICON:
  case DATA_FILEPATH_TYPE_WORDMAP:
  case DATA_FILEPATH_TYPE_LEAVES:
  case DATA_FILEPATH_TYPE_RACK_INFO_TABLE:
  case DATA_FILEPATH_TYPE_DAWG_PACKED:
  case DATA_FILEPATH_TYPE_WORD_INFO_TABLE:
    string_builder_add_formatted_string(sb, "%s/lexica/", data_path);
    break;
  case DATA_FILEPATH_TYPE_LAYOUT:
    string_builder_add_formatted_string(sb, "%s/layouts/", data_path);
    break;
  case DATA_FILEPATH_TYPE_WIN_PCT:
  case DATA_FILEPATH_TYPE_PAT:
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
  case DATA_FILEPATH_TYPE_RACK_INFO_TABLE:
    file_ext = RACK_INFO_TABLE_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_PAT:
    file_ext = PAT_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_DAWG_PACKED:
    file_ext = DAWG_PACKED_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_WORD_INFO_TABLE:
    file_ext = WORD_INFO_TABLE_EXTENSION;
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
  char *filepath = string_builder_dump_and_destroy(filepath_sb, NULL);
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
  char **split_data_paths = NULL;
  const int split_data_paths_count =
      cpath_split_list(data_paths, &split_data_paths);
  int number_of_data_paths = split_data_paths_count;
  char *ret_val = NULL;
  for (int i = 0; i < number_of_data_paths; i++) {
    const char *data_path_i = split_data_paths[i];
    char *full_filename_i = get_filepath(data_path_i, data_name, type);
    bool file_exists = fileproxy_file_exists(full_filename_i);
    if (file_exists) {
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
  cpath_list_destroy(split_data_paths, split_data_paths_count);
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
// If data paths has multiple paths (see cpath_split_list), then the
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
  char *first_data_path = cpath_list_first(data_paths);
  char *writable_filepath = get_filepath(first_data_path, data_name, type);
  free(first_data_path);
  // File already exists and is not writable
  if (cfile_access(writable_filepath, CFILE_ACCESS_EXISTS) &&
      !cfile_access(writable_filepath, CFILE_ACCESS_WRITE)) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_WRITABLE,
        get_formatted_string(
            "file %s exists but does not have required write permissions",
            writable_filepath));
    free(writable_filepath);
    return NULL;
  }
  char *dir_path = get_dirpath_from_filepath(writable_filepath);
  if (!cfile_access(dir_path, CFILE_ACCESS_WRITE)) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_WRITABLE,
        get_formatted_string("directory %s does not exist or is not writable",
                             dir_path));
  }
  free(dir_path);
  return writable_filepath;
}

static int compare_filenames(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Adds the files of `type` in `data_path` to `list`, in name order: what the
// glob <directory>/*<extension> lists (hidden files excluded).
static void add_data_path_files(StringList *list, const char *data_path,
                                data_filepath_t type) {
  StringBuilder *directory_sb = string_builder_create();
  string_builder_add_directory_for_data_type(directory_sb, data_path, type);
  const char *directory = string_builder_peek(directory_sb);
  char *empty_name_path = get_filepath(data_path, "", type);
  const char *extension = empty_name_path + string_length(directory);
  CDir *dir = cdir_open(directory);
  if (dir) {
    int count = 0;
    int capacity = 16;
    char **names = malloc_or_die(sizeof(char *) * (size_t)capacity);
    const char *name;
    while ((name = cdir_next(dir)) != NULL) {
      if (name[0] == '.' || !has_suffix(extension, name)) {
        continue;
      }
      if (count == capacity) {
        capacity *= 2;
        names = realloc_or_die(names, sizeof(char *) * (size_t)capacity);
      }
      names[count++] = get_formatted_string("%s%s", directory, name);
    }
    cdir_close(dir);
    qsort(names, (size_t)count, sizeof(char *), compare_filenames);
    for (int name_idx = 0; name_idx < count; name_idx++) {
      string_list_add_string(list, names[name_idx]);
      free(names[name_idx]);
    }
    free(names);
  }
  free(empty_name_path);
  string_builder_destroy(directory_sb);
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
  char **split_data_paths = NULL;
  const int number_of_data_paths =
      cpath_split_list(data_paths, &split_data_paths);
  for (int path_idx = 0; path_idx < number_of_data_paths; path_idx++) {
    // Note: don't push error here - it's OK if some paths don't have matches
    // as long as at least one path does
    add_data_path_files(file_path_list, split_data_paths[path_idx], type);
  }
  cpath_list_destroy(split_data_paths, number_of_data_paths);

  // Only report error if NO paths found ANY files
  if (string_list_get_count(file_path_list) == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_NO_MATCHING_FILES,
        get_formatted_string(
            "no files of type %s matched in any of the data paths: %s",
            filepath_type_names[type], data_paths));
  }

  return file_path_list;
}

static bool is_safe_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool data_filepaths_is_safe_name(const char *name) {
  if (!name || !is_safe_name_char(*name)) {
    return false;
  }
  bool seen_dot = false;
  for (const char *c = name; *c; c++) {
    if (*c == '.') {
      if (seen_dot || !is_safe_name_char(c[1])) {
        return false;
      }
      seen_dot = true;
    } else if (!is_safe_name_char(*c)) {
      return false;
    }
  }
  return true;
}
