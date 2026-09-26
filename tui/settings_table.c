#include "settings_table.h"

#include "config.h"
#include "game_state.h"
#include "lexicon_picker.h"
#include "theme.h"
#include "tui_ui_state.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
  ANALYSIS_TIME_MAX = 24 * 3600,
  THREADS_MAX = 256,
  SIM_PLIES_MAX = 1024,
  SIM_CANDIDATES_MIN = 2,
  SIM_CANDIDATES_MAX = 1024,
  SIM_CANDIDATES_STEP = 10,
  BORDER_MAX_PX = 6,
};

static const char *const scale_choices[] = {"1x", "2x"};
static const char *const subscript_choices[] = {"off", "nonzero", "all"};
static const char *const premium_choices[] = {"uppercase", "lowercase",
                                              "punctuation", "none"};
static const char *const blank_choices[] = {"lowercase", "uppercase"};
// Leading "?+" means blanks come first; the rest is the letter ordering
// ("alpha" = alphabetical, "vow+con" = vowels then consonants).
static const char *const rack_sort_choices[] = {"alpha+?", "?+alpha",
                                                "vow+con+?", "?+vow+con"};
static const char *const spoiler_choices[] = {"shown", "hidden"};
// Indexed by TuiAutoAnalyze.
static const char *const auto_analyze_choices[] = {"off", "kibitz", "sim"};
// Indexed by ThemeName.
static const char *const theme_choices[] = {"dark", "light", "dim",
                                            "high_contrast"};

#define CHOICES(arr) (arr), (int)(sizeof(arr) / sizeof((arr)[0]))

static const TuiSettingDef setting_defs[] = {
    {TUI_SETTING_SCALE, 1, "scale", "Scale",
     "Board size: 1x text, or 2x pixel tiles on terminals with graphics.",
     TUI_SETTING_KIND_CHOICE, CHOICES(scale_choices), 0, 1, 1, NULL, NULL},
    {TUI_SETTING_ANTIALIAS, 1, "antialias", "Antialias",
     "Smooth the edges of letters on 2x tiles.", TUI_SETTING_KIND_BOOL, NULL, 0,
     0, 1, 1, NULL, NULL},
    {TUI_SETTING_SUBSCRIPTS, 1, "subscripts", "Subscripts",
     "Show tile point values on 2x tiles: off, nonzero, or all.",
     TUI_SETTING_KIND_CHOICE, CHOICES(subscript_choices), 0, 2, 1, NULL, NULL},
    {TUI_SETTING_BORDER, 1, "border", "Border",
     "Width of the grid lines between 2x tiles.", TUI_SETTING_KIND_INT, NULL, 0,
     0, BORDER_MAX_PX, 1, "px", "off"},
    {TUI_SETTING_PREMIUM_LABELS, 1, "premiums", "Premium labels",
     "How premium squares are labeled: TW, tw, punctuation, or none.",
     TUI_SETTING_KIND_CHOICE, CHOICES(premium_choices), 0, 3, 1, NULL, NULL},
    {TUI_SETTING_BLANKS, 1, "blanks", "Blanks",
     "Show played blanks as uppercase (tinted) or lowercase.",
     TUI_SETTING_KIND_CHOICE, CHOICES(blank_choices), 0, 1, 1, NULL, NULL},
    {TUI_SETTING_RACK_SORT, 2, "racksort", "Rack sort",
     "Tile order in racks and leaves; ? marks where blanks go.",
     TUI_SETTING_KIND_CHOICE, CHOICES(rack_sort_choices), 0, 3, 1, NULL, NULL},
    {TUI_SETTING_SPOILERS, 4, "spoilers", "Upcoming moves",
     "Hidden: stepping through History shows each turn's rack but not its "
     "move, nor any later turn, until you step past it.",
     TUI_SETTING_KIND_CHOICE, CHOICES(spoiler_choices), 0, 1, 1, NULL, NULL},
    {TUI_SETTING_SIM_PLIES, 5, "simplies", "Sim plies",
     "How many turns ahead each simulation looks.", TUI_SETTING_KIND_INT, NULL,
     0, 1, SIM_PLIES_MAX, 1, "plies", NULL},
    {TUI_SETTING_SIM_CANDIDATES, 5, "simcands", "Sim candidates",
     "How many candidate moves each simulation compares.", TUI_SETTING_KIND_INT,
     NULL, 0, SIM_CANDIDATES_MIN, SIM_CANDIDATES_MAX, SIM_CANDIDATES_STEP,
     "moves", NULL},
    {TUI_SETTING_AUTO_ANALYZE, 5, "autoanalyze", "Auto-analyze",
     "Analyze each turn you select in History: rank its moves (kibitz), or "
     "simulate it (solving endgames and pre-endgames instead).",
     TUI_SETTING_KIND_CHOICE, CHOICES(auto_analyze_choices), 0, 2, 1, NULL,
     NULL},
    {TUI_SETTING_ANALYSIS_TIME, 5, "analysistime", "Time limit",
     "Stop a sim or solve after this many seconds; none runs until you "
     "stop it.",
     TUI_SETTING_KIND_INT, NULL, 0, 0, ANALYSIS_TIME_MAX, 10, "s", "none"},
    {TUI_SETTING_LANGUAGE, TUI_SETTING_PANEL_GENERAL, "language", "Language",
     "The language to play in; picks its first lexicon. Applies from the "
     "next game.",
     TUI_SETTING_KIND_LANGUAGE, NULL, 0, 0, 0, 1, NULL, NULL},
    {TUI_SETTING_LEXICON, TUI_SETTING_PANEL_GENERAL, "lexicon", "Lexicon",
     "The word list for the language. Applies from the next game.",
     TUI_SETTING_KIND_LEXICON, NULL, 0, 0, 0, 1, NULL, NULL},
    {TUI_SETTING_THEME, TUI_SETTING_PANEL_GENERAL, "theme", "Theme",
     "Colors for the whole interface.", TUI_SETTING_KIND_CHOICE,
     CHOICES(theme_choices), 0, THEME_COUNT - 1, 1, NULL, NULL},
    {TUI_SETTING_RIT, TUI_SETTING_PANEL_GENERAL, "rit", "RIT",
     "Load the rack info table: faster move generation, more memory. "
     "Applies from the next game.",
     TUI_SETTING_KIND_BOOL, NULL, 0, 0, 1, 1, NULL, NULL},
    {TUI_SETTING_THREADS, TUI_SETTING_PANEL_GENERAL, "threads", "Threads",
     "Threads for sims and solves; auto uses every core but one, leaving "
     "it for the display.",
     TUI_SETTING_KIND_INT, NULL, 0, 0, THREADS_MAX, 1, "threads", "auto"},
};

