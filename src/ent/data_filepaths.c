#include "data_filepaths.h"

#include <stdlib.h>

#include "../util/log.h"
#include "../util/string_util.h"

char *data_filepaths_get(const char *data_dir, const char *data_name,
                         data_filepath_t type) {
  if (!data_dir) {
    log_fatal("data dir is null for filepath type %d\n", type);
  }
  if (!data_name) {
    log_fatal("data name is null for filepath type %d\n", type);
  }
  char *path = NULL;
  switch (type) {
  case DATA_FILEPATH_TYPE_KWG:
    path = get_formatted_string("%s/lexica/%s.kwg", data_dir, data_name);
    break;
  case DATA_FILEPATH_TYPE_KLV:
    path = get_formatted_string("%s/lexica/%s.klv2", data_dir, data_name);
    break;
  case DATA_FILEPATH_TYPE_LAYOUT:
    path = get_formatted_string("%s/layouts/%s.txt", data_dir, data_name);
    break;
  case DATA_FILEPATH_TYPE_WIN_PCT:
    path = get_formatted_string("%s/strategy/%s.csv", data_dir, data_name);
    break;
  case DATA_FILEPATH_TYPE_LD:
    path = get_formatted_string("%s/letterdistributions/%s.csv", data_dir,
                                data_name);
    break;
  }
  return path;
}