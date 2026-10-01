#include "sim_nomination.h"

#include "../compat/ctime.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/blocking_setup_params.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "blocking_setup.h"
#include "move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  // Every move of a position, so exchanges far down the static order are
  // still seen by the exchange quota.
  SIM_NOMINATION_GEN_CAPACITY = 200000,
  SIM_NOMINATION_POOL_CAPACITY = 1024,
};

typedef struct NominationCandidate {
  // Index into the static list.
  int static_index;
  SimNominee nominee;
  Equity blocking_key;
  Equity setup_key;
  // Smallest rank in any list that nominated the move (for the cap).
  int best_rank;
} NominationCandidate;

struct SimNominator {
  Game *scratch;
  const LetterDistribution *scratch_ld;
  MoveList *static_list;
  MoveList *pat_list;
  MoveList *out_list;
  int out_capacity;
  NominationCandidate *candidates;
  int candidate_capacity;
  int num_candidates;
  // Indices into candidates, in out_list order.
  int *out_order;
  SimNominee *out_nominees;
  BlockingSetupSamples *samples;
  BlockingSetupChecker *checker;
  SimNominationTiming timing;
};

SimNominator *sim_nominator_create(void) {
  SimNominator *nominator = calloc_or_die(1, sizeof(SimNominator));
  nominator->static_list = move_list_create(SIM_NOMINATION_GEN_CAPACITY);
  nominator->pat_list = move_list_create(SIM_NOMINATION_GEN_CAPACITY);
  nominator->checker = blocking_setup_checker_create();
  return nominator;
}

void sim_nominator_destroy(SimNominator *nominator) {
  if (nominator == NULL) {
    return;
  }
  game_destroy(nominator->scratch);
  move_list_destroy(nominator->static_list);
  move_list_destroy(nominator->pat_list);
  move_list_destroy(nominator->out_list);
  free(nominator->candidates);
  free(nominator->out_order);
  free(nominator->out_nominees);
  blocking_setup_samples_destroy(nominator->samples);
  blocking_setup_checker_destroy(nominator->checker);
  free(nominator);
}

static void nominator_generate(const Game *game, MoveList *list,
                               bool disable_pat) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .disable_pat = disable_pat,
  };
  generate_moves(&args);
  move_list_sort_moves(list);
}

static int nominator_find(const MoveList *list, const Move *move) {
  for (int move_idx = 0; move_idx < move_list_get_count(list); move_idx++) {
    if (compare_moves_without_equity(move, move_list_get_move(list, move_idx),
                                     true) == -1) {
      return move_idx;
    }
  }
  return -1;
}

static void nominator_ensure_capacity(SimNominator *nominator, int count) {
  if (count <= nominator->candidate_capacity) {
    return;
  }
  free(nominator->candidates);
  free(nominator->out_order);
  free(nominator->out_nominees);
  move_list_destroy(nominator->out_list);
  nominator->candidate_capacity = count;
  nominator->candidates =
      malloc_or_die(sizeof(NominationCandidate) * (size_t)count);
  nominator->out_order = malloc_or_die(sizeof(int) * (size_t)count);
  nominator->out_nominees = malloc_or_die(sizeof(SimNominee) * (size_t)count);
  nominator->out_list = move_list_create(count);
}

// The candidate for static list index static_index, added when new.
static NominationCandidate *nominator_candidate(SimNominator *nominator,
                                                int static_index) {
  for (int idx = 0; idx < nominator->num_candidates; idx++) {
    if (nominator->candidates[idx].static_index == static_index) {
      return &nominator->candidates[idx];
    }
  }
  assert(nominator->num_candidates < nominator->candidate_capacity);
  NominationCandidate *candidate =
      &nominator->candidates[nominator->num_candidates++];
  const Move *move = move_list_get_move(nominator->static_list, static_index);
  candidate->static_index = static_index;
  candidate->nominee = (SimNominee){
      .sources = 0,
      .static_rank = static_index + 1,
      .pat_equity = move_get_equity(move),
      .static_equity = move_get_equity(move),
  };
  candidate->blocking_key = move_get_equity(move);
  candidate->setup_key = move_get_equity(move);
  candidate->best_rank = 0;
  return candidate;
}