#undef CHOICES

static const char *const bool_choices[] = {"off", "on"};

const TuiSettingDef *tui_setting_defs(int *count) {
  *count = (int)(sizeof(setting_defs) / sizeof(setting_defs[0]));
  return setting_defs;
}

const TuiSettingDef *tui_setting_def(TuiSettingId id) {
  int count = 0;
  const TuiSettingDef *defs = tui_setting_defs(&count);
  for (int def_idx = 0; def_idx < count; def_idx++) {
    if (defs[def_idx].id == id) {
      return &defs[def_idx];
    }
  }
  return NULL;
}

const char *tui_setting_panel_title(int panel) {
  switch (panel) {
  case 1:
    return "Board";
  case 2:
    return "Rack";
  case 3:
    return "Bag";
  case 4:
    return "History";
  case 5:
    return "Analysis";
  default:
    return "General";
  }
}

// The lexica under data/lexica, grouped by language; loaded on first
// use. NULL when there are none.
static const LexiconList *installed_lexica(void) {
  static LexiconList *lexica = NULL;
  static bool loaded = false;
  if (!loaded) {
    lexica = tui_lexicon_list_load();
    loaded = true;
  }
  return lexica;
}

static bool is_lexicon_kind(const TuiSettingDef *def) {
  return def->kind == TUI_SETTING_KIND_LANGUAGE ||
         def->kind == TUI_SETTING_KIND_LEXICON;
}

const TuiSettingDef *tui_setting_resolve(const char *typed, int len) {
  if (len <= 0) {
    return NULL;
  }
  int count = 0;
  const TuiSettingDef *defs = tui_setting_defs(&count);
  const TuiSettingDef *prefix_match = NULL;
  int prefix_matches = 0;
  for (int def_idx = 0; def_idx < count; def_idx++) {
    const char *key = defs[def_idx].key;
    if ((int)strlen(key) < len || strncasecmp(key, typed, (size_t)len) != 0) {
      continue;
    }
    if ((int)strlen(key) == len) {
      return &defs[def_idx];
    }
    prefix_match = &defs[def_idx];
    prefix_matches++;
  }
  return prefix_matches == 1 ? prefix_match : NULL;
}

