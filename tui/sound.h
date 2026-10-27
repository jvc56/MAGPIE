#ifndef TUI_SOUND_H
#define TUI_SOUND_H

#include <stdint.h>

// Short sound effects for play against the computer, synthesized on the
// fly (every play sounds a little different) and played in-process
// through miniaudio (vendor/miniaudio).

typedef enum {
  // The computer's tile placement: a soft tock per tile placed.
  TUI_SOUND_PLACE,
  // The computer's exchange: a muffled drop per tile, into the bag.
  TUI_SOUND_EXCHANGE,
  // A play of the user's that was phony: two soft falling notes.
  TUI_SOUND_PHONY,
} TuiSound;

enum { TUI_SOUND_RATE = 44100 };

// Sets up the sound thread's state. Call once at startup, before any
// other thread can play a sound; nothing starts until the first sound.
void tui_sound_init(void);

// Plays `sound` for `tiles` tiles (ignored by TUI_SOUND_PHONY). Returns at
// once: the sound thread renders it and plays it, opening the audio
// device the first time. Without an audio device, nothing plays.
void tui_sound_play(TuiSound sound, int tiles);

// Stops the sound thread and closes the audio device, if they started.
void tui_sound_shutdown(void);

// Renders `sound` for `tiles` tiles as mono 16-bit samples at
// TUI_SOUND_RATE into `out`, varied by `seed`. Returns the number of
// samples, at most `capacity`.
int tui_sound_render(TuiSound sound, int tiles, uint32_t seed, int16_t *out,
                     int capacity);

#endif
