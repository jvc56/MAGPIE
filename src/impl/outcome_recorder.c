// Per-game in-memory buffer + thread-safe CSV writer for outcome_model
// training data.

#include "outcome_recorder.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../util/io_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct OutcomeRecorder {
  FILE *fp;
  cpthread_mutex_t mutex;
};

typedef struct {
  OutcomeFeatures features;
  int us_player_index;
} BufferedRow;

struct OutcomeGameBuffer {
  BufferedRow *rows;
  int count;
  int cap;
};

OutcomeRecorder *outcome_recorder_create(const char *path) {
  OutcomeRecorder *rec = malloc_or_die(sizeof(OutcomeRecorder));
  rec->fp = fopen(path, "w");
  if (rec->fp == NULL) {
    log_fatal("could not open outcome recorder file '%s'", path);
  }
  cpthread_mutex_init(&rec->mutex);
  // Header row.
  fprintf(rec->fp, "us_st_frac_playable,us_st_top1,us_st_top2,"
                   "opp_st_frac_playable,opp_st_top1,opp_st_top2,"
                   "us_bingo_prob,opp_bingo_prob,"
                   "unplayed_blanks,tiles_unseen,score_diff,"
                   "us_leave_value,"
                   "win,final_spread\n");
  return rec;
}

void outcome_recorder_destroy(OutcomeRecorder *rec) {
  if (rec == NULL) {
    return;
  }
  if (rec->fp != NULL) {
    fclose(rec->fp);
  }
  free(rec);
}

OutcomeGameBuffer *outcome_game_buffer_create(void) {
  OutcomeGameBuffer *buf = malloc_or_die(sizeof(OutcomeGameBuffer));
  buf->cap = 64;
  buf->count = 0;
  buf->rows = malloc_or_die((size_t)buf->cap * sizeof(BufferedRow));
  return buf;
}

void outcome_game_buffer_destroy(OutcomeGameBuffer *buf) {
  if (buf == NULL) {
    return;
  }
  free(buf->rows);
  free(buf);
}

void outcome_game_buffer_reset(OutcomeGameBuffer *buf) { buf->count = 0; }

void outcome_game_buffer_add(OutcomeGameBuffer *buf,
                             const OutcomeFeatures *features,
                             int us_player_index) {
  if (buf->count == buf->cap) {
    buf->cap *= 2;
    buf->rows =
        realloc_or_die(buf->rows, (size_t)buf->cap * sizeof(BufferedRow));
  }
  buf->rows[buf->count].features = *features;
  buf->rows[buf->count].us_player_index = us_player_index;
  buf->count++;
}

void outcome_game_buffer_flush(OutcomeGameBuffer *buf, OutcomeRecorder *rec,
                               int p0_final_score, int p1_final_score) {
  if (rec == NULL || buf->count == 0) {
    outcome_game_buffer_reset(buf);
    return;
  }
  cpthread_mutex_lock(&rec->mutex);
  for (int i = 0; i < buf->count; i++) {
    const BufferedRow *row = &buf->rows[i];
    const int us_score =
        (row->us_player_index == 0) ? p0_final_score : p1_final_score;
    const int opp_score =
        (row->us_player_index == 0) ? p1_final_score : p0_final_score;
    const int spread = us_score - opp_score;
    const double win = (spread > 0) ? 1.0 : (spread < 0) ? 0.0 : 0.5;
    const OutcomeFeatures *f = &row->features;
    fprintf(rec->fp,
            "%.6f,%.3f,%.3f,%.6f,%.3f,%.3f,%.6f,%.6f,%d,%d,%d,%d,%.1f,%d\n",
            f->us_st_frac_playable, f->us_st_top1, f->us_st_top2,
            f->opp_st_frac_playable, f->opp_st_top1, f->opp_st_top2,
            f->us_bingo_prob, f->opp_bingo_prob, f->unplayed_blanks,
            f->tiles_unseen, f->score_diff, f->us_leave_value, win, spread);
  }
  cpthread_mutex_unlock(&rec->mutex);
  outcome_game_buffer_reset(buf);
}
