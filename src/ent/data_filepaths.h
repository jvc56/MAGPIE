#ifndef DATA_FILEPATHS_H
#define DATA_FILEPATHS_H

typedef enum {
  DATA_FILEPATH_TYPE_KWG,
  DATA_FILEPATH_TYPE_KLV,
  DATA_FILEPATH_TYPE_LAYOUT,
  DATA_FILEPATH_TYPE_WIN_PCT,
  DATA_FILEPATH_TYPE_LD,
  DATA_FILEPATH_TYPE_GCG,
  DATA_FILEPATH_TYPE_LEAVES,
} data_filepath_t;

char *data_filepaths_get(const char *data_path, const char *data_name,
                         data_filepath_t type);

#endif