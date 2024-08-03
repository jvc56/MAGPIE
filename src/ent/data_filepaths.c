#include "data_filepaths.h"

#include <stdlib.h>
#include <unistd.h>

#include "../util/log.h"
#include "../util/string_util.h"

char *get_filepath(const char *data_paths, const char *data_name,
                   data_filepath_t type) {
  char *filepath = NULL;
  switch (type) {
  case DATA_FILEPATH_TYPE_KWG:
    filepath = get_formatted_string("%s/lexica/%s.kwg", data_paths, data_name);
    break;
  case DATA_FILEPATH_TYPE_KLV:
    filepath = get_formatted_string("%s/lexica/%s.klv2", data_paths, data_name);
    break;
  case DATA_FILEPATH_TYPE_LAYOUT:
    filepath = get_formatted_string("%s/layouts/%s.txt", data_paths, data_name);
    break;
  case DATA_FILEPATH_TYPE_WIN_PCT:
    filepath =
        get_formatted_string("%s/strategy/%s.csv", data_paths, data_name);
    break;
  case DATA_FILEPATH_TYPE_LD:
    filepath = get_formatted_string("%s/letterdistributions/%s.csv", data_paths,
                                    data_name);
    break;
  case DATA_FILEPATH_TYPE_GCG:
    filepath = get_formatted_string("%s/gcgs/%s.gcg", data_paths, data_name);
    break;
  case DATA_FILEPATH_TYPE_LEAVES:
    filepath = get_formatted_string("%s/leaves/%s.csv", data_paths, data_name);
    break;
  }
  return filepath;
}

// Returns a filename string that exists and is readable.
// Dies if no such file can be found.
char *data_filepaths_get_readable_filename(const char *data_paths,
                                           const char *data_name,
                                           data_filepath_t type) {
  if (!data_paths) {
    log_fatal("data paths is null for filepath type %d\n", type);
  }
  if (!data_name) {
    log_fatal("data name is null for filepath type %d\n", type);
  }

  StringSplitter *split_data_paths = split_string(data_paths, ':', true);
  int number_of_data_paths =
      string_splitter_get_number_of_items(split_data_paths);

  char *full_filename = NULL;
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
    log_fatal("File for %s not found for the following paths: %s\n", data_name,
              data_paths);
  }

  return full_filename;
}

// Returns a filename string for writing. This filename might not
// currently exist.
// Assumes that only a single data path is provided with the data_paths
// argument as opposed to multiple paths separated by ':' as in
// data_filepaths_get_readable_filename
char *data_filepaths_get_writable_filename(const char *data_path,
                                           const char *data_name,
                                           data_filepath_t type) {
  return get_filepath(data_path, data_name, type);
}