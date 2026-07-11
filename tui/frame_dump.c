#include "frame_dump.h"

#include "glyph_cache.h"
#include <notcurses/notcurses.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// ── Per-plane RGBA registry ───────────────────────────────────────────────
//
// Keyed by notcurses plane pointer. The board has BOARD_DIM*BOARD_DIM tile
// planes plus a handful of board-composite / rack / overlay planes, so a
// few hundred slots covers every live pixel plane with room to spare. The
// registry is touched only from the render/UI thread (blits during render,
// the dump just after notcurses_render), so no locking is needed.

typedef struct {
  struct ncplane *plane;
  uint8_t *rgba; // width*height*4, owned
  int width;
  int height;
} CapturedPlane;

#define FRAME_DUMP_MAX_PLANES 512

static CapturedPlane g_planes[FRAME_DUMP_MAX_PLANES];
static int g_plane_count;

static atomic_int g_dump_requested;

void tui_frame_dump_request(void) { atomic_store(&g_dump_requested, 1); }

bool tui_frame_dump_pending(void) {
  return atomic_load(&g_dump_requested) != 0;
}

void tui_frame_dump_capture(struct ncplane *plane, const uint8_t *rgba,
                            int width, int height) {
  if (plane == NULL || rgba == NULL || width <= 0 || height <= 0) {
    return;
  }
  const size_t bytes = (size_t)width * (size_t)height * 4;
  CapturedPlane *slot = NULL;
  for (int idx = 0; idx < g_plane_count; idx++) {
    if (g_planes[idx].plane == plane) {
      slot = &g_planes[idx];
      break;
    }
  }
  if (slot == NULL) {
    if (g_plane_count >= FRAME_DUMP_MAX_PLANES) {
      return; // registry full; drop (debug tooling, best-effort)
    }
    slot = &g_planes[g_plane_count++];
    slot->plane = plane;
    slot->rgba = NULL;
    slot->width = 0;
    slot->height = 0;
  }
  if (slot->width != width || slot->height != height || slot->rgba == NULL) {
    uint8_t *grown = (uint8_t *)realloc(slot->rgba, bytes);
    if (grown == NULL) {
      return; // keep the old copy rather than corrupt the slot
    }
    slot->rgba = grown;
    slot->width = width;
    slot->height = height;
  }
  memcpy(slot->rgba, rgba, bytes);
}

// ── Compositing ───────────────────────────────────────────────────────────

// Alpha-over `src` (w*h RGBA) onto `frame` (frame_w*frame_h RGBA, opaque)
// at pixel offset (dst_x, dst_y). Straight (non-premultiplied) alpha.
static void composite_over(uint8_t *frame, int frame_w, int frame_h,
                           const uint8_t *src, int src_w, int src_h, int dst_x,
                           int dst_y) {
  for (int row = 0; row < src_h; row++) {
    const int fy = dst_y + row;
    if (fy < 0 || fy >= frame_h) {
      continue;
    }
    const uint8_t *srow = src + (size_t)row * src_w * 4;
    uint8_t *frow = frame + (size_t)fy * frame_w * 4;
    for (int col = 0; col < src_w; col++) {
      const int fx = dst_x + col;
      if (fx < 0 || fx >= frame_w) {
        continue;
      }
      const uint8_t *sp = srow + (size_t)col * 4;
      const unsigned alpha = sp[3];
      if (alpha == 0) {
        continue;
      }
      uint8_t *fp = frow + (size_t)fx * 4;
      if (alpha == 255) {
        fp[0] = sp[0];
        fp[1] = sp[1];
        fp[2] = sp[2];
        fp[3] = 255;
        continue;
      }
      const unsigned inv = 255 - alpha;
      fp[0] = (uint8_t)((sp[0] * alpha + fp[0] * inv + 127) / 255);
      fp[1] = (uint8_t)((sp[1] * alpha + fp[1] * inv + 127) / 255);
      fp[2] = (uint8_t)((sp[2] * alpha + fp[2] * inv + 127) / 255);
      fp[3] = 255;
    }
  }
}

// ── Cell-text rasterization ───────────────────────────────────────────────
//
// Planes drawn with ncplane_putstr (panels, labels, status, borders) hold
// their content as notcurses cells, not RGBA. We rasterize them ourselves:
// for each non-blank cell, fill its background (when not the terminal
// default) and blit the cell's grapheme through a FreeType glyph cache at
// the cell's pixel position, in the cell's foreground color.

