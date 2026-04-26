#ifndef OUTCOME_RECORDER_H
#define OUTCOME_RECORDER_H

#include "../compat/cpthread.h"
#include "outcome_features.h"
#include <stdint.h>
#include <stdio.h>

// Thread-safe CSV writer for outcome_model training rows. Each thread
// owns a buffer of features captured during a single game; on game end
// the thread fills in the win/spread targets (looked up from final
// game scores) and flushes the buffer to the shared CSV file under a
// mutex.
//
// CSV columns:
//   us_st_frac_playable, us_st_top1, us_st_top2,
//   opp_st_frac_playable, opp_st_top1, opp_st_top2,
//   us_bingo_prob, opp_bingo_prob,
//   unplayed_blanks, tiles_unseen, score_diff,
//   win, final_spread
//
// "win" is 0 / 0.5 / 1 from the recording-moment on-turn player's POV.
// "final_spread" is signed, also from that POV.

typedef struct OutcomeRecorder OutcomeRecorder;

OutcomeRecorder *outcome_recorder_create(const char *path);
void outcome_recorder_destroy(OutcomeRecorder *rec);

// Per-game buffer. Each worker (or each game-runner) gets its own.
typedef struct OutcomeGameBuffer OutcomeGameBuffer;

OutcomeGameBuffer *outcome_game_buffer_create(void);
void outcome_game_buffer_destroy(OutcomeGameBuffer *buf);
void outcome_game_buffer_reset(OutcomeGameBuffer *buf);

// Append one row's features. The recording-moment on-turn player index
// is captured so we can flip win/spread to that POV at flush time.
void outcome_game_buffer_add(OutcomeGameBuffer *buf,
                             const OutcomeFeatures *features,
                             int us_player_index);

// Flush the buffer to the recorder, computing win/spread for each row
// from the final scores. Acquires the recorder's mutex once for the
// whole flush. Resets the buffer.
void outcome_game_buffer_flush(OutcomeGameBuffer *buf, OutcomeRecorder *rec,
                               int p0_final_score, int p1_final_score);

#endif
