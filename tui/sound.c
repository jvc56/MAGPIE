#include "sound.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../vendor/miniaudio/miniaudio_tui.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Longest sound: seven tiles' worth of drops plus their tails.
  MAX_SECONDS = 2,
  MAX_SAMPLES = TUI_SOUND_RATE * MAX_SECONDS,
  MAX_TILES = 15,
  // Sounds playing at once; one more is dropped.
  MAX_VOICES = 8,
  // Sounds waiting for the sound thread; one more is dropped.
  QUEUE_MAX = 8,
};

static const double TWO_PI = 6.283185307179586;

// The device stops after this long with nothing playing: an open output
// stream keeps some systems (macOS) from sleeping.
static const int64_t IDLE_STOP_NS = 2000000000LL;
// How often the sound thread checks for that while the device runs.
static const long IDLE_POLL_NS = 250000000L;

// ── Synthesis ─────────────────────────────────────────────────────────

// xorshift32: varies each tile's pitch, level and timing.
static uint32_t next_random(uint32_t *state) {
  uint32_t bits = *state;
  bits ^= bits << 13;
  bits ^= bits >> 17;
  bits ^= bits << 5;
  *state = bits;
  return bits;
}

// Uniform in [-1, 1).
static double random_unit(uint32_t *state) {
  return (double)(next_random(state) >> 8) / (double)(1U << 23) - 1.0;
}

// A struck mode: a sine at `freq` from `start`, rising over half a
// millisecond and decaying with time constant `decay` seconds.
static void add_mode(double *mix, int len, double start, double freq,
                     double amp, double decay) {
  const int first = (int)(start * TUI_SOUND_RATE);
  const int span = (int)(decay * 7.0 * TUI_SOUND_RATE);
  const double attack = 0.0005;
  for (int sample_idx = 0; sample_idx < span && first + sample_idx < len;
       sample_idx++) {
    if (first + sample_idx < 0) {
      continue;
    }
    const double seconds = (double)sample_idx / TUI_SOUND_RATE;
    const double env =
        (seconds < attack ? seconds / attack : 1.0) * exp(-seconds / decay);
    mix[first + sample_idx] += amp * env * sin(TWO_PI * freq * seconds);
  }
}

// Noise from `start`, lowpassed at `cutoff` Hz, rising over `attack`
// seconds and decaying with time constant `decay`.
static void add_noise(double *mix, int len, double start, double amp,
                      double cutoff, double attack, double decay,
                      uint32_t *rng) {
  const int first = (int)(start * TUI_SOUND_RATE);
  const int span = (int)((attack + decay * 7.0) * TUI_SOUND_RATE);
  const double alpha = 1.0 - exp(-TWO_PI * cutoff / TUI_SOUND_RATE);
  double low = 0.0;
  for (int sample_idx = 0; sample_idx < span && first + sample_idx < len;
       sample_idx++) {
    const double seconds = (double)sample_idx / TUI_SOUND_RATE;
    low += alpha * (random_unit(rng) - low);
    if (first + sample_idx < 0) {
      continue;
    }
    const double env =
        seconds < attack ? seconds / attack : exp(-(seconds - attack) / decay);
    mix[first + sample_idx] += amp * env * low;
  }
}

// A tile set down on the board: a short hollow tock (a bar's first three
// modes) over a low thump, with a tick of noise for the contact.
static void add_tock(double *mix, int len, double start, double pitch,
                     double level, uint32_t *rng) {
  const double base = 820.0 * pitch;
  add_mode(mix, len, start, base, 0.55 * level, 0.030);
  add_mode(mix, len, start, base * 2.76, 0.22 * level, 0.014);
  add_mode(mix, len, start, base * 5.40, 0.08 * level, 0.007);
  add_mode(mix, len, start, 165.0 * pitch, 0.30 * level, 0.028);
  add_noise(mix, len, start, 0.35 * level, 6000.0, 0.0003, 0.0012, rng);
}

