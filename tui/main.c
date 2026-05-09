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

static void render_frame(struct ncplane *plane, uint64_t frame_idx,
                         uint32_t last_key) {
  ncplane_erase(plane);
  ncplane_set_fg_rgb8(plane, 255, 220, 80);
  ncplane_set_bg_rgb8(plane, 0, 24, 64);
  ncplane_putstr_yx(plane, 1, 2, "  MAGPIE TUI — phase 1 skeleton  ");

  ncplane_set_fg_rgb8(plane, 200, 200, 200);
  ncplane_set_bg_default(plane);
  char counter[80];
  const int counter_written = snprintf(counter, sizeof(counter),
                                       "frame %llu",
                                       (unsigned long long)frame_idx);
  if (counter_written > 0) {
    ncplane_putstr_yx(plane, 3, 2, counter);
  }

  char keyline[80];
  const int key_written = snprintf(keyline, sizeof(keyline),
                                   "last key id: 0x%08x",
                                   (unsigned)last_key);
  if (key_written > 0) {
    ncplane_putstr_yx(plane, 4, 2, keyline);
  }

  ncplane_set_fg_rgb8(plane, 120, 200, 120);
  ncplane_putstr_yx(plane, 6, 2, "press q, Q, or ESC to quit");
}

int main(void) {
  setlocale(LC_ALL, "");
  notcurses_options opts = {
      .flags = NCOPTION_SUPPRESS_BANNERS,
  };
  struct notcurses *nc = notcurses_core_init(&opts, NULL);
  if (nc == NULL) {
    return 1;
  }
  struct ncplane *std_plane = notcurses_stdplane(nc);

  uint64_t frame_idx = 0;
  uint32_t last_key = 0;
  bool running = true;
  while (running) {
    render_frame(std_plane, frame_idx, last_key);
    notcurses_render(nc);
    frame_idx++;

    const struct timespec wait = {.tv_sec = 0, .tv_nsec = FRAME_NS};
    ncinput input;
    const uint32_t key = notcurses_get(nc, &wait, &input);
    if (key == (uint32_t)-1) {
      running = false;
      break;
    }
    if (key == 0) {
      continue;
    }
    if (input.evtype == NCTYPE_RELEASE) {
      continue;
    }
    last_key = key;
    if (key == 'q' || key == 'Q' || key == NCKEY_ESC) {
      running = false;
    }
  }

  notcurses_stop(nc);
  return 0;
}
