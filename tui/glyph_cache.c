#include "glyph_cache.h"

#include <freetype/freetype.h>
#include <freetype/ftsynth.h>
#include <ft2build.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
  GLYPH_CACHE_CAPACITY = 256, // ASCII + a little headroom is plenty
};

struct TuiGlyphCache {
  FT_Library lib;
  FT_Face face;
  int pixel_size;
  bool antialias;
  TuiGlyph *slots[GLYPH_CACHE_CAPACITY];
  TuiGlyph *slots_bold[GLYPH_CACHE_CAPACITY];
};

bool tui_glyph_cache_resolve_font_path(char *out, size_t out_size) {
  if (out == NULL || out_size == 0) {
    return false;
  }
  out[0] = '\0';

  const char *env = getenv("MAGPIE_TUI_FONT");
  if (env != NULL && env[0] != '\0' && access(env, R_OK) == 0) {
    snprintf(out, out_size, "%s", env);
    return true;
  }

  // Bundled font lookups: same shape as find_data_paths in game_state.c
  // (which probes data/, ../data/, ./data/) so the binary works whether
  // it's invoked from the repo root or from bin/.
  static const char *bundled_candidates[] = {
      "tui/assets/magpie.ttf",
      "../tui/assets/magpie.ttf",
      "./tui/assets/magpie.ttf",
  };
  for (size_t idx = 0;
       idx < sizeof(bundled_candidates) / sizeof(bundled_candidates[0]);
       idx++) {
    if (access(bundled_candidates[idx], R_OK) == 0) {
      snprintf(out, out_size, "%s", bundled_candidates[idx]);
      return true;
    }
  }
  return false;
}

TuiGlyphCache *tui_glyph_cache_create(const char *font_path) {
  if (font_path == NULL || font_path[0] == '\0') {
    return NULL;
  }
  TuiGlyphCache *cache = (TuiGlyphCache *)calloc(1, sizeof(*cache));
  if (cache == NULL) {
    return NULL;
  }
  if (FT_Init_FreeType(&cache->lib) != 0) {
    free(cache);
    return NULL;
  }
  if (FT_New_Face(cache->lib, font_path, 0, &cache->face) != 0) {
    FT_Done_FreeType(cache->lib);
    free(cache);
    return NULL;
  }
  // FT defaults to a Unicode charmap when one is present, but Nerd Font
  // variants ship multiple cmaps and the autoselect occasionally lands
  // on a non-Unicode one — every ASCII FT_Load_Char then resolves to
  // glyph 0 (.notdef), which renders as an empty bitmap and the tile
  // letters silently vanish. Force Unicode explicitly so the lookup
  // always works regardless of cmap order in the file.
  FT_Select_Charmap(cache->face, FT_ENCODING_UNICODE);
  cache->pixel_size = 0;
  cache->antialias = true;
  return cache;
}

static void free_slot_in(TuiGlyph **slots, int idx) {
  TuiGlyph *g = slots[idx];
  if (g == NULL) {
    return;
  }
  free(g->alpha);
  free(g);
  slots[idx] = NULL;
}

static void drop_all_slots(TuiGlyphCache *cache) {
  for (int idx = 0; idx < GLYPH_CACHE_CAPACITY; idx++) {
    free_slot_in(cache->slots, idx);
    free_slot_in(cache->slots_bold, idx);
  }
}

void tui_glyph_cache_destroy(TuiGlyphCache *cache) {
  if (cache == NULL) {
    return;
  }
  drop_all_slots(cache);
  if (cache->face != NULL) {
    FT_Done_Face(cache->face);
  }
  if (cache->lib != NULL) {
    FT_Done_FreeType(cache->lib);
  }
  free(cache);
}

void tui_glyph_cache_set_size(TuiGlyphCache *cache, int pixel_size,
                              bool antialias) {
  if (cache == NULL || pixel_size <= 0) {
    return;
  }
  if (cache->pixel_size == pixel_size && cache->antialias == antialias) {
    return;
  }
  // Resize via the pixel-height API; pixel_width=0 lets FreeType pick the
  // proportional width. Failure here leaves the cache size unchanged so
  // the next render keeps using whatever last worked.
  if (FT_Set_Pixel_Sizes(cache->face, 0, (FT_UInt)pixel_size) != 0) {
    return;
  }
  drop_all_slots(cache);
  cache->pixel_size = pixel_size;
  cache->antialias = antialias;
}

