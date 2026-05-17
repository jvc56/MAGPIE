// Camera backend stub for platforms without a native implementation.
// V4L2 support on Linux is a future PR; this file lets cam.h compile
// and link so the TUI's call-sites don't need #ifdef guards.

#include "cam.h"

#include <stdbool.h>

static const char *kStubMsg = "camera not yet implemented on this platform";

int cam_init(int device_index) {
  (void)device_index;
  return -1;
}

int cam_start(int width, int height, int fps) {
  (void)width;
  (void)height;
  (void)fps;
  return -1;
}

int cam_get_frame(cam_frame_t *out) {
  (void)out;
  return -1;
}

void cam_release_frame(cam_frame_t *frame) { (void)frame; }

void cam_stop(void) {}

void cam_shutdown(void) {}

const char *cam_last_error(void) { return kStubMsg; }

bool cam_is_available(void) { return false; }