int tui_setting_get(const TuiGameState *state, TuiSettingId id) {
  switch (id) {
  case TUI_SETTING_SCALE:
    return state->board_scale >= 2 ? 1 : 0;
  case TUI_SETTING_ANTIALIAS:
    return state->antialias ? 1 : 0;
  case TUI_SETTING_SUBSCRIPTS:
    return (int)state->score_subscripts;
  case TUI_SETTING_BORDER:
    return state->border_thickness;
  case TUI_SETTING_PREMIUM_LABELS:
    return (int)state->premium_labels;
  case TUI_SETTING_BLANKS:
    return state->blank_uppercase ? 1 : 0;
  case TUI_SETTING_RACK_SORT:
    return (int)state->rack_sort;
  case TUI_SETTING_SPOILERS:
    return state->hide_spoilers ? 1 : 0;
  case TUI_SETTING_SIM_PLIES:
    return state->sim_plies;
  case TUI_SETTING_SIM_CANDIDATES:
    return state->sim_candidates;
  case TUI_SETTING_AUTO_ANALYZE:
    return state->auto_analyze;
  case TUI_SETTING_ANALYSIS_TIME:
    return state->analysis_time_limit;
  case TUI_SETTING_THREADS:
    return state->thread_limit;
  case TUI_SETTING_LANGUAGE:
  case TUI_SETTING_LEXICON: {
    const LexiconList *lexica = installed_lexica();
    return lexica != NULL
               ? tui_lexicon_list_find(lexica, state->pending_lexicon)
               : -1;
  }
  case TUI_SETTING_THEME:
    return (int)state->theme;
  case TUI_SETTING_RIT:
    return state->pending_load_rit ? 1 : 0;
  case TUI_SETTING_COUNT:
  default:
    return 0;
  }
}

const char *tui_setting_unavailable_reason(const TuiGameState *state,
                                           const TuiSession *session,
                                           TuiSettingId id) {
  const bool scale_available =
      session->pixel_supported && session->font_available;
  switch (id) {
  case TUI_SETTING_SCALE:
    return scale_available ? NULL
                           : "2x needs a terminal with pixel graphics and a "
                             "tile font.";
  case TUI_SETTING_ANTIALIAS:
  case TUI_SETTING_SUBSCRIPTS:
  case TUI_SETTING_BORDER:
    if (!scale_available) {
      return "Only for 2x tiles, which this terminal can't show.";
    }
    return state->board_scale >= 2 ? NULL
                                   : "Only for 2x tiles; set Scale to 2x.";
  case TUI_SETTING_LANGUAGE:
  case TUI_SETTING_LEXICON:
    return installed_lexica() == NULL ? "No lexica installed in data/lexica."
                                      : NULL;
  default:
    return NULL;
  }
}

void tui_setting_values(const TuiSettingDef *def, TuiSettingValues *out) {
  out->count = 0;
  if (def->kind == TUI_SETTING_KIND_INT) {
    return;
  }
  if (is_lexicon_kind(def)) {
    const LexiconList *lexica = installed_lexica();
    const int lexicon_count =
        lexica != NULL ? tui_lexicon_list_count(lexica) : 0;
    char last_language[TUI_SETTING_VALUE_NAME_MAX] = "";
    for (int idx = 0;
         idx < lexicon_count && out->count < TUI_SETTING_MAX_VALUES; idx++) {
      char *name = out->names[out->count];
      if (def->kind == TUI_SETTING_KIND_LEXICON) {
        (void)tui_lexicon_list_name(lexica, idx, name,
                                    TUI_SETTING_VALUE_NAME_MAX);
      } else {
        // Lexica of a language are contiguous; list each language once,
        // as its first lexicon.
        (void)tui_lexicon_list_language_name(lexica, idx, name,
                                             TUI_SETTING_VALUE_NAME_MAX);
        if (strcmp(name, last_language) == 0) {
          continue;
        }
        (void)snprintf(last_language, sizeof(last_language), "%s", name);
      }
      out->values[out->count++] = idx;
    }
    return;
  }
  const bool is_bool = def->kind == TUI_SETTING_KIND_BOOL;
  const int count = is_bool ? 2 : def->choice_count;
  for (int idx = 0; idx < count && idx < TUI_SETTING_MAX_VALUES; idx++) {
    (void)snprintf(out->names[idx], TUI_SETTING_VALUE_NAME_MAX, "%s",
                   is_bool ? bool_choices[idx] : def->choices[idx]);
    out->values[idx] = idx;
  }
  out->count = count;
}