void tui_glyph_cache_reset(TuiGlyphCache *cache) {
  if (cache == NULL) {
    return;
  }
  drop_all_slots(cache);
  // Force the next set_size to actually reapply the size — clear our
  // memo so we don't skip the FT call thinking it's already set.
  cache->pixel_size = 0;
}

static const TuiGlyph *cache_get_styled(TuiGlyphCache *cache,
                                        uint32_t codepoint, bool bold) {
  if (cache == NULL || codepoint >= GLYPH_CACHE_CAPACITY ||
      cache->pixel_size <= 0) {
    return NULL;
  }
  TuiGlyph **slots = bold ? cache->slots_bold : cache->slots;
  if (slots[codepoint] != NULL) {
    return slots[codepoint];
  }

  // FT_LOAD_DEFAULT lets FreeType use bytecode hints if the font has
  // them and the auto-hinter otherwise — what most users would call
  // "good defaults". The AA toggle picks NORMAL vs MONO rendering;
  // MONO returns 1-bit-per-pixel bitmaps which we expand to 0/255
  // alpha for a uniform compositing path.
  FT_Int32 load_flags = FT_LOAD_DEFAULT;
  if (FT_Load_Char(cache->face, codepoint, load_flags) != 0) {
    return NULL;
  }
  if (bold) {
    // Outline-level emboldening: thickens the actual glyph outline
    // before rasterization, so the result has the proper terminals
    // and curve weights of a bold stroke rather than a flat
    // bitmap dilation. Default emboldening strength is proportional
    // to the loaded pixel size.
    FT_GlyphSlot_Embolden(cache->face->glyph);
  }
  FT_Render_Mode render_mode =
      cache->antialias ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO;
  if (FT_Render_Glyph(cache->face->glyph, render_mode) != 0) {
    return NULL;
  }
  FT_GlyphSlot slot = cache->face->glyph;
  const int width = (int)slot->bitmap.width;
  const int height = (int)slot->bitmap.rows;

  TuiGlyph *g = (TuiGlyph *)calloc(1, sizeof(*g));
  if (g == NULL) {
    return NULL;
  }
  g->width = width;
  g->height = height;
  g->bearing_x = slot->bitmap_left;
  g->bearing_y = slot->bitmap_top;
  g->advance = (int)(slot->advance.x >> 6); // 26.6 fixed -> pixels

  if (width > 0 && height > 0) {
    g->alpha = (unsigned char *)malloc((size_t)width * (size_t)height);
    if (g->alpha == NULL) {
      free(g);
      return NULL;
    }
    if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
      // Pre-aligned 8-bit alpha; bitmap.pitch may be > width if FT
      // padded the row, so copy row by row.
      for (int row = 0; row < height; row++) {
        memcpy(g->alpha + (size_t)row * width,
               slot->bitmap.buffer + (ptrdiff_t)row * slot->bitmap.pitch,
               (size_t)width);
      }
    } else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
      // 1 bit per pixel, MSB-first within each byte. Expand to 0/255.
      for (int row = 0; row < height; row++) {
        const unsigned char *src =
            slot->bitmap.buffer + (ptrdiff_t)row * slot->bitmap.pitch;
        unsigned char *dst = g->alpha + (size_t)row * width;
        for (int col = 0; col < width; col++) {
          const int byte = col >> 3;
          const int bit = 7 - (col & 7);
          dst[col] = ((src[byte] >> bit) & 1) ? 255 : 0;
        }
      }
    } else {
      // Some other pixel_mode we don't expect; bail safely.
      free(g->alpha);
      free(g);
      return NULL;
    }
  }

  slots[codepoint] = g;
  return g;
}

const TuiGlyph *tui_glyph_cache_get(TuiGlyphCache *cache, uint32_t codepoint) {
  return cache_get_styled(cache, codepoint, /*bold=*/false);
}

const TuiGlyph *tui_glyph_cache_get_bold(TuiGlyphCache *cache,
                                         uint32_t codepoint) {
  return cache_get_styled(cache, codepoint, /*bold=*/true);
}
