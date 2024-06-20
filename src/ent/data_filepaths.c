#include "data_filepaths.h"

#include <stdlib.h>
#include <unistd.h>

#include "../util/log.h"
#include "../util/string_util.h"

char *data_filepaths_get(const char *data_path, const char *data_name,
                         data_filepath_t type) {
  if (!data_path) {
    log_fatal("data path is null for filepath type %d\n", type);
  }
  if (!data_name) {
    log_fatal("data name is null for filepath type %d\n", type);
  }

  StringSplitter *split_data_paths = split_string(data_path, ':', true);
  int number_of_data_paths =
      string_splitter_get_number_of_items(split_data_paths);

  char *full_filename = NULL;
  for (int i = 0; i < number_of_data_paths; i++) {
    const char *data_path_i = string_splitter_get_item(split_data_paths, i);
    char *full_filename_i = NULL;
    switch (type) {
    case DATA_FILEPATH_TYPE_KWG:
      full_filename_i =
          get_formatted_string("%s/lexica/%s.kwg", data_path_i, data_name);
      break;
    case DATA_FILEPATH_TYPE_KLV:
      full_filename_i =
          get_formatted_string("%s/lexica/%s.klv2", data_path_i, data_name);
      break;
    case DATA_FILEPATH_TYPE_LAYOUT:
      full_filename_i =
          get_formatted_string("%s/layouts/%s.txt", data_path_i, data_name);
      break;
    case DATA_FILEPATH_TYPE_WIN_PCT:
      full_filename_i =
          get_formatted_string("%s/strategy/%s.csv", data_path_i, data_name);
      break;
    case DATA_FILEPATH_TYPE_LD:
      full_filename_i = get_formatted_string("%s/letterdistributions/%s.csv",
                                             data_path_i, data_name);
      break;
    case DATA_FILEPATH_TYPE_GCG:
      full_filename_i =
          get_formatted_string("%s/gcgs/%s.gcg", data_path_i, data_name);
      break;
    case DATA_FILEPATH_TYPE_LEAVES:
      full_filename_i =
          get_formatted_string("%s/leaves/%s.csv", data_path_i, data_name);
      break;
    }

    if (access(full_filename_i, F_OK | R_OK) == 0) {
      full_filename = full_filename_i;
      break;
    }
    free(full_filename_i);
  }

  destroy_string_splitter(split_data_paths);

  if (!full_filename) {
    log_fatal("File for %s not found for the following paths: %s\n", data_name,
              data_path);
  }

  return full_filename;
}