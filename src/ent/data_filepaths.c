#include "data_filepaths.h"

#include <glob.h>
#include <stdlib.h>
#include <unistd.h>

#include "../util/io.h"
#include "../util/string_util.h"

#define KWG_EXTENSION ".kwg"
#define WORDMAP_EXTENSION ".wmp"
#define KLV_EXTENSION ".klv2"
#define LAYOUT_EXTENSION ".txt"
#define WIN_PCT_EXTENSION ".csv"
#define LD_EXTENSION ".csv"
#define GCG_EXTENSION ".gcg"
#define LEAVES_EXTENSION ".csv"
#define LEXICON_EXTENSION ".txt"

bool is_filepath(const char *filepath) {
  return string_contains(filepath, '/') || string_contains(filepath, '\\') ||
         has_suffix(filepath, KWG_EXTENSION) ||
         has_suffix(filepath, WORDMAP_EXTENSION) ||
         has_suffix(filepath, KLV_EXTENSION) ||
         has_suffix(filepath, LAYOUT_EXTENSION) ||
         has_suffix(filepath, WIN_PCT_EXTENSION) ||
         has_suffix(filepath, LD_EXTENSION) ||
         has_suffix(filepath, GCG_EXTENSION) ||
         has_suffix(filepath, LEAVES_EXTENSION) ||
         has_suffix(filepath, LEXICON_EXTENSION);
}

void string_builder_add_directory_for_data_type(StringBuilder *sb,
                                                const char *data_path,
                                                data_filepath_t type) {
  switch (type) {
  case DATA_FILEPATH_TYPE_KWG:
  case DATA_FILEPATH_TYPE_KLV:
  case DATA_FILEPATH_TYPE_LEXICON:
  case DATA_FILEPATH_TYPE_WORDMAP:
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
  case DATA_FILEPATH_TYPE_LEAVES:
    string_builder_add_formatted_string(sb, "%s/leaves/", data_path);
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
    file_ext = LAYOUT_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_WIN_PCT:
    file_ext = WIN_PCT_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_LD:
    file_ext = LD_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_GCG:
    file_ext = GCG_EXTENSION;
    break;
  case DATA_FILEPATH_TYPE_LEAVES:
    file_ext = LEAVES_EXTENSION;
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
                                              bool data_path_only) {
  if (!data_paths) {
    log_fatal("data paths is null for filepath type %d\n", type);
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
    log_fatal("File for %s not found for the following paths: %s\n", data_name,
              data_paths);
  }
  return ret_val;
}

// Returns the data path that is used in the filename
// returned by data_filepaths_get_readable_filename.
// Assumes the data name is not already a valid filepath.
char *data_filepaths_get_data_path_name(const char *data_paths,
                                        const char *data_name,
                                        data_filepath_t type) {
  if (!data_name) {
    log_fatal("data name is null for filepath type %d\n", type);
  }
  if (is_filepath(data_name)) {
    return NULL;
  }
  return data_filepaths_get_first_valid_filename(data_paths, data_name, type,
                                                 true);
}

// Returns a filename string that exists and is readable.
// Dies if no such file can be found.
// If data_name looks like a filepath, then data_name is just returned
// as is.
char *data_filepaths_get_readable_filename(const char *data_paths,
                                           const char *data_name,
                                           data_filepath_t type) {
  if (!data_name) {
    log_fatal("data name is null for filepath type %d\n", type);
  }

  char *full_filename = NULL;

  if (is_filepath(data_name)) {
    if (access(data_name, F_OK | R_OK) == 0) {
      full_filename = string_duplicate(data_name);
    } else {
      log_fatal("Full file path %s not found\n", data_name);
    }
  } else {
    full_filename = data_filepaths_get_first_valid_filename(
        data_paths, data_name, type, false);
  }

  return full_filename;
}

// Returns a filename string for writing. This filename might not
// currently exist.
// If data_name looks like a filepath, then data_name is just returned
// as is.
// If data paths has multiple paths delimited by ':', then the
// first path is used.
char *data_filepaths_get_writable_filename(const char *data_paths,
                                           const char *data_name,
                                           data_filepath_t type) {
  if (!data_name) {
    log_fatal("data name is null for filepath type %d\n", type);
  }
  char *writable_filepath;
  if (is_filepath(data_name)) {
    writable_filepath = string_duplicate(data_name);
  } else {
    if (!data_paths) {
      log_fatal("data path is null for filepath type %d\n", type);
    }
    char *first_data_path = cut_off_after_first_char(data_paths, ':');
    writable_filepath = get_filepath(first_data_path, data_name, type);
    free(first_data_path);
  }
  // File already exists and is not writable
  if (access(writable_filepath, F_OK) == 0 &&
      access(writable_filepath, W_OK) != 0) {
    log_fatal("File %s already exists and is not writable\n",
              writable_filepath);
  }
  char *dir_path = get_dirpath_from_filepath(writable_filepath);
  if (access(dir_path, W_OK) != 0) {
    log_fatal("Directory %s is not writable\n", dir_path);
  }
  free(dir_path);
  return writable_filepath;
}

StringList *data_filepaths_get_all_data_path_names(const char *data_paths,
                                                   data_filepath_t type) {
  if (!data_paths) {
    log_error("data path is null for filepath type %d\n", type);
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
      log_error("no files matched pattern %s", glob_pattern);
    }
    free(glob_pattern);
    globfree(&glob_results);
  }
  string_splitter_destroy(split_data_paths);
  return file_path_list;
}