// A tile dropped into the cloth bag: a muffled rustle with a soft,
// low knock where it lands among the others.
static void add_drop(double *mix, int len, double start, double pitch,
                     double level, uint32_t *rng) {
  add_noise(mix, len, start, 1.4 * level, 900.0 * pitch, 0.006, 0.040, rng);
  add_mode(mix, len, start + 0.012, 240.0 * pitch, 0.48 * level, 0.022);
  add_mode(mix, len, start + 0.012, 240.0 * pitch * 2.3, 0.15 * level, 0.012);
}

// A soft mallet note at `freq` whose pitch sags by `sag` (a fraction)
// over its decay, so the figure sounds deflated rather than alarmed.
static void add_mallet(double *mix, int len, double start, double freq,
                       double amp, double decay, double sag) {
  const int first = (int)(start * TUI_SOUND_RATE);
  const int span = (int)(decay * 7.0 * TUI_SOUND_RATE);
  const double attack = 0.004;
  double phase = 0.0;
  double overtone_phase = 0.0;
  for (int sample_idx = 0; sample_idx < span && first + sample_idx < len;
       sample_idx++) {
    const double seconds = (double)sample_idx / TUI_SOUND_RATE;
    const double freq_now = freq * (1.0 - sag * (1.0 - exp(-seconds / decay)));
    phase += TWO_PI * freq_now / TUI_SOUND_RATE;
    overtone_phase += TWO_PI * freq_now * 3.9 / TUI_SOUND_RATE;
    const double env =
        (seconds < attack ? seconds / attack : 1.0) * exp(-seconds / decay);
    const double overtone = 0.18 * exp(-seconds / (decay * 0.15));
    mix[first + sample_idx] +=
        amp * env * (sin(phase) + overtone * sin(overtone_phase));
  }
}

int tui_sound_render(TuiSound sound, int tiles, uint32_t seed, int16_t *out,
                     int capacity) {
  uint32_t rng = seed != 0 ? seed : 0x9e3779b9U;
  if (tiles < 1) {
    tiles = 1;
  }
  if (tiles > MAX_TILES) {
    tiles = MAX_TILES;
  }
  double *mix = calloc(MAX_SAMPLES, sizeof(double));
  if (mix == NULL) {
    return 0;
  }
  double end = 0.0;
  switch (sound) {
  case TUI_SOUND_PLACE: {
    // Placed one after another, a little unevenly, as by hand.
    double start = 0.0;
    for (int tile_idx = 0; tile_idx < tiles; tile_idx++) {
      const double pitch = 1.0 + 0.035 * random_unit(&rng);
      const double level = 0.85 + 0.12 * random_unit(&rng);
      add_tock(mix, MAX_SAMPLES, start, pitch, level, &rng);
      end = start + 0.25;
      start += 0.075 + 0.012 * random_unit(&rng);
    }
    break;
  }
  case TUI_SOUND_EXCHANGE: {
    double start = 0.0;
    for (int tile_idx = 0; tile_idx < tiles; tile_idx++) {
      const double pitch = 1.0 + 0.06 * random_unit(&rng);
      const double level = 0.8 + 0.15 * random_unit(&rng);
      add_drop(mix, MAX_SAMPLES, start, pitch, level, &rng);
      end = start + 0.35;
      start += 0.085 + 0.02 * random_unit(&rng);
    }
    break;
  }
  case TUI_SOUND_PHONY:
    // A falling semitone, the second note softer and sagging.
    add_mallet(mix, MAX_SAMPLES, 0.0, 329.63, 0.55, 0.12, 0.0);
    add_mallet(mix, MAX_SAMPLES, 0.15, 311.13, 0.5, 0.20, 0.03);
    end = 0.15 + 0.20 * 4.5;
    break;
  }
  int count = (int)(end * TUI_SOUND_RATE);
  if (count > MAX_SAMPLES) {
    count = MAX_SAMPLES;
  }
  if (count > capacity) {
    count = capacity;
  }
  // Quiet overall, with a soft limit so stacked tails never clip.
  for (int sample_idx = 0; sample_idx < count; sample_idx++) {
    out[sample_idx] = (int16_t)(tanh(mix[sample_idx] * 0.5) * 32767.0 * 0.6);
  }
  free(mix);
  return count;
}

