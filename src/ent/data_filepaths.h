#ifndef DATA_FILEPATHS_H
#define DATA_FILEPATHS_H

typedef enum {
  DATA_FILEPATH_TYPE_KWG,
  DATA_FILEPATH_TYPE_KLV,
  DATA_FILEPATH_TYPE_LAYOUT,
  DATA_FILEPATH_TYPE_WIN_PCT,
  DATA_FILEPATH_TYPE_LD,
} data_filepath_t;

char *data_filepaths_get(const char *data_dir, const char *data_name,
                         data_filepath_t type);

#endif