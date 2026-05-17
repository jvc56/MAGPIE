// macOS camera backend: AVFoundation behind the C façade declared in
// cam.h. Pixel format is BGRA — what AVCaptureVideoDataOutput emits
// natively for kCVPixelFormatType_32BGRA, no extra YUV→RGB step.
//
// Buffer model: three slots (CAPTURE / READY / RENDER) plus a small
// pool of allocated buffers. The capture delegate copies each frame
// into the back slot, atomically swaps the back slot into READY, and
// retains the previous READY for recycling. The renderer pulls READY
// → RENDER, blits it, then releases it back into the pool. Renderer
// and capture never block each other.
//
// Memory: each slot holds an independently-allocated pixel buffer
// sized for the active capture resolution. We allocate up to three
// (capture-side back buffer, ready buffer, render-held buffer) and
// recycle. Reallocates only when the capture resolution changes.

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cam.h"

namespace {

struct CamBuffer {
  uint8_t *data = nullptr;
  size_t capacity = 0; // bytes
  int width = 0;
  int height = 0;
  int stride = 0;
  uint64_t timestamp_ns = 0;
  bool in_use = false; // owned by capture or renderer
};

constexpr int kPoolSize = 3;

struct CamState {
  AVCaptureSession *session = nil;
  AVCaptureDevice *device = nil;
  AVCaptureDeviceInput *input = nil;
  AVCaptureVideoDataOutput *output = nil;
  dispatch_queue_t queue = nullptr;
  id delegate = nil;

  pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  CamBuffer pool[kPoolSize];
  CamBuffer *ready = nullptr; // most-recently-captured frame, if any

  int target_w = 640;
  int target_h = 480;
  int target_fps = 30;

  char last_error[256] = {0};
  _Atomic int started = 0;
};

CamState g;

void set_error(const char *msg) {
  if (!msg) {
    g.last_error[0] = '\0';
    return;
  }
  snprintf(g.last_error, sizeof(g.last_error), "%s", msg);
}

// Pick a free buffer big enough to hold w*h*4 bytes. Reallocates if
// we have a free slot but it's the wrong size. Caller holds g.lock.
CamBuffer *acquire_buffer(int w, int h) {
  const size_t need = (size_t)w * (size_t)h * 4;
  for (int i = 0; i < kPoolSize; i++) {
    CamBuffer *b = &g.pool[i];
    if (b->in_use) {
      continue;
    }
    if (b->capacity < need) {
      free(b->data);
      b->data = (uint8_t *)malloc(need);
      if (!b->data) {
        b->capacity = 0;
        continue;
      }
      b->capacity = need;
    }
    b->in_use = true;
    return b;
  }
  return nullptr;
}

void release_buffer_locked(CamBuffer *b) {
  if (b) {
    b->in_use = false;
  }
}

} // namespace

@interface MagpieCamDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@end

@implementation MagpieCamDelegate
- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection {
  (void)output;
  (void)connection;
  CVImageBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!pixelBuffer) {
    return;
  }
  CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
  const int w = (int)CVPixelBufferGetWidth(pixelBuffer);
  const int h = (int)CVPixelBufferGetHeight(pixelBuffer);
  const int stride = (int)CVPixelBufferGetBytesPerRow(pixelBuffer);
  const uint8_t *src = (const uint8_t *)CVPixelBufferGetBaseAddress(pixelBuffer);
  const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
  const uint64_t ts_ns = (uint64_t)(CMTimeGetSeconds(pts) * 1e9);

  pthread_mutex_lock(&g.lock);
  CamBuffer *dst = acquire_buffer(w, h);
  if (dst && src) {
    // Tight-pack into our buffer regardless of CV stride.
    const int dst_stride = w * 4;
    for (int y = 0; y < h; y++) {
      memcpy(dst->data + (size_t)y * (size_t)dst_stride,
             src + (size_t)y * (size_t)stride, (size_t)dst_stride);
    }
    dst->width = w;
    dst->height = h;
    dst->stride = dst_stride;
    dst->timestamp_ns = ts_ns;
    // Swap the previous ready buffer back into the free pool so the
    // next capture has somewhere to land. The renderer may still be
    // holding a third buffer; that's why we have three slots.
    if (g.ready) {
      release_buffer_locked(g.ready);
    }
    g.ready = dst;
  }
  pthread_mutex_unlock(&g.lock);
  CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
}
@end

extern "C" {

int cam_init(int device_index) {
  (void)device_index; // single-camera v1
  @autoreleasepool {
    AVAuthorizationStatus auth =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (auth == AVAuthorizationStatusDenied ||
        auth == AVAuthorizationStatusRestricted) {
      set_error("camera permission denied — grant access in System "
                "Settings > Privacy & Security > Camera");
      return -1;
    }
    if (auth == AVAuthorizationStatusNotDetermined) {
      // Kick off the permission prompt asynchronously. The first
      // cam_start() may fail until the user approves; the next
      // attempt will succeed.
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                               completionHandler:^(BOOL granted) {
                                 (void)granted;
                               }];
    }
    g.device = [AVCaptureDevice
        defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera
                          mediaType:AVMediaTypeVideo
                           position:AVCaptureDevicePositionUnspecified];
    if (!g.device) {
      // Fall back to any video device.
      g.device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    }
    if (!g.device) {
      set_error("no camera device found");
      return -1;
    }
  }
  return 0;
}

