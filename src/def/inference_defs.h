#ifndef INFERENCE_DEFS_H
#define INFERENCE_DEFS_H

#define INFERENCE_EQUITY_EPSILON 0 // 0/EQUITY_RESOLUTION

typedef enum {
  INFERENCE_STATUS_SUCCESS,
  INFERENCE_STATUS_NO_TILES_PLAYED,
  INFERENCE_STATUS_RACK_OVERFLOW,
  INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG,
  INFERENCE_STATUS_BOTH_PLAY_AND_EXCHANGE,
  INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO,
  INFERENCE_STATUS_EXCHANGE_NOT_ALLOWED,
} inference_status_t;

typedef enum {
  INFERENCE_TYPE_LEAVE,
  INFERENCE_TYPE_EXCHANGED,
  INFERENCE_TYPE_RACK,
} inference_stat_t;

typedef enum {
  INFERENCE_SUBTOTAL_DRAW,
  INFERENCE_SUBTOTAL_LEAVE,
} inference_subtotal_t;

#endif