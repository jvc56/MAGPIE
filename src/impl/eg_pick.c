#include "eg_pick.h"

#include "../compat/ctime.h"
#include "../def/equity_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"
#include "../util/io_util.h"
#include "gameplay.h"
#include "move_gen.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  EG_PICK_MOVE_LIST_CAPACITY = 20000,
  EG_PICK_MAX_CANDIDATES = 64,
  EG_PICK_NUM_TAGS = 2,
  EG_PICK_TAG_NO = 0,
  EG_PICK_TAG_OO = 1,
  EG_PICK_MAX_LEAVE_SIZE = RACK_SIZE,
  EG_PICK_LINE_LENGTH = 256,
  EG_PICK_NAME_LENGTH = 64,
};

typedef enum {
  EG_PICK_MODE_FULL,
  EG_PICK_MODE_NO_OBS,
  EG_PICK_MODE_RULE,
  EG_PICK_MODE_STATIC,
  EG_PICK_MODE_STATIC_BEST,
} eg_pick_mode_t;

typedef struct EgPickWeights {
  double out;
  double constant[EG_PICK_NUM_TAGS];
  double obo;
  double obs_no;
  double face[EG_PICK_NUM_TAGS];
  double leave_size[EG_PICK_NUM_TAGS][EG_PICK_MAX_LEAVE_SIZE + 1];
  // w[s] + w[s|tag] for each 1- and 2-tile sub-multiset s.
  double single[EG_PICK_NUM_TAGS][MAX_ALPHABET_SIZE];
  double pair[EG_PICK_NUM_TAGS][MAX_ALPHABET_SIZE][MAX_ALPHABET_SIZE];
} EgPickWeights;

struct EgPick {
  int num_candidates;
  eg_pick_mode_t mode;
  EgPickWeights weights;
  MoveList *moves;
  MoveList *replies;
  Game *after;
  Move candidates[EG_PICK_MAX_CANDIDATES];
};

static atomic_int eg_pick_suppressed;
static atomic_llong eg_pick_total_picks;
static atomic_llong eg_pick_total_candidates;
static atomic_llong eg_pick_total_reply_movegens;
static atomic_llong eg_pick_total_nanoseconds;

static int eg_pick_requested_candidates(void) {
  const char *value = getenv("MAGPIE_EGPICK_N");
  if (value == NULL) {
    return 0;
  }
  const int num_candidates = atoi(value);
  if (num_candidates > EG_PICK_MAX_CANDIDATES) {
    return EG_PICK_MAX_CANDIDATES;
  }
  return num_candidates > 0 ? num_candidates : 0;
}

bool eg_pick_enabled(void) { return eg_pick_requested_candidates() > 0; }

static eg_pick_mode_t eg_pick_requested_mode(void) {
  const char *value = getenv("MAGPIE_EGPICK_MODE");
  if (value == NULL || strcmp(value, "full") == 0) {
    return EG_PICK_MODE_FULL;
  }
  if (strcmp(value, "noobs") == 0) {
    return EG_PICK_MODE_NO_OBS;
  }
  if (strcmp(value, "rule") == 0) {
    return EG_PICK_MODE_RULE;
  }
  if (strcmp(value, "staticbest") == 0) {
    return EG_PICK_MODE_STATIC_BEST;
  }
  if (strcmp(value, "static") == 0) {
    return EG_PICK_MODE_STATIC;
  }
  log_fatal("MAGPIE_EGPICK_MODE must be full, noobs, rule or static, not %s",
            value);
  return EG_PICK_MODE_FULL;
}

static int eg_pick_parse_tag(const char *tag) {
  if (strcmp(tag, "no") == 0) {
    return EG_PICK_TAG_NO;
  }
  if (strcmp(tag, "oo") == 0) {
    return EG_PICK_TAG_OO;
  }
  log_fatal("egpick: unknown tag %s", tag);
  return EG_PICK_TAG_NO;
}

static MachineLetter eg_pick_parse_letter(const LetterDistribution *ld,
                                          char letter) {
  if (letter == '?') {
    return BLANK_MACHINE_LETTER;
  }
  const char letter_string[2] = {letter, '\0'};
  const MachineLetter ml = ld_hl_to_ml(ld, letter_string);
  if (ml == INVALID_LETTER || ml >= MAX_ALPHABET_SIZE) {
    log_fatal("egpick: unknown letter %c", letter);
  }
  return ml;
}