// Dedicated glyph cache, created lazily on first dump and kept for the
// process lifetime. Separate from the renderer's caches so a dump never
// perturbs live-render glyph state. NULL if no usable font was found.
static TuiGlyphCache *g_text_cache;
static bool g_text_cache_tried;

static TuiGlyphCache *text_glyph_cache(void) {
  if (!g_text_cache_tried) {
    g_text_cache_tried = true;
    char font_path[4096];
    if (tui_glyph_cache_resolve_font_path(font_path, sizeof(font_path))) {
      g_text_cache = tui_glyph_cache_create(font_path);
    }
  }
  return g_text_cache;
}

// Decode the first codepoint of a UTF-8 grapheme cluster. Combining marks
// in the cluster are ignored — the base character is all we render.
static uint32_t utf8_first_codepoint(const char *s) {
  const unsigned char *u = (const unsigned char *)s;
  if (u[0] < 0x80) {
    return u[0];
  }
  if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
    return (uint32_t)((u[0] & 0x1F) << 6 | (u[1] & 0x3F));
  }
  if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
    return (uint32_t)((u[0] & 0x0F) << 12 | (u[1] & 0x3F) << 6 | (u[2] & 0x3F));
  }
  if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
      (u[3] & 0xC0) == 0x80) {
    return (uint32_t)((u[0] & 0x07) << 18 | (u[1] & 0x3F) << 12 |
                      (u[2] & 0x3F) << 6 | (u[3] & 0x3F));
  }
  return 0;
}

// Fill an opaque rectangle (clipped to the frame).
static void fill_rect(uint8_t *frame, int frame_w, int frame_h, int x, int y,
                      int w, int h, unsigned r, unsigned g, unsigned b) {
  for (int row = 0; row < h; row++) {
    const int fy = y + row;
    if (fy < 0 || fy >= frame_h) {
      continue;
    }
    uint8_t *p = frame + ((size_t)fy * frame_w + x) * 4;
    for (int col = 0; col < w; col++) {
      const int fx = x + col;
      if (fx < 0 || fx >= frame_w) {
        p += 4;
        continue;
      }
      p[0] = (uint8_t)r;
      p[1] = (uint8_t)g;
      p[2] = (uint8_t)b;
      p[3] = 255;
      p += 4;
    }
  }
}

// Alpha-blend a glyph's coverage mask onto the frame in fg color. Blends
// against the existing destination pixel so anti-aliased edges feather
// into whatever the cell background or underlying plane painted.
static void blend_glyph(uint8_t *frame, int frame_w, int frame_h, int gx,
                        int gy, const TuiGlyph *glyph, unsigned r, unsigned g,
                        unsigned b) {
  if (glyph == NULL || glyph->width <= 0 || glyph->height <= 0) {
    return;
  }
  for (int row = 0; row < glyph->height; row++) {
    const int fy = gy + row;
    if (fy < 0 || fy >= frame_h) {
      continue;
    }
    const unsigned char *src = glyph->alpha + (size_t)row * glyph->width;
    for (int col = 0; col < glyph->width; col++) {
      const int fx = gx + col;
      if (fx < 0 || fx >= frame_w) {
        continue;
      }
      const unsigned alpha = src[col];
      if (alpha == 0) {
        continue;
      }
      uint8_t *p = frame + ((size_t)fy * frame_w + fx) * 4;
      const unsigned inv = 255 - alpha;
      p[0] = (uint8_t)((r * alpha + p[0] * inv + 127) / 255);
      p[1] = (uint8_t)((g * alpha + p[1] * inv + 127) / 255);
      p[2] = (uint8_t)((b * alpha + p[2] * inv + 127) / 255);
      p[3] = 255;
    }
  }
}