void tui_setting_format(const TuiSettingDef *def, int value, char *out,
                        size_t out_size) {
  if (def->kind == TUI_SETTING_KIND_INT) {
    if (value == 0 && def->zero_label != NULL) {
      (void)snprintf(out, out_size, "%s", def->zero_label);
    } else if (def->unit != NULL &&
               (strcmp(def->unit, "px") == 0 || strcmp(def->unit, "s") == 0)) {
      (void)snprintf(out, out_size, "%d%s", value, def->unit);
    } else {
      (void)snprintf(out, out_size, "%d", value);
    }
    return;
  }
  (void)snprintf(out, out_size, "?");
  if (is_lexicon_kind(def)) {
    const LexiconList *lexica = installed_lexica();
    if (lexica != NULL) {
      if (def->kind == TUI_SETTING_KIND_LEXICON) {
        (void)tui_lexicon_list_name(lexica, value, out, out_size);
      } else {
        (void)tui_lexicon_list_language_name(lexica, value, out, out_size);
      }
    }
    return;
  }
  TuiSettingValues values;
  tui_setting_values(def, &values);
  if (value >= 0 && value < values.count) {
    (void)snprintf(out, out_size, "%s", values.names[value]);
  }
}

void tui_setting_describe_values(const TuiSettingDef *def, char *out,
                                 size_t out_size) {
  if (def->kind == TUI_SETTING_KIND_INT) {
    char zero_note[32] = "";
    if (def->zero_label != NULL) {
      (void)snprintf(zero_note, sizeof(zero_note), " (0 = %s)",
                     def->zero_label);
    }
    (void)snprintf(out, out_size, "%d-%d %s%s", def->min, def->max,
                   def->unit != NULL ? def->unit : "", zero_note);
    return;
  }
  // The installed lexica are too many to list inline; "/set lexicon "
  // lists them as values.
  if (def->kind == TUI_SETTING_KIND_LEXICON) {
    (void)snprintf(out, out_size, "an installed lexicon");
    return;
  }
  if (def->kind == TUI_SETTING_KIND_LANGUAGE) {
    (void)snprintf(out, out_size, "an installed language");
    return;
  }
  TuiSettingValues values;
  tui_setting_values(def, &values);
  size_t used = 0;
  out[0] = '\0';
  for (int value_idx = 0; value_idx < values.count && used < out_size;
       value_idx++) {
    const int written =
        snprintf(out + used, out_size - used, "%s%s",
                 value_idx > 0 ? " | " : "", values.names[value_idx]);
    if (written < 0) {
      break;
    }
    used += (size_t)written;
  }
}

// The value whose name is `typed` (its first `len` characters, any case),
// or else the only one it's a prefix of; -1 when none or several.
static int find_value(const TuiSettingValues *values, const char *typed,
                      size_t len) {
  int match = -1;
  int matches = 0;
  for (int value_idx = 0; value_idx < values->count; value_idx++) {
    const char *name = values->names[value_idx];
    if (strlen(name) == len && strncasecmp(name, typed, len) == 0) {
      return value_idx;
    }
    if (strlen(name) > len && strncasecmp(name, typed, len) == 0) {
      match = value_idx;
      matches++;
    }
  }
  return matches == 1 ? match : -1;
}

const char *tui_setting_complete_value(const TuiSettingDef *def,
                                       const char *typed, int len) {
  static TuiSettingValues values;
  if (def->kind == TUI_SETTING_KIND_INT) {
    return NULL;
  }
  tui_setting_values(def, &values);
  const int found = find_value(&values, typed, (size_t)len);
  return found >= 0 ? values.names[found] : NULL;
}

