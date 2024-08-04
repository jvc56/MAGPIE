#include "data_filepaths.h"

#include <stdlib.h>
#include <unistd.h>

#include "../util/log.h"
#include "../util/string_util.h"

#define KWG_EXTENSION ".kwg"
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
         has_suffix(filepath, KLV_EXTENSION) ||
         has_suffix(filepath, LAYOUT_EXTENSION) ||
         has_suffix(filepath, WIN_PCT_EXTENSION) ||
         has_suffix(filepath, LD_EXTENSION) ||
         has_suffix(filepath, GCG_EXTENSION) ||
         has_suffix(filepath, LEAVES_EXTENSION) ||
         has_suffix(filepath, LEXICON_EXTENSION);
}

char *get_filepath(const char *data_paths, const char *data_name,
                   data_filepath_t type) {
  char *filepath = NULL;
  switch (type) {
  case DATA_FILEPATH_TYPE_KWG:
    filepath = get_formatted_string("%s/lexica/%s%s", data_paths, data_name,
                                    KWG_EXTENSION);
    break;
  case DATA_FILEPATH_TYPE_KLV:
    filepath = get_formatted_string("%s/lexica/%s%s", data_paths, data_name,
                                    KLV_EXTENSION);
    break;
  case DATA_FILEPATH_TYPE_LAYOUT:
    filepath = get_formatted_string("%s/layouts/%s%s", data_paths, data_name,
                                    LAYOUT_EXTENSION);
    break;
  case DATA_FILEPATH_TYPE_WIN_PCT:
    filepath = get_formatted_string("%s/strategy/%s%s", data_paths, data_name,
                                    WIN_PCT_EXTENSION);
    break;
  case DATA_FILEPATH_TYPE_LD:
    filepath = get_formatted_string("%s/letterdistributions/%s%s", data_paths,
                                    data_name, LD_EXTENSION);
    break;
  case DATA_FILEPATH_TYPE_GCG:
    filepath = get_formatted_string("%s/gcgs/%s%s", data_paths, data_name,
                                    GCG_EXTENSION);
    break;
  case DATA_FILEPATH_TYPE_LEAVES:
    filepath = get_formatted_string("%s/leaves/%s%s", data_paths, data_name,
                                    LEAVES_EXTENSION);
    break;
  case DATA_FILEPATH_TYPE_LEXICON:
    filepath = get_formatted_string("%s/lexica/%s%s", data_paths, data_name,
                                    LEXICON_EXTENSION);
    break;
  }
  return filepath;
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
    if (!data_paths) {
      log_fatal("data paths is null for filepath type %d\n", type);
    }
    StringSplitter *split_data_paths = split_string(data_paths, ':', true);
    int number_of_data_paths =
        string_splitter_get_number_of_items(split_data_paths);
    for (int i = 0; i < number_of_data_paths; i++) {
      const char *data_path_i = string_splitter_get_item(split_data_paths, i);
      char *full_filename_i = get_filepath(data_path_i, data_name, type);
      if (access(full_filename_i, F_OK | R_OK) == 0) {
        full_filename = full_filename_i;
        break;
      }
      free(full_filename_i);
    }
    string_splitter_destroy(split_data_paths);
    if (!full_filename) {
      log_fatal("File for %s not found for the following paths: %s\n",
                data_name, data_paths);
    }
  }

  return full_filename;
}

// Returns a filename string for writing. This filename might not
// currently exist.
// If data_name looks like a filepath, then data_name is just returned
// as is.
// Assumes that only a single data path is provided with the data_paths
// argument as opposed to multiple paths separated by ':' as in
// data_filepaths_get_readable_filename
char *data_filepaths_get_writable_filename(const char *data_path,
                                           const char *data_name,
                                           data_filepath_t type) {
  if (is_filepath(data_name)) {
    return string_duplicate(data_name);
  }
  return get_filepath(data_path, data_name, type);
}