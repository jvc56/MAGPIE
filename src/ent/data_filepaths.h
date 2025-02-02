#ifndef DATA_FILEPATHS_H
#define DATA_FILEPATHS_H

#include "../util/string_util.h"

typedef enum {
  DATA_FILEPATH_TYPE_KWG,
  DATA_FILEPATH_TYPE_KLV,
  DATA_FILEPATH_TYPE_LAYOUT,
  DATA_FILEPATH_TYPE_WIN_PCT,
  DATA_FILEPATH_TYPE_LD,
  DATA_FILEPATH_TYPE_GCG,
  DATA_FILEPATH_TYPE_LEAVES,
  DATA_FILEPATH_TYPE_LEXICON,
  DATA_FILEPATH_TYPE_WORDMAP,
} data_filepath_t;

char *data_filepaths_get_readable_filename(const char *data_paths,
                                           const char *data_name,
                                           data_filepath_t type);
char *data_filepaths_get_writable_filename(const char *data_path,
                                           const char *data_name,
                                           data_filepath_t type);

char *data_filepaths_get_data_path_name(const char *data_paths,
                                        const char *data_name,
                                        data_filepath_t type);

StringList *data_filepaths_get_all_data_path_names(const char *data_paths,
                                                   data_filepath_t type);

#endif