static void nominator_admit(NominationCandidate *candidate, uint32_t source,
                            int rank) {
  candidate->nominee.sources |= source;
  if (candidate->best_rank == 0 || rank < candidate->best_rank) {
    candidate->best_rank = rank;
  }
}

static bool nominator_validate(const Game *game,
                               const SimNominationSettings *settings,
                               ErrorStack *error_stack) {
  const int counts[] = {settings->static_count,   settings->pat_count,
                        settings->blocking_count, settings->setup_count,
                        settings->exchange_quota, settings->universe_count,
                        settings->max_candidates};
  for (size_t count_idx = 0; count_idx < sizeof(counts) / sizeof(counts[0]);
       count_idx++) {
    if (counts[count_idx] < 0) {
      error_stack_push(error_stack, ERROR_STATUS_SIM_NOMINATION_INVALID,
                       string_duplicate("nomination counts must be >= 0"));
      return false;
    }
  }
  const bool checks = settings->blocking_count > 0 || settings->setup_count > 0;
  if (checks && settings->params == NULL) {
    error_stack_push(
        error_stack, ERROR_STATUS_SIM_NOMINATION_INVALID,
        string_duplicate("blocking or setup nominees need blocking/setup "
                         "parameters for the lexicon"));
    return false;
  }
  if (settings->check_base_pat && settings->pat_count == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_SIM_NOMINATION_INVALID,
        string_duplicate("ranking checks by PAT equity needs PAT nominees"));
    return false;
  }
  if (settings->pat_count > 0 &&
      player_get_pat(
          game_get_player(game, game_get_player_on_turn_index(game))) == NULL) {
    error_stack_push(
        error_stack, ERROR_STATUS_SIM_NOMINATION_INVALID,
        string_duplicate("PAT nominees need PAT weights for the player on "
                         "turn"));
    return false;
  }
  return true;
}

// The scratch copy PAT nominees are generated on, with the on-turn player's
// PAT applied in full whatever its candidate and rollout settings say.
static const Game *nominator_pat_game(SimNominator *nominator,
                                      const Game *game) {
  if (nominator->scratch == NULL ||
      nominator->scratch_ld != game_get_ld(game)) {
    game_destroy(nominator->scratch);
    nominator->scratch = game_duplicate(game);
    nominator->scratch_ld = game_get_ld(game);
  } else {
    game_copy(nominator->scratch, game);
  }
  player_set_pat_usage(
      game_get_player(nominator->scratch,
                      game_get_player_on_turn_index(nominator->scratch)),
      false, 0);
  return nominator->scratch;
}

static void nominator_add_pat_nominees(SimNominator *nominator,
                                       const SimNominationSettings *settings) {
  int pat_rank = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(nominator->pat_list);
       move_idx++) {
    const Move *move = move_list_get_move(nominator->pat_list, move_idx);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    pat_rank++;
    const int static_index = nominator_find(nominator->static_list, move);
    assert(static_index >= 0);
    if (pat_rank <= settings->pat_count) {
      NominationCandidate *candidate =
          nominator_candidate(nominator, static_index);
      nominator_admit(candidate, SIM_NOMINATION_SOURCE_PAT, pat_rank);
    }
  }
}

// Fills every universe candidate's PAT rank and equity from the PAT list.
static void nominator_fill_pat_values(SimNominator *nominator) {
  int pat_rank = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(nominator->pat_list);
       move_idx++) {
    const Move *move = move_list_get_move(nominator->pat_list, move_idx);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    pat_rank++;
    for (int idx = 0; idx < nominator->num_candidates; idx++) {
      NominationCandidate *candidate = &nominator->candidates[idx];
      if (candidate->nominee.pat_rank == 0 &&
          compare_moves_without_equity(
              move,
              move_list_get_move(nominator->static_list,
                                 candidate->static_index),
              true) == -1) {
        candidate->nominee.pat_rank = pat_rank;
        candidate->nominee.pat_equity = move_get_equity(move);
      }
    }
  }
}

