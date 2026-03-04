#ifndef AUTOPLAY_DEFS_H
#define AUTOPLAY_DEFS_H

typedef enum {
  AUTOPLAY_TYPE_DEFAULT,
  AUTOPLAY_TYPE_LEAVE_GEN,
} autoplay_t;

typedef enum {
  MULTI_THREADING_MODE_ONE_THREAD_PER_GAME,
  MULTI_THREADING_MODE_ONE_GAME_ALL_THREADS,
} multi_threading_mode_t;

#endif