#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <notcurses/notcurses.h>

enum {
  TARGET_FPS = 60,
};

static const long FRAME_NS = 1000000000L / TARGET_FPS;

static void render_frame(struct ncplane *plane, uint64_t frame_idx) {
  ncplane_erase(plane);
  ncplane_putstr_yx(plane, 0, 0, "MAGPIE TUI — phase 1 skeleton");
  char counter[64];
  const int written = snprintf(counter, sizeof(counter),
                               "frame %llu  (q or ESC to quit)",
                               (unsigned long long)frame_idx);
  if (written > 0) {
    ncplane_putstr_yx(plane, 2, 0, counter);
  }
}

int main(void) {
  setlocale(LC_ALL, "");
  struct notcurses *nc = notcurses_core_init(NULL, NULL);
  if (nc == NULL) {
    return 1;
  }
  struct ncplane *std_plane = notcurses_stdplane(nc);

  uint64_t frame_idx = 0;
  bool running = true;
  while (running) {
    struct timespec frame_start;
    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    ncinput input;
    uint32_t key;
    while ((key = notcurses_get_nblock(nc, &input)) != 0) {
      if (key == (uint32_t)-1) {
        running = false;
        break;
      }
      if (input.evtype == NCTYPE_RELEASE) {
        continue;
      }
      if (input.id == 'q' || input.id == 'Q' || input.id == NCKEY_ESC) {
        running = false;
      }
    }

    render_frame(std_plane, frame_idx);
    notcurses_render(nc);
    frame_idx++;

    struct timespec frame_end;
    clock_gettime(CLOCK_MONOTONIC, &frame_end);
    const long elapsed_ns =
        (frame_end.tv_sec - frame_start.tv_sec) * 1000000000L +
        (frame_end.tv_nsec - frame_start.tv_nsec);
    const long sleep_ns = FRAME_NS - elapsed_ns;
    if (sleep_ns > 0) {
      const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = sleep_ns};
      nanosleep(&sleep_ts, NULL);
    }
  }

  notcurses_stop(nc);
  return 0;
}