static int nominator_effective_bag(const Game *game) {
  MachineLetter pool[SIM_NOMINATION_POOL_CAPACITY];
  assert(ld_get_total_tiles(game_get_ld(game)) <= SIM_NOMINATION_POOL_CAPACITY);
  const int pool_size = blocking_setup_unseen_pool(game, pool);
  return pool_size - (pool_size < RACK_SIZE ? pool_size : RACK_SIZE);
}

static int nominator_lead(const Game *game) {
  const int on_turn = game_get_player_on_turn_index(game);
  return equity_to_int(player_get_score(game_get_player(game, on_turn)) -
                       player_get_score(game_get_player(game, 1 - on_turn)));
}

static void nominator_run_checks(SimNominator *nominator, const Game *game,
                                 const SimNominationSettings *settings,
                                 int effective_bag) {
  const BlockingSetupParams *params = settings->params;
  const int num_racks = blocking_setup_params_get_teacher_racks(params);
  if (nominator->samples == NULL ||
      nominator->samples->rack_capacity < num_racks) {
    blocking_setup_samples_destroy(nominator->samples);
    nominator->samples =
        blocking_setup_samples_create(num_racks, SIM_NOMINATION_POOL_CAPACITY);
  }
  blocking_setup_samples_deal(
      nominator->samples, game, num_racks,
      blocking_setup_params_get_teacher_partition(params),
      blocking_setup_params_get_teacher_condition_draws(params),
      settings->seed);
  blocking_setup_checker_set_value(
      nominator->checker, blocking_setup_params_get_teacher_value(params));
  blocking_setup_checker_load(
      nominator->checker, game, nominator->samples,
      blocking_setup_params_get_teacher_followup_draws(params));
  double blocking_weight = 0.0;
  double setup_weight = 0.0;
  blocking_setup_params_get_weights(params, effective_bag, nominator_lead(game),
                                    &blocking_weight, &setup_weight);
  for (int idx = 0; idx < nominator->num_candidates; idx++) {
    NominationCandidate *candidate = &nominator->candidates[idx];
    const Move *move =
        move_list_get_move(nominator->static_list, candidate->static_index);
    BlockingSetupResult result;
    blocking_setup_checker_measure(nominator->checker, move, &result);
    SimNominee *nominee = &candidate->nominee;
    nominee->checked = true;
    nominee->check = result;
    nominee->blocking_adjustment =
        double_to_equity(blocking_weight * result.blocking_delta);
    nominee->setup_adjustment =
        double_to_equity(setup_weight * result.setup_delta);
    const Equity base =
        settings->check_base_pat ? nominee->pat_equity : nominee->static_equity;
    candidate->blocking_key = base + nominee->blocking_adjustment;
    candidate->setup_key = base + nominee->setup_adjustment;
  }
  nominator->timing.checked_count = nominator->num_candidates;
}

static Equity nominator_key(const NominationCandidate *candidate,
                            bool blocking) {
  return blocking ? candidate->blocking_key : candidate->setup_key;
}

// Admits the top count universe candidates by key (blocking or setup).
static void nominator_admit_by_key(SimNominator *nominator, int count,
                                   bool blocking) {
  const uint32_t source =
      blocking ? SIM_NOMINATION_SOURCE_BLOCKING : SIM_NOMINATION_SOURCE_SETUP;
  for (int rank = 1; rank <= count && rank <= nominator->num_candidates;
       rank++) {
    NominationCandidate *best = NULL;
    for (int idx = 0; idx < nominator->num_candidates; idx++) {
      NominationCandidate *candidate = &nominator->candidates[idx];
      const int taken = blocking ? candidate->nominee.blocking_rank
                                 : candidate->nominee.setup_rank;
      if (taken != 0) {
        continue;
      }
      const Equity key = nominator_key(candidate, blocking);
      if (best == NULL || key > nominator_key(best, blocking) ||
          (key == nominator_key(best, blocking) &&
           candidate->static_index < best->static_index)) {
        best = candidate;
      }
    }
    if (blocking) {
      best->nominee.blocking_rank = rank;
    } else {
      best->nominee.setup_rank = rank;
    }
    nominator_admit(best, source, rank);
  }
}