// One "name<TAB>value" line. A leave term without a tag counts for both tags.
static void eg_pick_add_weight(EgPickWeights *weights,
                               const LetterDistribution *ld, const char *name,
                               double value) {
  char base[EG_PICK_NAME_LENGTH];
  const char *bar = strchr(name, '|');
  const size_t base_length = bar == NULL ? strlen(name) : (size_t)(bar - name);
  if (base_length == 0 || base_length >= sizeof(base)) {
    log_fatal("egpick: bad weight name %s", name);
  }
  memcpy(base, name, base_length);
  base[base_length] = '\0';
  const bool has_tag = bar != NULL;
  const int tag = has_tag ? eg_pick_parse_tag(bar + 1) : -1;
  if (strcmp(base, "_out") == 0 && !has_tag) {
    weights->out += value;
  } else if (strcmp(base, "obo") == 0 && !has_tag) {
    weights->obo += value;
  } else if (strcmp(base, "obs") == 0 && tag == EG_PICK_TAG_NO) {
    weights->obs_no += value;
  } else if (strcmp(base, "const") == 0 && has_tag) {
    weights->constant[tag] += value;
  } else if (strcmp(base, "face") == 0 && has_tag) {
    weights->face[tag] += value;
  } else if (base[0] == 'n' && base[1] >= '0' && base[1] <= '9' &&
             base[2] == '\0' && has_tag) {
    const int leave_size = base[1] - '0';
    if (leave_size > EG_PICK_MAX_LEAVE_SIZE) {
      log_fatal("egpick: bad leave size in %s", name);
    }
    weights->leave_size[tag][leave_size] += value;
  } else if (base_length == 1 || base_length == 2) {
    const MachineLetter first = eg_pick_parse_letter(ld, base[0]);
    for (int tag_idx = 0; tag_idx < EG_PICK_NUM_TAGS; tag_idx++) {
      if (has_tag && tag_idx != tag) {
        continue;
      }
      if (base_length == 1) {
        weights->single[tag_idx][first] += value;
      } else {
        const MachineLetter second = eg_pick_parse_letter(ld, base[1]);
        if (second < first) {
          log_fatal("egpick: unsorted pair %s", name);
        }
        weights->pair[tag_idx][first][second] += value;
      }
    }
  } else {
    log_fatal("egpick: unknown weight %s", name);
  }
}

static void eg_pick_load_weights(EgPickWeights *weights,
                                 const LetterDistribution *ld) {
  memset(weights, 0, sizeof(*weights));
  const char *path = getenv("MAGPIE_EGPICK_WEIGHTS");
  if (path == NULL) {
    path = "scripts/egregret/weights/check_best_split_l3.tsv";
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    log_fatal("egpick: cannot open weights %s", path);
  }
  char line[EG_PICK_LINE_LENGTH];
  while (fgets(line, sizeof(line), file) != NULL) {
    char *tab = strchr(line, '\t');
    if (tab == NULL) {
      continue;
    }
    *tab = '\0';
    eg_pick_add_weight(weights, ld, line, strtod(tab + 1, NULL));
  }
  (void)fclose(file);
}

void eg_pick_set_suppressed(bool suppressed) {
  atomic_store(&eg_pick_suppressed, suppressed ? 1 : 0);
}

EgPick *eg_pick_create(const Game *game) {
  const int num_candidates = eg_pick_requested_candidates();
  if (num_candidates == 0 || atomic_load(&eg_pick_suppressed) != 0) {
    return NULL;
  }
  EgPick *pick = malloc_or_die(sizeof(EgPick));
  pick->num_candidates = num_candidates;
  pick->mode = eg_pick_requested_mode();
  eg_pick_load_weights(&pick->weights, game_get_ld(game));
  pick->moves = move_list_create(EG_PICK_MOVE_LIST_CAPACITY);
  pick->replies = move_list_create(EG_PICK_MOVE_LIST_CAPACITY);
  pick->after = game_duplicate(game);
  return pick;
}

void eg_pick_destroy(EgPick *pick) {
  if (pick == NULL) {
    return;
  }
  move_list_destroy(pick->moves);
  move_list_destroy(pick->replies);
  game_destroy(pick->after);
  free(pick);
}

// The leave terms of a move that does not go out.
static double eg_pick_leave_terms(const EgPickWeights *weights,
                                  const LetterDistribution *ld,
                                  const Rack *leave, int tag) {
  const int leave_size = rack_get_total_letters(leave);
  double value = 0.0;
  if (leave_size <= EG_PICK_MAX_LEAVE_SIZE) {
    value += weights->leave_size[tag][leave_size];
  }
  value += weights->face[tag] * equity_to_double(rack_get_score(ld, leave));
  const int ld_size = ld_get_size(ld);
  for (int first = 0; first < ld_size; first++) {
    const int count = rack_get_letter(leave, (MachineLetter)first);
    if (count == 0) {
      continue;
    }
    value += weights->single[tag][first];
    if (count >= 2) {
      value += weights->pair[tag][first][first];
    }
    for (int second = first + 1; second < ld_size; second++) {
      if (rack_get_letter(leave, (MachineLetter)second) > 0) {
        value += weights->pair[tag][first][second];
      }
    }
  }
  return value;
}