bool tui_setting_parse(const TuiSettingDef *def, const char *text,
                       int *out_value, char *err, size_t err_size) {
  char described[128];
  tui_setting_describe_values(def, described, sizeof(described));
  if (text == NULL || text[0] == '\0') {
    (void)snprintf(err, err_size, "%s: expected %s", def->key, described);
    return false;
  }
  if (def->kind == TUI_SETTING_KIND_INT) {
    if (def->zero_label != NULL && strcasecmp(text, def->zero_label) == 0) {
      *out_value = 0;
      return true;
    }
    char *end = NULL;
    const long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < def->min || parsed > def->max) {
      (void)snprintf(err, err_size, "%s: expected %s", def->key, described);
      return false;
    }
    *out_value = (int)parsed;
    return true;
  }
  if (def->kind == TUI_SETTING_KIND_BOOL) {
    static const char *const on_words[] = {"on", "true", "yes", "1"};
    static const char *const off_words[] = {"off", "false", "no", "0"};
    for (size_t word_idx = 0; word_idx < sizeof(on_words) / sizeof(on_words[0]);
         word_idx++) {
      if (strcasecmp(text, on_words[word_idx]) == 0) {
        *out_value = 1;
        return true;
      }
      if (strcasecmp(text, off_words[word_idx]) == 0) {
        *out_value = 0;
        return true;
      }
    }
  }
  TuiSettingValues values;
  tui_setting_values(def, &values);
  const int found = find_value(&values, text, strlen(text));
  if (found >= 0) {
    *out_value = values.values[found];
    return true;
  }
  (void)snprintf(err, err_size, "%s: expected %s", def->key, described);
  return false;
}

// Applies `value` (already in range) to the running game and records it
// in the config. Caller holds state->mutex.
static void apply_setting(TuiGameState *state, TuiSession *session,
                          TuiSettingId id, int value) {
  TuiConfig *cfg = &session->to_save;
  switch (id) {
  case TUI_SETTING_SCALE:
    state->board_scale = value + 1;
    cfg->board_scale = state->board_scale;
    cfg->board_scale_set = true;
    break;
  case TUI_SETTING_ANTIALIAS:
    state->antialias = value != 0;
    cfg->antialias = state->antialias;
    cfg->antialias_set = true;
    break;
  case TUI_SETTING_SUBSCRIPTS:
    state->score_subscripts = (TuiScoreSubscripts)value;
    cfg->score_subscripts = state->score_subscripts;
    cfg->score_subscripts_set = true;
    break;
  case TUI_SETTING_BORDER:
    state->border_thickness = value;
    cfg->border_thickness = value;
    cfg->border_thickness_set = true;
    break;
  case TUI_SETTING_PREMIUM_LABELS:
    state->premium_labels = (TuiPremiumLabels)value;
    cfg->premium_labels = state->premium_labels;
    cfg->premium_labels_set = true;
    break;
  case TUI_SETTING_BLANKS:
    state->blank_uppercase = value != 0;
    cfg->blank_uppercase = state->blank_uppercase;
    cfg->blank_uppercase_set = true;
    break;
  case TUI_SETTING_RACK_SORT:
    state->rack_sort = (TuiRackSort)value;
    cfg->rack_sort = state->rack_sort;
    cfg->rack_sort_set = true;
    break;
  case TUI_SETTING_SPOILERS:
    state->hide_spoilers = value != 0;
    cfg->hide_spoilers = state->hide_spoilers;
    cfg->hide_spoilers_set = true;
    break;
  case TUI_SETTING_SIM_PLIES:
    state->sim_plies = value;
    cfg->sim_plies = value;
    cfg->sim_plies_set = true;
    break;
  case TUI_SETTING_SIM_CANDIDATES:
    state->sim_candidates = value;
    cfg->sim_candidates = value;
    cfg->sim_candidates_set = true;
    break;
  case TUI_SETTING_AUTO_ANALYZE:
    state->auto_analyze = value;
    cfg->auto_analyze = value;
    cfg->auto_analyze_set = true;
    break;
  case TUI_SETTING_ANALYSIS_TIME:
    state->analysis_time_limit = value;
    cfg->analysis_time_limit = value;
    cfg->analysis_time_limit_set = true;
    break;
  case TUI_SETTING_THREADS:
    state->thread_limit = value;
    cfg->thread_limit = value;
    cfg->thread_limit_set = true;
    break;
  case TUI_SETTING_LANGUAGE:
  case TUI_SETTING_LEXICON: {
    // Takes effect from the next game ("Next game: lexicon A -> B").
    char name[TUI_LEXICON_NAME_MAX];
    const LexiconList *lexica = installed_lexica();
    if (lexica == NULL ||
        !tui_lexicon_list_name(lexica, value, name, sizeof(name))) {
      break;
    }
    (void)snprintf(state->pending_lexicon, sizeof(state->pending_lexicon), "%s",
                   name);
    (void)snprintf(session->chosen_lexicon, sizeof(session->chosen_lexicon),
                   "%s", name);
    (void)snprintf(cfg->lexicon, sizeof(cfg->lexicon), "%s", name);
    cfg->lexicon_set = true;
    break;
  }
  case TUI_SETTING_THEME:
    state->theme = (ThemeName)value;
    cfg->theme = state->theme;
    cfg->theme_set = true;
    break;
  case TUI_SETTING_RIT:
    // Takes effect on the next New Game; the pending-change banner shows
    // the divergence until then.
    cfg->load_rit = value != 0;
    cfg->load_rit_set = true;
    state->pending_load_rit = cfg->load_rit;
    break;
  case TUI_SETTING_COUNT:
  default:
    break;
  }
}