static int compare_ints(const void *a, const void *b) {
  const int left = *(const int *)a;
  const int right = *(const int *)b;
  return (left > right) - (left < right);
}

// Fills out_list with the admitted candidates, applying the cap.
static void nominator_emit(SimNominator *nominator,
                           const SimNominationSettings *settings) {
  int num_admitted = 0;
  for (int idx = 0; idx < nominator->num_candidates; idx++) {
    if (nominator->candidates[idx].nominee.sources != 0) {
      nominator->out_order[num_admitted++] = idx;
    }
  }
  if (settings->max_candidates > 0 && num_admitted > settings->max_candidates) {
    // Keep the best-ranked: selection by (best rank, static index).
    for (int keep = 0; keep < settings->max_candidates; keep++) {
      int chosen = keep;
      for (int other = keep + 1; other < num_admitted; other++) {
        const NominationCandidate *a =
            &nominator->candidates[nominator->out_order[other]];
        const NominationCandidate *b =
            &nominator->candidates[nominator->out_order[chosen]];
        if (a->best_rank < b->best_rank ||
            (a->best_rank == b->best_rank &&
             a->static_index < b->static_index)) {
          chosen = other;
        }
      }
      const int swap = nominator->out_order[keep];
      nominator->out_order[keep] = nominator->out_order[chosen];
      nominator->out_order[chosen] = swap;
    }
    num_admitted = settings->max_candidates;
  }
  // Output in static order: sort the kept candidates' static indices.
  int *static_indices = malloc_or_die(sizeof(int) * (size_t)num_admitted);
  for (int idx = 0; idx < num_admitted; idx++) {
    static_indices[idx] =
        nominator->candidates[nominator->out_order[idx]].static_index;
  }
  qsort(static_indices, (size_t)num_admitted, sizeof(int), compare_ints);
  move_list_reset(nominator->out_list);
  for (int idx = 0; idx < num_admitted; idx++) {
    const NominationCandidate *candidate =
        nominator_candidate(nominator, static_indices[idx]);
    move_list_add_move_to_sorted_list(
        nominator->out_list,
        move_list_get_move(nominator->static_list, static_indices[idx]));
    nominator->out_nominees[idx] = candidate->nominee;
  }
  free(static_indices);
}

