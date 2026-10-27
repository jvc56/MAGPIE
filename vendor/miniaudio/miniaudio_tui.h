// miniaudio as magpie_tui uses it (tui/sound.c): only the low-level
// playback device API. Include this rather than miniaudio.h, so the
// implementation (miniaudio.c) and its users agree on these options.
#ifndef MINIAUDIO_TUI_H
#define MINIAUDIO_TUI_H

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#include "miniaudio.h"

#endif