// Rasterize one plane's cell-text content into the frame. `cache` is sized
// to the cell. `baseline_off` is the pixel offset from a cell's top to the
// glyph baseline.
static void render_plane_cells(uint8_t *frame, int frame_w, int frame_h,
                               struct ncplane *plane, unsigned celldimx,
                               unsigned celldimy, int baseline_off,
                               const Theme *theme, TuiGlyphCache *cache) {
  unsigned rows = 0;
  unsigned cols = 0;
  ncplane_dim_yx(plane, &rows, &cols);
  int abs_y = 0;
  int abs_x = 0;
  ncplane_abs_yx(plane, &abs_y, &abs_x);
  for (unsigned cy = 0; cy < rows; cy++) {
    for (unsigned cx = 0; cx < cols; cx++) {
      uint16_t style = 0;
      uint64_t channels = 0;
      char *egc = ncplane_at_yx(plane, (int)cy, (int)cx, &style, &channels);
      if (egc == NULL) {
        continue;
      }
      const int left = (abs_x + (int)cx) * (int)celldimx;
      const int top = (abs_y + (int)cy) * (int)celldimy;

      if (!ncchannels_bg_default_p(channels)) {
        unsigned br = 0;
        unsigned bgc = 0;
        unsigned bb = 0;
        ncchannels_bg_rgb8(channels, &br, &bgc, &bb);
        fill_rect(frame, frame_w, frame_h, left, top, (int)celldimx,
                  (int)celldimy, br, bgc, bb);
      }

      const uint32_t codepoint = utf8_first_codepoint(egc);
      if (codepoint != 0 && codepoint != ' ') {
        unsigned fr = theme->fg.r;
        unsigned fg = theme->fg.g;
        unsigned fb = theme->fg.b;
        if (!ncchannels_fg_default_p(channels)) {
          ncchannels_fg_rgb8(channels, &fr, &fg, &fb);
        }
        const TuiGlyph *glyph = (style & NCSTYLE_BOLD)
                                    ? tui_glyph_cache_get_bold(cache, codepoint)
                                    : tui_glyph_cache_get(cache, codepoint);
        if (glyph != NULL) {
          const int gx = left + glyph->bearing_x;
          const int gy = top + baseline_off - glyph->bearing_y;
          blend_glyph(frame, frame_w, frame_h, gx, gy, glyph, fr, fg, fb);
        }
      }
      free(egc);
    }
  }
}

// ── PNG encoding ──────────────────────────────────────────────────────────

static void put_be32(uint8_t *dst, uint32_t value) {
  dst[0] = (uint8_t)(value >> 24);
  dst[1] = (uint8_t)(value >> 16);
  dst[2] = (uint8_t)(value >> 8);
  dst[3] = (uint8_t)value;
}

// Append one PNG chunk (length, type, data, CRC32 over type+data) to `fp`.
static bool write_chunk(FILE *fp, const char type[4], const uint8_t *data,
                        uint32_t len) {
  uint8_t header[8];
  put_be32(header, len);
  memcpy(header + 4, type, 4);
  if (fwrite(header, 1, 8, fp) != 8) {
    return false;
  }
  if (len > 0 && fwrite(data, 1, len, fp) != len) {
    return false;
  }
  uLong crc = crc32(0L, (const Bytef *)type, 4);
  if (len > 0) {
    crc = crc32(crc, (const Bytef *)data, len);
  }
  uint8_t crc_be[4];
  put_be32(crc_be, (uint32_t)crc);
  return fwrite(crc_be, 1, 4, fp) == 4;
}

// Encode an RGBA8 buffer (`w`*`h`) as a PNG at `path`. Uses filter type 0
// (None) on every scanline and zlib for the IDAT.
static bool write_png_rgba(const char *path, const uint8_t *rgba, int w,
                           int h) {
  // Raw image stream: one filter byte (0) per scanline, then the row.
  const size_t row_bytes = (size_t)w * 4;
  const size_t raw_len = (size_t)h * (row_bytes + 1);
  uint8_t *raw = (uint8_t *)malloc(raw_len);
  if (raw == NULL) {
    return false;
  }
  for (int row = 0; row < h; row++) {
    uint8_t *out_row = raw + (size_t)row * (row_bytes + 1);
    out_row[0] = 0; // filter: None
    memcpy(out_row + 1, rgba + (size_t)row * row_bytes, row_bytes);
  }

  uLong comp_cap = compressBound((uLong)raw_len);
  uint8_t *comp = (uint8_t *)malloc(comp_cap);
  if (comp == NULL) {
    free(raw);
    return false;
  }
  uLongf comp_len = comp_cap;
  const int zrc = compress2(comp, &comp_len, raw, (uLong)raw_len, 6);
  free(raw);
  if (zrc != Z_OK) {
    free(comp);
    return false;
  }

  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    free(comp);
    return false;
  }
  bool ok = true;
  static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  ok = ok && fwrite(signature, 1, 8, fp) == 8;

  uint8_t ihdr[13];
  put_be32(ihdr, (uint32_t)w);
  put_be32(ihdr + 4, (uint32_t)h);
  ihdr[8] = 8;  // bit depth
  ihdr[9] = 6;  // color type: RGBA
  ihdr[10] = 0; // compression
  ihdr[11] = 0; // filter
  ihdr[12] = 0; // interlace
  ok = ok && write_chunk(fp, "IHDR", ihdr, sizeof(ihdr));
  ok = ok && write_chunk(fp, "IDAT", comp, (uint32_t)comp_len);
  ok = ok && write_chunk(fp, "IEND", NULL, 0);

  free(comp);
  if (fclose(fp) != 0) {
    ok = false;
  }
  return ok;
}