const MoveList *sim_nominator_nominate(SimNominator *nominator,
                                       const Game *game,
                                       const SimNominationSettings *settings,
                                       ErrorStack *error_stack) {
  const int64_t start_ns = ctimer_monotonic_ns();
  nominator->timing = (SimNominationTiming){0};
  if (!nominator_validate(game, settings, error_stack)) {
    return NULL;
  }
  nominator_generate(game, nominator->static_list, true);
  const int64_t static_done_ns = ctimer_monotonic_ns();
  nominator->timing.static_movegen_ns = static_done_ns - start_ns;
  const bool want_pat = settings->pat_count > 0;
  if (want_pat) {
    nominator_generate(nominator_pat_game(nominator, game), nominator->pat_list,
                       false);
    assert(move_list_get_count(nominator->pat_list) ==
           move_list_get_count(nominator->static_list));
  }
  const int64_t pat_done_ns = ctimer_monotonic_ns();
  nominator->timing.pat_movegen_ns = pat_done_ns - static_done_ns;

  const int effective_bag = nominator_effective_bag(game);
  int min_check_bag = settings->min_check_bag;
  if (min_check_bag < 1) {
    min_check_bag = 1;
  }
  const bool run_checks =
      (settings->blocking_count > 0 || settings->setup_count > 0) &&
      effective_bag >= min_check_bag && !game_over(game);
  const int universe_count = settings->universe_count > settings->static_count
                                 ? settings->universe_count
                                 : settings->static_count;
  nominator_ensure_capacity(nominator, universe_count +
                                           settings->exchange_quota +
                                           settings->pat_count + 1);
  nominator->num_candidates = 0;

  // Static nominees and, when checks run, the rest of the static universe.
  const int static_limit = run_checks ? universe_count : settings->static_count;
  int static_rank = 0;
  for (int move_idx = 0;
       move_idx < move_list_get_count(nominator->static_list) &&
       static_rank < static_limit;
       move_idx++) {
    if (move_get_type(move_list_get_move(nominator->static_list, move_idx)) ==
        GAME_EVENT_PASS) {
      continue;
    }
    static_rank++;
    NominationCandidate *candidate = nominator_candidate(nominator, move_idx);
    if (static_rank <= settings->static_count) {
      nominator_admit(candidate, SIM_NOMINATION_SOURCE_STATIC, static_rank);
    }
  }
  // Exchanges within the margin of the top static move.
  if (move_list_get_count(nominator->static_list) > 0) {
    const Equity top =
        move_get_equity(move_list_get_move(nominator->static_list, 0));
    int taken = 0;
    for (int move_idx = 0;
         move_idx < move_list_get_count(nominator->static_list) &&
         taken < settings->exchange_quota;
         move_idx++) {
      const Move *move = move_list_get_move(nominator->static_list, move_idx);
      if (move_get_equity(move) < top - settings->exchange_margin) {
        break;
      }
      if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
        taken++;
        nominator_admit(nominator_candidate(nominator, move_idx),
                        SIM_NOMINATION_SOURCE_EXCHANGE, taken);
      }
    }
  }
  if (want_pat) {
    nominator_add_pat_nominees(nominator, settings);
    nominator_fill_pat_values(nominator);
  }
  nominator->timing.universe_count = nominator->num_candidates;
  if (run_checks) {
    nominator_run_checks(nominator, game, settings, effective_bag);
    nominator_admit_by_key(nominator, settings->blocking_count, true);
    nominator_admit_by_key(nominator, settings->setup_count, false);
  }
  nominator->timing.check_ns = ctimer_monotonic_ns() - pat_done_ns;
  // Only a pass is legal: nominate it so the caller has a move.
  if (nominator->num_candidates == 0 &&
      move_list_get_count(nominator->static_list) > 0) {
    nominator_admit(nominator_candidate(nominator, 0),
                    SIM_NOMINATION_SOURCE_STATIC, 1);
  }
  nominator_emit(nominator, settings);
  nominator->timing.total_ns = ctimer_monotonic_ns() - start_ns;
  return nominator->out_list;
}

const SimNominee *sim_nominator_get_nominee(const SimNominator *nominator,
                                            int idx) {
  assert(idx >= 0 && idx < move_list_get_count(nominator->out_list));
  return &nominator->out_nominees[idx];
}

const SimNominationTiming *
sim_nominator_get_timing(const SimNominator *nominator) {
  return &nominator->timing;
}

void sim_nomination_candidate_source_init(
    SimNominationCandidateSource *source,
    const SimNominationSettings *settings) {
  source->nominator = sim_nominator_create();
  source->settings = *settings;
  source->calls = 0;
}

void sim_nomination_candidate_source_cleanup(
    SimNominationCandidateSource *source) {
  sim_nominator_destroy(source->nominator);
  source->nominator = NULL;
}

const MoveList *sim_nomination_candidates(void *context, const Game *game) {
  SimNominationCandidateSource *source = context;
  SimNominationSettings settings = source->settings;
  // A fresh, reproducible sample per decision.
  settings.seed = source->settings.seed + (source->calls++ * UINT64_C(1000003));
  ErrorStack *error_stack = error_stack_create();
  const MoveList *list =
      sim_nominator_nominate(source->nominator, game, &settings, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    list = NULL;
  }
  error_stack_destroy(error_stack);
  return list;
}