static void eg_pick_leave(const Rack *rack, const Move *move, Rack *leave) {
  rack_copy(leave, rack);
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_take_letter(leave, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
}

// The value of one candidate (see HANDOFF_M2.md).
static double eg_pick_value(EgPick *pick, const Game *game, const Move *move,
                            const Rack *rack) {
  // A pass carries movegen's sentinel equity; its static value at an empty
  // bag is -2 * rack face - 10 (HANDOFF_M2.md).
  const double equity =
      move_get_type(move) == GAME_EVENT_PASS
          ? equity_to_double(endgame_nonoutplay_adjustment(
                rack_get_score(game_get_ld(game), rack)))
          : equity_to_double(move_get_equity(move));
  const int rack_tiles = rack_get_total_letters(rack);
  const bool goes_out = move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                        move_get_tiles_played(move) == rack_tiles;
  if (pick->mode == EG_PICK_MODE_STATIC) {
    return equity;
  }
  const EgPickWeights *weights = &pick->weights;
  const bool use_weights = pick->mode != EG_PICK_MODE_RULE;
  if (goes_out) {
    return use_weights ? equity + weights->out : equity;
  }
  game_copy(pick->after, game);
  play_move(move, pick->after, NULL);
  bool opp_out = false;
  int best_out_score = 0;
  int best_score = 0;
  if (!game_over(pick->after)) {
    const int opp_tiles =
        rack_get_total_letters(player_get_rack(game_get_player(
            pick->after, game_get_player_on_turn_index(pick->after))));
    const MoveGenArgs reply_args = {
        .game = pick->after,
        .move_list = pick->replies,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&reply_args);
    atomic_fetch_add(&eg_pick_total_reply_movegens, 1);
    for (int reply_idx = 0; reply_idx < move_list_get_count(pick->replies);
         reply_idx++) {
      const Move *reply = move_list_get_move(pick->replies, reply_idx);
      if (move_get_type(reply) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      const int score = equity_to_int(move_get_score(reply));
      if (score > best_score) {
        best_score = score;
      }
      if (move_get_tiles_played(reply) == opp_tiles &&
          (!opp_out || score > best_out_score)) {
        best_out_score = score;
        opp_out = true;
      }
    }
  }
  double value = equity;
  if (opp_out) {
    value -= best_out_score;
  }
  if (!use_weights) {
    return value;
  }
  const int tag = opp_out ? EG_PICK_TAG_OO : EG_PICK_TAG_NO;
  value += weights->constant[tag];
  if (opp_out) {
    value += weights->obo * best_out_score;
  } else if (pick->mode == EG_PICK_MODE_FULL) {
    value += weights->obs_no * best_score;
  }
  Rack leave;
  eg_pick_leave(rack, move, &leave);
  value += eg_pick_leave_terms(weights, game_get_ld(game), &leave, tag);
  return value;
}

bool eg_pick_choose(EgPick *pick, Game *game, const KWG *override_kwg,
                    Move *out) {
  const int64_t start_ns = ctimer_monotonic_ns();
  if (pick->mode == EG_PICK_MODE_STATIC_BEST) {
    // The best play by static equity alone, from one pruned generation.
    const MoveGenArgs best_args = {
        .game = game,
        .move_list = pick->moves,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = override_kwg,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&best_args);
    if (move_list_get_count(pick->moves) == 0) {
      return false;
    }
    move_copy(out, move_list_get_move(pick->moves, 0));
    atomic_fetch_add(&eg_pick_total_picks, 1);
    atomic_fetch_add(&eg_pick_total_candidates, 1);
    atomic_fetch_add(&eg_pick_total_nanoseconds,
                     ctimer_monotonic_ns() - start_ns);
    return true;
  }
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = pick->moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = override_kwg,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&gen_args);
  const int num_moves = move_list_get_count(pick->moves);
  if (num_moves == 0) {
    return false;
  }
  move_list_sort_moves(pick->moves);
  const int num_candidates =
      num_moves < pick->num_candidates ? num_moves : pick->num_candidates;
  // Copies: the reply generations below reuse no list, but play_move on the
  // scratch game must not see the candidate list change underneath it.
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    move_copy(&pick->candidates[cand_idx],
              move_list_get_move(pick->moves, cand_idx));
  }
  const Rack *rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  int best_idx = 0;
  double best_value = 0.0;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    const double value =
        eg_pick_value(pick, game, &pick->candidates[cand_idx], rack);
    if (cand_idx == 0 || value > best_value) {
      best_value = value;
      best_idx = cand_idx;
    }
  }
  move_copy(out, &pick->candidates[best_idx]);
  atomic_fetch_add(&eg_pick_total_picks, 1);
  atomic_fetch_add(&eg_pick_total_candidates, num_candidates);
  atomic_fetch_add(&eg_pick_total_nanoseconds,
                   ctimer_monotonic_ns() - start_ns);
  return true;
}

bool eg_pick_is_static_best(const EgPick *pick) {
  return pick != NULL && pick->mode == EG_PICK_MODE_STATIC_BEST;
}

void eg_pick_stats_get(EgPickStats *stats) {
  stats->picks = atomic_load(&eg_pick_total_picks);
  stats->candidates = atomic_load(&eg_pick_total_candidates);
  stats->reply_movegens = atomic_load(&eg_pick_total_reply_movegens);
  stats->nanoseconds = atomic_load(&eg_pick_total_nanoseconds);
}

void eg_pick_stats_reset(void) {
  atomic_store(&eg_pick_total_picks, 0);
  atomic_store(&eg_pick_total_candidates, 0);
  atomic_store(&eg_pick_total_reply_movegens, 0);
  atomic_store(&eg_pick_total_nanoseconds, 0);
}