void tui_setting_set(TuiGameState *state, TuiSession *session, TuiSettingId id,
                     int value) {
  const TuiSettingDef *def = tui_setting_def(id);
  if (def == NULL) {
    return;
  }
  if (def->kind == TUI_SETTING_KIND_INT) {
    value = value < def->min ? def->min : value;
    value = value > def->max ? def->max : value;
  } else if (is_lexicon_kind(def)) {
    const LexiconList *lexica = installed_lexica();
    if (lexica == NULL || value < 0 ||
        value >= tui_lexicon_list_count(lexica)) {
      return;
    }
  } else if (value < def->min || value > def->max) {
    return;
  }
  pthread_mutex_lock(&state->mutex);
  apply_setting(state, session, id, value);
  pthread_mutex_unlock(&state->mutex);
  // A changed value can alter the board's look; invalidate the
  // pixel-blit caches keyed on render_version.
  atomic_fetch_add(&state->render_version, 1);
  if (!session->args.no_config) {
    tui_config_save(&session->to_save);
  }
}

void tui_setting_step(TuiGameState *state, TuiSession *session, TuiSettingId id,
                      int dir) {
  const TuiSettingDef *def = tui_setting_def(id);
  if (def == NULL) {
    return;
  }
  pthread_mutex_lock(&state->mutex);
  const int current = tui_setting_get(state, id);
  pthread_mutex_unlock(&state->mutex);
  int next = current + dir * def->step;
  if (def->kind == TUI_SETTING_KIND_LANGUAGE ||
      def->kind == TUI_SETTING_KIND_LEXICON) {
    const LexiconList *lexica = installed_lexica();
    if (lexica == NULL) {
      return;
    }
    const int from = current >= 0 ? current : 0;
    next = def->kind == TUI_SETTING_KIND_LANGUAGE
               ? tui_lexicon_list_step_language(lexica, from, dir)
               : tui_lexicon_list_step_same_language(lexica, from, dir);
  } else if (def->kind != TUI_SETTING_KIND_INT) {
    const int count = def->max - def->min + 1;
    next = def->min + ((current - def->min + dir) % count + count) % count;
  }
  tui_setting_set(state, session, id, next);
}

void tui_settings_snapshot(const TuiGameState *state,
                           int values[TUI_SETTING_COUNT]) {
  for (int id = 0; id < TUI_SETTING_COUNT; id++) {
    values[id] = tui_setting_get(state, (TuiSettingId)id);
  }
}

void tui_settings_restore(TuiGameState *state, TuiSession *session,
                          const int values[TUI_SETTING_COUNT]) {
  for (int id = 0; id < TUI_SETTING_COUNT; id++) {
    apply_setting(state, session, (TuiSettingId)id, values[id]);
  }
}

int tui_settings_rows(TuiSettingsRow rows[TUI_SETTINGS_MAX_ROWS]) {
  int count = 0;
  const TuiSettingDef *defs = tui_setting_defs(&count);
  int row_count = 0;
  int last_panel = -1;
  for (int def_idx = 0; def_idx < count; def_idx++) {
    if (defs[def_idx].panel != last_panel) {
      last_panel = defs[def_idx].panel;
      rows[row_count++] = (TuiSettingsRow){.kind = TUI_SETTINGS_ROW_HEADING,
                                           .panel = last_panel};
    }
    rows[row_count++] = (TuiSettingsRow){.kind = TUI_SETTINGS_ROW_SETTING,
                                         .id = defs[def_idx].id};
  }
  rows[row_count++] = (TuiSettingsRow){.kind = TUI_SETTINGS_ROW_BACK};
  return row_count;
}