// ── Playback ──────────────────────────────────────────────────────────

// A rendered sound the device is playing. Free when position reaches
// count; its samples are released by the sound thread, not the device.
typedef struct {
  int16_t *samples;
  int count;
  int position;
} Voice;

typedef struct {
  TuiSound sound;
  int tiles;
  uint32_t seed;
} PlayJob;

// Producers (any thread) and the sound thread share the queue.
static cpthread_mutex_t queue_mutex;
static cpthread_cond_t queue_cond;
static PlayJob queue[QUEUE_MAX];
static int queue_count;
static bool initialized;
static bool thread_started;
static bool quitting;
static cpthread_t sound_thread_handle;
static atomic_uint play_count;

// Only the sound thread touches the device after it starts.
static ma_device device;
static bool device_open;
static bool device_failed;
static bool device_running;

// The device's callback and the sound thread share the voices.
static ma_spinlock voices_lock;
static Voice voices[MAX_VOICES];

// The device's callback: mixes every playing voice into `output`.
static void mix_voices(ma_device *playing_device, void *output,
                       const void *input, ma_uint32 frame_count) {
  (void)playing_device;
  (void)input;
  float *frames = (float *)output;
  memset(frames, 0, (size_t)frame_count * sizeof(float));
  ma_spinlock_lock(&voices_lock);
  for (int voice_idx = 0; voice_idx < MAX_VOICES; voice_idx++) {
    Voice *voice = &voices[voice_idx];
    int left = voice->count - voice->position;
    if (left > (int)frame_count) {
      left = (int)frame_count;
    }
    for (int frame_idx = 0; frame_idx < left; frame_idx++) {
      frames[frame_idx] +=
          (float)voice->samples[voice->position + frame_idx] / 32768.0F;
    }
    if (left > 0) {
      voice->position += left;
    }
  }
  ma_spinlock_unlock(&voices_lock);
  for (ma_uint32 frame_idx = 0; frame_idx < frame_count; frame_idx++) {
    if (frames[frame_idx] > 1.0F) {
      frames[frame_idx] = 1.0F;
    } else if (frames[frame_idx] < -1.0F) {
      frames[frame_idx] = -1.0F;
    }
  }
}

static bool voices_playing(void) {
  bool playing = false;
  ma_spinlock_lock(&voices_lock);
  for (int voice_idx = 0; voice_idx < MAX_VOICES; voice_idx++) {
    if (voices[voice_idx].position < voices[voice_idx].count) {
      playing = true;
    }
  }
  ma_spinlock_unlock(&voices_lock);
  return playing;
}

// Opens the default playback device as mono float at TUI_SOUND_RATE
// (miniaudio converts to what the device takes). A failure is final:
// there is no sound for the rest of the run.
static void open_device(void) {
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 1;
  config.sampleRate = TUI_SOUND_RATE;
  config.dataCallback = mix_voices;
  if (ma_device_init(NULL, &config, &device) == MA_SUCCESS) {
    device_open = true;
  } else {
    device_failed = true;
  }
}

