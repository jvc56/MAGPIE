#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fileproxy.h"
#include "winpct.h"

extern inline float win_pct(WinPct *wp, int spread_plus_leftover,
                            unsigned int tiles_unseen);

// note: this function was largely written by ChatGPT.
void parse_winpct_csv(WinPct *wp, const char *filename) {
  FILE *file = stream_from_filename(filename);
  if (file == NULL) {
    printf("Error opening file: %s\n", filename);
    return;
  }
  int max_rows = MAX_SPREAD * 2 + 1;
  // Allocate memory for the 2D array
  float **array = (float **)malloc(max_rows * sizeof(float *));
  for (int i = 0; i < max_rows; i++) {
    array[i] = (float *)malloc(MAX_COLS * sizeof(float));
  }

  // Read and process the CSV file
  // Assuming each field is no longer than 10 characters
  char line[MAX_COLS * 10];

  // Read the header line
  fgets(line, sizeof(line), file);

  // Read data lines
  int row = 0;
  while (fgets(line, sizeof(line), file) != NULL && row < max_rows) {
    char *token = strtok(line, ",");
    int col = 0;

    while (token != NULL) {
      if (col != 0) {
        // ignore first column.
        array[row][col - 1] = atof(token);
      }
      col++;
      token = strtok(NULL, ",");
    }

    row++;
  }

  fclose(file);
  wp->win_pcts = array;

  wp->max_spread = 300;
  wp->min_spread = -300;
  wp->max_tiles_unseen = 93;
}

WinPct *create_winpct(const char *winpct_filename) {
  WinPct *wp = malloc(sizeof(WinPct));
  parse_winpct_csv(wp, winpct_filename);
  return wp;
}

// Function to free the memory allocated for the 2D array
void destroy_winpct(WinPct *wp) {
  if (wp == NULL) {
    return;
  }
  for (int i = 0; i < MAX_SPREAD * 2 + 1; i++) {
    free(wp->win_pcts[i]);
  }
  free(wp->win_pcts);
  free(wp);
}