bool tui_frame_dump_write(struct notcurses *nc, struct ncplane *std,
                          const Theme *theme, const char *out_path) {
  atomic_store(&g_dump_requested, 0);
  if (nc == NULL || std == NULL || theme == NULL) {
    return false;
  }

  unsigned pxy = 0;
  unsigned pxx = 0;
  unsigned celldimy = 0;
  unsigned celldimx = 0;
  ncplane_pixel_geom(std, &pxy, &pxx, &celldimy, &celldimx, NULL, NULL);
  if (pxy == 0 || pxx == 0 || celldimy == 0 || celldimx == 0) {
    return false; // no pixel geometry (terminal lacks bitmap support)
  }
  const int frame_w = (int)pxx;
  const int frame_h = (int)pxy;

  uint8_t *frame = (uint8_t *)malloc((size_t)frame_w * frame_h * 4);
  if (frame == NULL) {
    return false;
  }
  for (size_t px = 0; px < (size_t)frame_w * frame_h; px++) {
    frame[px * 4 + 0] = theme->bg.r;
    frame[px * 4 + 1] = theme->bg.g;
    frame[px * 4 + 2] = theme->bg.b;
    frame[px * 4 + 3] = 255;
  }

  // Set up the cell-text glyph cache, sized so a reference glyph's advance
  // matches the terminal's cell width — the bundled font and the terminal's
  // font differ, so we rescale to fit rather than assume cell == em metrics.
  TuiGlyphCache *cache = text_glyph_cache();
  int baseline_off = (int)celldimy * 8 / 10; // refined below from font ascent
  if (cache != NULL) {
    tui_glyph_cache_set_size(cache, (int)celldimy, /*antialias=*/true);
    const TuiGlyph *ref = tui_glyph_cache_get(cache, 'M');
    if (ref != NULL && ref->advance > 0 && (unsigned)ref->advance != celldimx) {
      int fitted = (int)celldimy * (int)celldimx / ref->advance;
      if (fitted < 6) {
        fitted = 6;
      }
      tui_glyph_cache_set_size(cache, fitted, /*antialias=*/true);
      ref = tui_glyph_cache_get(cache, 'M');
    }
    // Vertically center the cap-height band in the cell: baseline sits at
    // top + (cell + capheight)/2, with capheight ~= the 'M' bearing.
    if (ref != NULL && ref->bearing_y > 0) {
      baseline_off = ((int)celldimy + ref->bearing_y) / 2;
    }
  }

  // Walk the live plane stack bottom-to-top (painter's order) so overlapping
  // planes composite in true z-order regardless of the order they were
  // blitted. Pixel planes (in the captured-RGBA registry) are alpha-
  // composited from their buffers; every other plane is a cell-text plane
  // and is rasterized through the glyph cache.
  for (struct ncplane *plane = notcurses_bottom(nc); plane != NULL;
       plane = ncplane_above(plane)) {
    const CapturedPlane *slot = NULL;
    for (int idx = 0; idx < g_plane_count; idx++) {
      if (g_planes[idx].plane == plane && g_planes[idx].rgba != NULL) {
        slot = &g_planes[idx];
        break;
      }
    }
    if (slot != NULL) {
      int cell_y = 0;
      int cell_x = 0;
      ncplane_abs_yx(plane, &cell_y, &cell_x);
      composite_over(frame, frame_w, frame_h, slot->rgba, slot->width,
                     slot->height, cell_x * (int)celldimx,
                     cell_y * (int)celldimy);
    } else if (cache != NULL) {
      render_plane_cells(frame, frame_w, frame_h, plane, celldimx, celldimy,
                         baseline_off, theme, cache);
    }
  }

  const char *path = out_path;
  if (path == NULL) {
    path = getenv("MAGPIE_TUI_DUMP_PATH");
  }
  if (path == NULL) {
    path = "/tmp/magpie_tui_frame.png";
  }
  const bool ok = write_png_rgba(path, frame, frame_w, frame_h);
  free(frame);
  return ok;
}
