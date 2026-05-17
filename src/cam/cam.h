#ifndef MAGPIE_CAM_H
#define MAGPIE_CAM_H

// Tiny C façade over the platform-specific camera backend. The macOS
// implementation lives in cam_macos.mm and wraps AVFoundation; the
// Linux implementation is currently a no-op stub.
//
// Concurrency: the capture backend pushes frames on its own thread
// (AVFoundation's dispatch queue on macOS). The renderer thread
// calls cam_get_frame to retrieve the latest published frame — the
// backend triple-buffers internally, so the renderer never blocks
// capture and capture never blocks render.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CAM_FORMAT_BGRA8888 = 0,
  CAM_FORMAT_RGBA8888 = 1,
} cam_format_t;

typedef struct {
  const uint8_t *data; // borrowed: valid until cam_release_frame
  int width;
  int height;
  int stride; // bytes per row
  uint64_t timestamp_ns;
  cam_format_t format;
} cam_frame_t;

// Initialize the backend. device_index 0 = default / built-in.
// Returns 0 on success, -1 otherwise (cam_last_error() for detail).
int cam_init(int device_index);

// Start streaming at the requested resolution / framerate. Backend
// may choose the closest supported preset. Returns 0 on success.
int cam_start(int width, int height, int fps);

// Fetch the latest published frame. Non-blocking: returns -1 if no
// frame has been published yet. The returned frame is borrowed —
// the caller must call cam_release_frame after consuming it so the
// backend can recycle the buffer.
int cam_get_frame(cam_frame_t *out);

// Release a frame previously returned by cam_get_frame.
void cam_release_frame(cam_frame_t *frame);

void cam_stop(void);
void cam_shutdown(void);

// Human-readable string explaining the most recent failure. Lifetime
// is the backend's; do not free.
const char *cam_last_error(void);

// Whether the backend has at least one camera available without
// having to actually open it. Used by the TUI to decide whether to
// even reserve the camera panel.
bool cam_is_available(void);

#ifdef __cplusplus
}
#endif

#endif // MAGPIE_CAM_H