// Renders the job and hands it to a free voice, opening and starting the
// device as needed. Runs on the sound thread.
static void play_job(const PlayJob *job) {
  int16_t *samples = malloc(MAX_SAMPLES * sizeof(int16_t));
  if (samples == NULL) {
    return;
  }
  const int count =
      tui_sound_render(job->sound, job->tiles, job->seed, samples, MAX_SAMPLES);
  if (!device_open && !device_failed) {
    open_device();
  }
  if (!device_open) {
    free(samples);
    return;
  }
  int16_t *finished = NULL;
  ma_spinlock_lock(&voices_lock);
  for (int voice_idx = 0; voice_idx < MAX_VOICES; voice_idx++) {
    Voice *voice = &voices[voice_idx];
    if (voice->position >= voice->count) {
      finished = voice->samples;
      voice->samples = samples;
      voice->count = count;
      voice->position = 0;
      samples = NULL;
      break;
    }
  }
  ma_spinlock_unlock(&voices_lock);
  // Either the slot's old samples, or these when every voice was busy.
  free(finished);
  free(samples);
  if (!device_running && ma_device_start(&device) == MA_SUCCESS) {
    device_running = true;
  }
}

// Plays queued sounds, and stops the device once it has been idle for
// IDLE_STOP_NS.
static void *sound_thread(void *arg) {
  (void)arg;
  int64_t idle_since_ns = 0;
  cpthread_mutex_lock(&queue_mutex);
  while (!quitting) {
    if (queue_count > 0) {
      const PlayJob job = queue[0];
      queue_count--;
      memmove(&queue[0], &queue[1], (size_t)queue_count * sizeof(queue[0]));
      cpthread_mutex_unlock(&queue_mutex);
      play_job(&job);
      idle_since_ns = 0;
      cpthread_mutex_lock(&queue_mutex);
      continue;
    }
    if (!device_running) {
      cpthread_cond_wait(&queue_cond, &queue_mutex);
      continue;
    }
    const int64_t now_ns = ctimer_monotonic_ns();
    if (voices_playing()) {
      idle_since_ns = 0;
    } else if (idle_since_ns == 0) {
      idle_since_ns = now_ns;
    } else if (now_ns - idle_since_ns >= IDLE_STOP_NS) {
      cpthread_mutex_unlock(&queue_mutex);
      (void)ma_device_stop(&device);
      cpthread_mutex_lock(&queue_mutex);
      device_running = false;
      idle_since_ns = 0;
      continue;
    }
    TimeSpec deadline;
    ctimer_clock_gettime_realtime(&deadline);
    deadline.tv_nsec += IDLE_POLL_NS;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
    (void)cpthread_cond_timedwait(&queue_cond, &queue_mutex, &deadline);
  }
  cpthread_mutex_unlock(&queue_mutex);
  return NULL;
}

void tui_sound_init(void) {
  cpthread_mutex_init(&queue_mutex);
  cpthread_cond_init(&queue_cond);
  initialized = true;
}

void tui_sound_play(TuiSound sound, int tiles) {
  if (!initialized) {
    return;
  }
  const PlayJob job = {
      .sound = sound,
      .tiles = tiles,
      .seed = (uint32_t)ctime_get_current_time() ^
              (atomic_fetch_add(&play_count, 1) + 1) * 0x9e3779b9U,
  };
  cpthread_mutex_lock(&queue_mutex);
  if (!thread_started) {
    cpthread_create(&sound_thread_handle, sound_thread, NULL);
    thread_started = true;
  }
  if (queue_count < QUEUE_MAX) {
    queue[queue_count++] = job;
    cpthread_cond_signal(&queue_cond);
  }
  cpthread_mutex_unlock(&queue_mutex);
}

void tui_sound_shutdown(void) {
  if (!initialized) {
    return;
  }
  cpthread_mutex_lock(&queue_mutex);
  const bool started = thread_started;
  quitting = true;
  cpthread_cond_signal(&queue_cond);
  cpthread_mutex_unlock(&queue_mutex);
  if (started) {
    cpthread_join(sound_thread_handle);
  }
  if (device_open) {
    ma_device_uninit(&device);
    device_open = false;
  }
  for (int voice_idx = 0; voice_idx < MAX_VOICES; voice_idx++) {
    free(voices[voice_idx].samples);
    voices[voice_idx].samples = NULL;
    voices[voice_idx].count = 0;
    voices[voice_idx].position = 0;
  }
}