int cam_start(int width, int height, int fps) {
  if (!g.device) {
    set_error("cam_init not called");
    return -1;
  }
  g.target_w = width;
  g.target_h = height;
  g.target_fps = fps;
  @autoreleasepool {
    NSError *error = nil;
    g.input = [AVCaptureDeviceInput deviceInputWithDevice:g.device
                                                    error:&error];
    if (!g.input) {
      set_error(error ? [[error localizedDescription] UTF8String]
                      : "device input creation failed");
      return -1;
    }
    g.session = [[AVCaptureSession alloc] init];
    [g.session beginConfiguration];
    // The TUI camera panel is only ~200×112 visible pixels, so we
    // always ask AVFoundation for the lowest-quality preset (352×288
    // on macOS). That cuts the per-frame BGRA buffer copy 4× vs
    // 640×480 and shrinks the input ncvisual_blit downscales from.
    (void)width; // kept in the API in case callers want larger
    (void)height;
    if ([g.session canSetSessionPreset:AVCaptureSessionPreset352x288]) {
      g.session.sessionPreset = AVCaptureSessionPreset352x288;
    } else if ([g.session canSetSessionPreset:AVCaptureSessionPreset640x480]) {
      g.session.sessionPreset = AVCaptureSessionPreset640x480;
    }
    if ([g.session canAddInput:g.input]) {
      [g.session addInput:g.input];
    }
    g.output = [[AVCaptureVideoDataOutput alloc] init];
    g.output.videoSettings = @{
      (id)kCVPixelBufferPixelFormatTypeKey :
          @(kCVPixelFormatType_32BGRA),
    };
    g.output.alwaysDiscardsLateVideoFrames = YES;
    g.queue = dispatch_queue_create("com.magpie.cam", DISPATCH_QUEUE_SERIAL);
    g.delegate = [[MagpieCamDelegate alloc] init];
    [g.output setSampleBufferDelegate:g.delegate queue:g.queue];
    if ([g.session canAddOutput:g.output]) {
      [g.session addOutput:g.output];
    }
    [g.session commitConfiguration];
    [g.session startRunning];
  }
  atomic_store(&g.started, 1);
  return 0;
}

int cam_get_frame(cam_frame_t *out) {
  if (!out) {
    return -1;
  }
  pthread_mutex_lock(&g.lock);
  CamBuffer *r = g.ready;
  if (!r) {
    pthread_mutex_unlock(&g.lock);
    return -1;
  }
  g.ready = nullptr;
  pthread_mutex_unlock(&g.lock);
  out->data = r->data;
  out->width = r->width;
  out->height = r->height;
  out->stride = r->stride;
  out->timestamp_ns = r->timestamp_ns;
  out->format = CAM_FORMAT_BGRA8888;
  // Stash the buffer pointer so release can find it. We park it in
  // a static; cam_release_frame is the only consumer and only one
  // frame can be "checked out" at a time.
  static CamBuffer *checked_out = nullptr;
  (void)checked_out;
  // Mark via a sentinel in stride's high bit? Simpler: rely on
  // pointer equality in cam_release_frame by scanning the pool.
  return 0;
}

void cam_release_frame(cam_frame_t *frame) {
  if (!frame || !frame->data) {
    return;
  }
  pthread_mutex_lock(&g.lock);
  for (int i = 0; i < kPoolSize; i++) {
    if (g.pool[i].data == frame->data) {
      release_buffer_locked(&g.pool[i]);
      break;
    }
  }
  pthread_mutex_unlock(&g.lock);
}

void cam_stop(void) {
  if (!atomic_load(&g.started)) {
    return;
  }
  @autoreleasepool {
    [g.session stopRunning];
    if (g.output) {
      [g.output setSampleBufferDelegate:nil queue:nullptr];
    }
    g.session = nil;
    g.input = nil;
    g.output = nil;
    g.delegate = nil;
  }
  if (g.queue) {
    g.queue = nullptr;
  }
  atomic_store(&g.started, 0);
}

void cam_shutdown(void) {
  cam_stop();
  pthread_mutex_lock(&g.lock);
  for (int i = 0; i < kPoolSize; i++) {
    free(g.pool[i].data);
    g.pool[i].data = nullptr;
    g.pool[i].capacity = 0;
    g.pool[i].in_use = false;
  }
  g.ready = nullptr;
  pthread_mutex_unlock(&g.lock);
  g.device = nil;
}

const char *cam_last_error(void) { return g.last_error; }

bool cam_is_available(void) {
  @autoreleasepool {
    AVCaptureDevice *d = [AVCaptureDevice
        defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera
                          mediaType:AVMediaTypeVideo
                           position:AVCaptureDevicePositionUnspecified];
    if (!d) {
      d = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    }
    return d != nil;
  }
}

} // extern "C"
