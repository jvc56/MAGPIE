#include "movegen_wordsmog.h"

#include "../def/bit_rack_defs.h"
#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../def/wordsmog_defs.h"
#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/pat.h"
#include "../ent/pat_eval.h"
#include "../ent/static_eval.h"
#include "../ent/wmp.h"
#include "../util/io_util.h"
#include "move_gen_base.h"
#include "wmp_move_gen.h"
#include "wordsmog_arrangement.h"
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Wordsmog with the WMP. A subrack whose letters and the playthrough letters
// are an anagram of a word forms a valid main word in every arrangement, so
// each of the anchor's start columns gets one play per such subrack: its best
// arrangement on the placement's empty squares (see wordsmog_arrangement.h).

// The tiles of a subrack for wordsmog_best_arrangement: its natural letters,
// its blanks, and the designations that keep main_word, the subrack plus the
// playthrough letters, an anagram of a word. entry is main_word's WMP entry.
void wordsmog_tiles_init(const MoveGen *gen, const BitRack *subrack,
                         const BitRack *main_word, const WMPEntry *entry,
                         WordsmogTiles *tiles) {
  const int ld_size = ld_get_size(&gen->ld);
  tiles->num_letters = 0;
  tiles->vowels = 0;
  for (int ml = 1; ml < ld_size; ml++) {
    if (ld_get_is_vowel(&gen->ld, ml)) {
      tiles->vowels |= 1U << ml;
    }
    const int count = bit_rack_get_letter(subrack, ml);
    if (count == 0) {
      continue;
    }
    tiles->letters[tiles->num_letters] = (MachineLetter)ml;
    tiles->counts[tiles->num_letters] = count;
    tiles->scores[tiles->num_letters] = gen->tile_scores[ml];
    tiles->num_letters++;
  }
  tiles->num_blanks = bit_rack_get_letter(subrack, BLANK_MACHINE_LETTER);
  tiles->blank_score =
      gen->tile_scores[get_blanked_machine_letter(BLANK_MACHINE_LETTER)];
  tiles->blank_letters = 0;
  memset(tiles->pair_letters, 0, sizeof(tiles->pair_letters));
  if (tiles->num_blanks == 1) {
    tiles->blank_letters = entry->blank_letters;
  } else if (tiles->num_blanks == 2) {
    // A double-blank entry holds the lower letter of each pair. The higher
    // ones are the designations of the other blank once the lower one is
    // fixed, at or above it.
    const int word_length = gen->wmp_move_gen.word_length;
    for (uint32_t lows = entry->first_blank_letters & ~1U; lows != 0;
         lows &= lows - 1) {
      const MachineLetter low = (MachineLetter)lowest_set_bit_index(lows);
      BitRack one_blank = *main_word;
      bit_rack_take_letter(&one_blank, BLANK_MACHINE_LETTER);
      bit_rack_add_letter(&one_blank, low);
      const WMPEntry *one_blank_entry = wfl_get_blank_entry(
          &gen->wmp_move_gen.wmp->wfls[word_length], &one_blank);
      if (one_blank_entry != NULL) {
        tiles->pair_letters[low] =
            one_blank_entry->blank_letters & ~((1U << low) - 1U);
      }
    }
  }
  wordsmog_tiles_set_states(tiles);
}

// The empty squares of the current anchor's placement at start_col, in board
// order, for wordsmog_best_arrangement, and their positions in the word in
// square_letter_idxs. Marks the board's tiles in gen->playthrough_marked, and
// writes the part of every play's score there that does not depend on its
// tiles to base_score. Returns how many squares there are.
int wordsmog_placement_squares(MoveGen *gen, int start_col,
                               WordsmogSquare squares[RACK_SIZE],
                               int square_letter_idxs[RACK_SIZE],
                               Equity *base_score) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  // The opening move's placement penalty is an equity term that depends on
  // where the tiles go, less the parts the PAT term replaces, as
  // placement_adjustment prices it. The PAT term itself is left to
  // wordsmog_pat_refine.
  const bool opening = gen->move_sort_type == MOVE_SORT_EQUITY &&
                       gen->board_number_of_tiles_played == 0;
  const uint32_t pat_classes =
      pat_eval_context_get_active_classes(&gen->pat_eval_ctx);
  const bool word_penalties = (pat_classes & PAT_CLASS_MASK_WORD_MULT) == 0;
  const bool letter_penalties = (pat_classes & PAT_CLASS_MASK_LETTER_MULT) == 0;
  const int penalty_offset = board_is_dir_vertical(gen->dir) ? BOARD_DIM : 0;
  // A square with no cross word allows every bit; keep the alphabet's letters.
  const uint64_t letters =
      (((uint64_t)1 << ld_get_size(&gen->ld)) - 1) & ~(uint64_t)1;
  int num_squares = 0;
  int word_multiplier = 1;
  Equity playthrough_score = 0;
  Equity hooked_cross_score = 0;
  for (int letter_idx = 0; letter_idx < wgen->word_length; letter_idx++) {
    const int board_col = start_col + letter_idx;
    if ((wgen->playthrough_positions & (1U << board_col)) != 0) {
      gen->playthrough_marked[letter_idx] = PLAYED_THROUGH_MARKER;
      playthrough_score +=
          gen->tile_scores[gen_cache_get_letter(gen, board_col)];
      continue;
    }
    assert(num_squares < RACK_SIZE);
    const int this_word_multiplier = bonus_square_get_word_multiplier(
        gen_cache_get_bonus_square(gen, board_col));
    word_multiplier *= this_word_multiplier;
    hooked_cross_score +=
        gen_cache_get_cross_score(gen, board_col) * this_word_multiplier;
    square_letter_idxs[num_squares] = letter_idx;
    squares[num_squares].cross_set =
        gen_cache_get_cross_set(gen, board_col) & letters;
    Equity vowel_adjustment = 0;
    if (opening && word_penalties) {
      vowel_adjustment +=
          gen->opening_move_word_penalties[penalty_offset + board_col];
    }
    if (opening && letter_penalties) {
      vowel_adjustment +=
          gen->opening_move_letter_penalties[penalty_offset + board_col];
    }
    squares[num_squares].vowel_adjustment = vowel_adjustment;
    num_squares++;
  }
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    const int board_col = start_col + square_letter_idxs[square_idx];
    const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, board_col);
    const int cross_word_multiplier =
        gen_cache_get_is_cross_word(gen, board_col)
            ? bonus_square_get_word_multiplier(bonus_square)
            : 0;
    squares[square_idx].coefficient =
        bonus_square_get_letter_multiplier(bonus_square) *
        (word_multiplier + cross_word_multiplier);
  }
  *base_score = playthrough_score * word_multiplier + hooked_cross_score +
                (gen->max_tiles_to_play == RACK_SIZE ? gen->bingo_bonus : 0);
  return num_squares;
}

// Whether no play at a placement whose scores are at most score_bound can be
// recorded: as wordmap_gen_leave_prunes_subrack, with the placement's own
// score bound in place of the anchor's, under equity sort with tiles in the
// bag; by score alone under score sort.
static inline bool wordsmog_placement_pruned(const MoveGen *gen,
                                             Equity score_bound,
                                             Equity anchor_pat_bound) {
  if (gen->wmp_prune_subracks_by_leave) {
    return better_play_has_been_found(gen, score_bound + anchor_pat_bound +
                                               gen->wmp_move_gen.leave_value);
  }
  return gen->move_sort_type == MOVE_SORT_SCORE &&
         better_play_has_been_found(gen, score_bound);
}

// With the PAT term on, the best play of a placement and tile multiset, a
// group, need not be its best arrangement by every other term, since the term
// depends on which tile goes on which square and which letter each blank
// stands for. Under MOVE_RECORD_BEST, move generation records each group's
// best arrangement, priced with the term, and keeps the group;
// wordsmog_pat_refine then prices more of the groups' plays, a round at a
// time: every group that could still hold the best play gets its next play
// priced before any group gets another. It stops once gen->wordsmog_pat_evals
// plays have been priced in all, or once no play left could beat the best.
// Without a limit it prices every play that could be best, highest bound
// first, which is exact.
//
// Under MOVE_RECORD_ALL the list holds one play per group, as without the
// term: move generation prices every group's best arrangement, the rounds
// look for better arrangements of the groups that could still make the list,
// and each group's best play is recorded at the end. There every group's
// first play is priced anyway, so gen->wordsmog_pat_evals counts the plays
// priced in rounds.
//
// A group's plays come best first by every other term, as in the play graph:
// a node is the arrangements that pin the group's first squares, and the best
// of them, wordsmog_best_arrangement with the pins, bounds the rest by every
// other term. The term is at most the group's defense_bound plus the largest
// utility correction of the play's score, so a node that cannot beat the best
// play is passed over whole. Every designation of an arrangement's blanks is a
// play of its own.
typedef struct WordsmogPATGroup {
  int row;
  int dir;
  int start_col;
  int word_length;
  int tiles_played;
  int num_squares;
  // The play's tiles, with PLAYED_THROUGH_MARKER on the board's tiles; an
  // arrangement's tiles go at square_letter_idxs.
  MachineLetter strip[BOARD_DIM];
  int square_letter_idxs[RACK_SIZE];
  WordsmogSquare squares[RACK_SIZE];
  // The group's tiles, in the refine's tiles, shared by the start columns of
  // a subrack.
  int tiles_idx;
  Equity base_score;
  Equity leave_value;
  // The PAT term without the utility correction is at most this for every
  // play of the group (see pat_eval_move_defense_bound).
  Equity defense_bound;
  // The highest score of any of its arrangements (see wordsmog_score_bound).
  Equity max_score;
  // The tiles of the play move generation recorded, already priced.
  MachineLetter recorded[RACK_SIZE];
  // How many of its plays have been priced.
  int evals;
  // Under MOVE_RECORD_ALL, its best play so far: its tiles, score and equity.
  MachineLetter best_tiles[RACK_SIZE];
  Equity best_score;
  Equity best_equity;
} WordsmogPATGroup;

// The arrangements of a group that put pins on its first num_pinned squares,
// or with designated set, the one play tiles spells.
typedef struct WordsmogPATNode {
  // An upper bound on the equity of its plays, and on their scores.
  Equity bound;
  Equity max_score;
  // The score of its best arrangement, and that arrangement's tiles.
  Equity score;
  MachineLetter tiles[RACK_SIZE];
  MachineLetter pins[RACK_SIZE];
  int num_pinned;
  bool designated;
  int group_idx;
  // The group's evals when the node was pushed, and when it was pushed.
  int round;
  uint32_t order;
} WordsmogPATNode;

struct WordsmogPATRefine {
  WordsmogPATGroup *groups;
  int num_groups;
  int groups_capacity;
  WordsmogTiles *tiles;
  int num_tiles;
  int tiles_capacity;
  WordsmogPATNode *heap;
  int heap_count;
  int heap_capacity;
  uint32_t next_order;
  // Whether move generation records every play (MOVE_RECORD_ALL), and then
  // the equity a group's best play needs to make the move list:
  // EQUITY_MIN_VALUE until the list could be filled.
  bool record_all;
  Equity threshold;
};

void wordsmog_pat_refine_reset(WordsmogPATRefine *refine) {
  refine->num_groups = 0;
  refine->num_tiles = 0;
  refine->heap_count = 0;
  refine->next_order = 0;
  refine->threshold = EQUITY_MIN_VALUE;
}

void wordsmog_pat_refine_destroy(WordsmogPATRefine *refine) {
  if (refine == NULL) {
    return;
  }
  free(refine->groups);
  free(refine->tiles);
  free(refine->heap);
  free(refine);
}

// The equity without the PAT term of a play of tiles_played tiles with score
// and adjustment: exactly static evaluation's, since the adjustment is the
// opening move's placement_adjustment.
static inline Equity wordsmog_equity_without_pat(const MoveGen *gen,
                                                 int tiles_played, Equity score,
                                                 Equity adjustment,
                                                 Equity leave_value) {
  return static_eval_get_nonopening_move_equity(
             &gen->ld, &gen->leave, &gen->opponent_rack, tiles_played,
             gen->number_of_tiles_in_bag, score, leave_value) +
         adjustment;
}

// Whether plays of group with equity at most bound and score at most
// max_score cannot beat the best play, or under MOVE_RECORD_ALL, the group's
// best play or the list's threshold: one that only ties its equity with a
// lower score loses to it (see compare_moves).
static inline bool wordsmog_pat_prunes(const MoveGen *gen,
                                       const WordsmogPATGroup *group,
                                       Equity bound, Equity max_score) {
  if (gen->wordsmog_pat->record_all) {
    return bound < gen->wordsmog_pat->threshold || bound < group->best_equity ||
           (bound == group->best_equity && max_score < group->best_score);
  }
  if (better_play_has_been_found(gen, bound)) {
    return true;
  }
  const Move *best = gen_get_readonly_best_move(gen);
  return move_get_equity(best) != EQUITY_INITIAL_VALUE &&
         bound == move_get_equity(best) && max_score < move_get_score(best);
}

// Fills node from the best arrangement of its group's arrangements with pins,
// and returns whether it could still beat the best play.
static bool wordsmog_pat_node_init(const MoveGen *gen,
                                   const WordsmogPATGroup *group, int group_idx,
                                   const MachineLetter pins[RACK_SIZE],
                                   int num_pinned,
                                   const WordsmogArrangement *arrangement,
                                   WordsmogPATNode *node) {
  const bool opening = gen->board_number_of_tiles_played == 0;
  node->score = group->base_score + arrangement->score;
  // On the opening move an arrangement's adjustment lets a lower key have a
  // higher score.
  node->max_score = opening ? group->max_score : node->score;
  node->bound =
      wordsmog_equity_without_pat(gen, group->tiles_played, node->score,
                                  arrangement->adjustment, group->leave_value) +
      group->defense_bound +
      pat_eval_utility_bound_for_play(&gen->pat_eval_ctx, group->tiles_played,
                                      node->max_score);
  memcpy(node->tiles, arrangement->tiles, sizeof(node->tiles));
  memcpy(node->pins, pins, sizeof(node->pins));
  node->num_pinned = num_pinned;
  node->designated = false;
  node->group_idx = group_idx;
  // Without a limit every play that could be best is priced anyway, and
  // pricing the most promising first prunes the most.
  node->round = gen->wordsmog_pat_evals == INT_MAX ? 0 : group->evals;
  return !wordsmog_pat_prunes(gen, group, node->bound, node->max_score);
}

// Fewer of its group's plays priced, then a higher bound; among those, the
// deeper node and then the one pushed first.
static bool wordsmog_pat_node_ranks_above(const WordsmogPATNode *a,
                                          const WordsmogPATNode *b) {
  if (a->round != b->round) {
    return a->round < b->round;
  }
  if (a->bound != b->bound) {
    return a->bound > b->bound;
  }
  const int a_depth = a->num_pinned + (a->designated ? 1 : 0);
  const int b_depth = b->num_pinned + (b->designated ? 1 : 0);
  if (a_depth != b_depth) {
    return a_depth > b_depth;
  }
  return a->order < b->order;
}

static void wordsmog_pat_push(WordsmogPATRefine *refine, WordsmogPATNode node) {
  if (refine->heap_count == refine->heap_capacity) {
    refine->heap_capacity =
        refine->heap_capacity == 0 ? 256 : refine->heap_capacity * 2;
    refine->heap = realloc_or_die(refine->heap, (size_t)refine->heap_capacity *
                                                    sizeof(WordsmogPATNode));
  }
  node.order = refine->next_order++;
  int child = refine->heap_count++;
  while (child > 0) {
    const int parent = (child - 1) / 2;
    if (!wordsmog_pat_node_ranks_above(&node, &refine->heap[parent])) {
      break;
    }
    refine->heap[child] = refine->heap[parent];
    child = parent;
  }
  refine->heap[child] = node;
}

static WordsmogPATNode wordsmog_pat_pop(WordsmogPATRefine *refine) {
  const WordsmogPATNode top = refine->heap[0];
  const WordsmogPATNode last = refine->heap[--refine->heap_count];
  int parent = 0;
  for (;;) {
    int child = 2 * parent + 1;
    if (child >= refine->heap_count) {
      break;
    }
    if (child + 1 < refine->heap_count &&
        wordsmog_pat_node_ranks_above(&refine->heap[child + 1],
                                      &refine->heap[child])) {
      child++;
    }
    if (!wordsmog_pat_node_ranks_above(&refine->heap[child], &last)) {
      break;
    }
    refine->heap[parent] = refine->heap[child];
    parent = child;
  }
  if (refine->heap_count > 0) {
    refine->heap[parent] = last;
  }
  return top;
}

// Keeps the group of the placement at start_col whose best arrangement move
// generation just recorded, with its root node, when any of its other plays
// could beat the best play; under MOVE_RECORD_ALL, which records it, the group
// whose best arrangement this is, priced here. tiles_idx is the subrack's
// tiles in the refine, or -1 until a group of the subrack is kept.
static void wordsmog_pat_add_group(MoveGen *gen, int start_col,
                                   const WordsmogSquare *squares,
                                   const int *square_letter_idxs,
                                   int num_squares, const WordsmogTiles *tiles,
                                   int *tiles_idx, Equity base_score,
                                   Equity score_bound,
                                   const WordsmogArrangement *arrangement) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  if (gen->wordsmog_pat == NULL) {
    gen->wordsmog_pat = calloc_or_die(1, sizeof(WordsmogPATRefine));
    wordsmog_pat_refine_reset(gen->wordsmog_pat);
  }
  WordsmogPATRefine *refine = gen->wordsmog_pat;
  refine->record_all = gen->move_record_type == MOVE_RECORD_ALL;
  WordsmogPATGroup group = {
      .row = gen->current_row_index,
      .dir = gen->dir,
      .start_col = start_col,
      .word_length = wgen->word_length,
      .tiles_played = gen->max_tiles_to_play,
      .num_squares = num_squares,
      .base_score = base_score,
      .leave_value = wgen->leave_value,
      .max_score = base_score + score_bound,
      .evals = 1,
  };
  memcpy(group.strip, gen->playthrough_marked, (size_t)wgen->word_length);
  memcpy(group.square_letter_idxs, square_letter_idxs,
         (size_t)num_squares * sizeof(int));
  memcpy(group.squares, squares, (size_t)num_squares * sizeof(WordsmogSquare));
  memcpy(group.recorded, arrangement->tiles, sizeof(group.recorded));
  // Any arrangement gives the group's squares.
  Move move;
  set_play_for_record_wmp(gen, &move, start_col,
                          base_score + arrangement->score);
  group.defense_bound =
      pat_eval_move_defense_bound(&gen->pat_eval_ctx, &move, &gen->leave);
  if (refine->record_all) {
    memcpy(group.best_tiles, arrangement->tiles, sizeof(group.best_tiles));
    group.best_score = move_get_score(&move);
    group.best_equity =
        static_eval_get_move_equity_with_leave_value(
            &gen->ld, &move, &gen->leave, &gen->opponent_rack,
            gen->opening_move_word_penalties,
            gen->opening_move_letter_penalties,
            pat_eval_context_get_active_classes(&gen->pat_eval_ctx), NULL,
            gen->board_number_of_tiles_played, gen->number_of_tiles_in_bag,
            wgen->leave_value) +
        pat_eval_move_penalty(&gen->pat_eval_ctx, &move, &gen->leave);
  }
  MachineLetter pins[RACK_SIZE];
  memset(pins, WORDSMOG_PIN_FREE, sizeof(pins));
  WordsmogPATNode root;
  const bool searchable = wordsmog_pat_node_init(
      gen, &group, refine->num_groups, pins, 0, arrangement, &root);
  if (!searchable && !refine->record_all) {
    return;
  }
  if (*tiles_idx < 0) {
    if (refine->num_tiles == refine->tiles_capacity) {
      refine->tiles_capacity =
          refine->tiles_capacity == 0 ? 16 : refine->tiles_capacity * 2;
      refine->tiles =
          realloc_or_die(refine->tiles, (size_t)refine->tiles_capacity *
                                            sizeof(WordsmogTiles));
    }
    refine->tiles[refine->num_tiles] = *tiles;
    *tiles_idx = refine->num_tiles++;
  }
  group.tiles_idx = *tiles_idx;
  if (refine->num_groups == refine->groups_capacity) {
    refine->groups_capacity =
        refine->groups_capacity == 0 ? 64 : refine->groups_capacity * 2;
    refine->groups =
        realloc_or_die(refine->groups, (size_t)refine->groups_capacity *
                                           sizeof(WordsmogPATGroup));
  }
  refine->groups[refine->num_groups++] = group;
  if (searchable) {
    wordsmog_pat_push(refine, root);
  }
}

// Pushes the children of node: with squares left to pin, the next one pinned
// to each tile left that has a legal arrangement there; with every square
// pinned, each designation of its blanks that keeps every word valid.
static void wordsmog_pat_expand(const MoveGen *gen, WordsmogPATRefine *refine,
                                const WordsmogPATNode *node) {
  const WordsmogPATGroup *group = &refine->groups[node->group_idx];
  WordsmogTiles *tiles = &refine->tiles[group->tiles_idx];
  WordsmogPATNode child;
  if (node->num_pinned == group->num_squares) {
    int blank_squares[2] = {0};
    int num_blanks = 0;
    for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
      if (get_is_blanked(node->tiles[square_idx])) {
        blank_squares[num_blanks++] = square_idx;
      }
    }
    child = *node;
    child.designated = true;
    const uint64_t designations =
        (((uint64_t)1 << BIT_RACK_MAX_ALPHABET_SIZE) - 1) & ~(uint64_t)1;
    const uint64_t first_set =
        group->squares[blank_squares[0]].cross_set & designations;
    if (num_blanks == 1) {
      for (uint64_t xs = first_set & tiles->blank_letters; xs != 0;
           xs &= xs - 1) {
        child.tiles[blank_squares[0]] =
            get_blanked_machine_letter((MachineLetter)lowest_set_bit_index(xs));
        wordsmog_pat_push(refine, child);
      }
      return;
    }
    const uint64_t second_set =
        group->squares[blank_squares[1]].cross_set & designations;
    for (uint64_t xs = first_set; xs != 0; xs &= xs - 1) {
      const MachineLetter x = (MachineLetter)lowest_set_bit_index(xs);
      for (uint64_t ys = second_set; ys != 0; ys &= ys - 1) {
        const MachineLetter y = (MachineLetter)lowest_set_bit_index(ys);
        const MachineLetter low = x < y ? x : y;
        const MachineLetter high = x < y ? y : x;
        if (((tiles->pair_letters[low] >> high) & 1U) == 0) {
          continue;
        }
        child.tiles[blank_squares[0]] = get_blanked_machine_letter(x);
        child.tiles[blank_squares[1]] = get_blanked_machine_letter(y);
        wordsmog_pat_push(refine, child);
      }
    }
    return;
  }
  int counts[RACK_SIZE];
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    counts[letter_idx] = tiles->counts[letter_idx];
  }
  int blanks = tiles->num_blanks;
  for (int square_idx = 0; square_idx < node->num_pinned; square_idx++) {
    if (node->pins[square_idx] == WORDSMOG_PIN_BLANK) {
      blanks--;
      continue;
    }
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (tiles->letters[letter_idx] == node->pins[square_idx]) {
        counts[letter_idx]--;
      }
    }
  }
  MachineLetter pins[RACK_SIZE];
  memcpy(pins, node->pins, sizeof(pins));
  const uint64_t cross_set = group->squares[node->num_pinned].cross_set;
  for (int letter_idx = 0; letter_idx <= tiles->num_letters; letter_idx++) {
    if (letter_idx < tiles->num_letters) {
      pins[node->num_pinned] = tiles->letters[letter_idx];
      if (counts[letter_idx] == 0 ||
          ((cross_set >> tiles->letters[letter_idx]) & 1U) == 0) {
        continue;
      }
    } else {
      if (blanks == 0) {
        continue;
      }
      pins[node->num_pinned] = WORDSMOG_PIN_BLANK;
    }
    WordsmogArrangement arrangement;
    if (wordsmog_best_arrangement(group->squares, group->num_squares, tiles,
                                  pins, &arrangement) &&
        wordsmog_pat_node_init(gen, group, node->group_idx, pins,
                               node->num_pinned + 1, &arrangement, &child)) {
      wordsmog_pat_push(refine, child);
    }
  }
}

// Prices the play of group with tiles, whose score is score, and makes it the
// best play if it beats it. Returns whether it was priced.
// Writes the play of group with tiles, whose score is score, to move, without
// its equity.
static void wordsmog_pat_write_play(const WordsmogPATGroup *group,
                                    const MachineLetter *tiles, Equity score,
                                    Move *move) {
  MachineLetter strip[BOARD_DIM];
  memcpy(strip, group->strip, (size_t)group->word_length);
  for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
    strip[group->square_letter_idxs[square_idx]] = tiles[square_idx];
  }
  move_set_all_except_equity(move, strip, 0, group->word_length - 1, score,
                             group->row, group->start_col, group->tiles_played,
                             group->dir, GAME_EVENT_TILE_PLACEMENT_MOVE);
  if (board_is_dir_vertical(group->dir)) {
    move_set_row_start(move, group->start_col);
    move_set_col_start(move, group->row);
  }
}

// Under MOVE_RECORD_ALL, the equity the best plays of the groups need to make
// the move list: the capacity-th highest, or EQUITY_MIN_VALUE when fewer
// groups than that have plays (or the list is too long to track).
static Equity wordsmog_pat_list_threshold(const MoveGen *gen,
                                          const WordsmogPATRefine *refine) {
  Equity top[WORDSMOG_PAT_MAX_TRACKED_LIST] = {0};
  const int capacity = move_list_get_capacity(gen->move_list);
  if (capacity > WORDSMOG_PAT_MAX_TRACKED_LIST ||
      refine->num_groups < capacity) {
    return EQUITY_MIN_VALUE;
  }
  int count = 0;
  for (int group_idx = 0; group_idx < refine->num_groups; group_idx++) {
    const Equity equity = refine->groups[group_idx].best_equity;
    if (count == capacity && equity <= top[count - 1]) {
      continue;
    }
    int insert_idx = count < capacity ? count++ : capacity - 1;
    while (insert_idx > 0 && top[insert_idx - 1] < equity) {
      top[insert_idx] = top[insert_idx - 1];
      insert_idx--;
    }
    top[insert_idx] = equity;
  }
  return top[capacity - 1];
}

// Prices the play of group with tiles, whose score is score, and makes it the
// best play, or under MOVE_RECORD_ALL the group's, if it beats it. Returns
// whether it was priced.
static bool wordsmog_pat_price(MoveGen *gen, WordsmogPATGroup *group,
                               const MachineLetter *tiles, Equity score) {
  WordsmogPATRefine *refine = gen->wordsmog_pat;
  Move scratch;
  Move *move = refine->record_all ? &scratch : gen_get_current_move(gen);
  wordsmog_pat_write_play(group, tiles, score, move);
  const Equity equity_without_pat =
      static_eval_get_move_equity_with_leave_value(
          &gen->ld, move, &gen->leave, &gen->opponent_rack,
          gen->opening_move_word_penalties, gen->opening_move_letter_penalties,
          pat_eval_context_get_active_classes(&gen->pat_eval_ctx), NULL,
          gen->board_number_of_tiles_played, gen->number_of_tiles_in_bag,
          group->leave_value);
  if (wordsmog_pat_prunes(
          gen, group,
          equity_without_pat + group->defense_bound +
              pat_eval_utility_bound_for_play(&gen->pat_eval_ctx,
                                              group->tiles_played, score),
          score)) {
    return false;
  }
  if (refine->record_all) {
    // As gen_pat_contender_equity, against what the play has to reach: a play
    // that cannot reach it gets an overestimate that still falls short.
    const Equity needed_equity = group->best_equity > refine->threshold
                                     ? group->best_equity
                                     : refine->threshold;
    int64_t needed = (int64_t)needed_equity - equity_without_pat;
    if (needed < EQUITY_MIN_VALUE) {
      needed = EQUITY_MIN_VALUE;
    } else if (needed > EQUITY_MAX_VALUE) {
      needed = EQUITY_MAX_VALUE;
    }
    const Equity equity = equity_without_pat + pat_eval_move_penalty_capped(
                                                   &gen->pat_eval_ctx, move,
                                                   &gen->leave, (Equity)needed);
    if (equity > group->best_equity ||
        (equity == group->best_equity && score > group->best_score)) {
      const bool raises_threshold = equity > refine->threshold;
      memcpy(group->best_tiles, tiles,
             (size_t)group->num_squares * sizeof(MachineLetter));
      group->best_score = score;
      group->best_equity = equity;
      if (raises_threshold) {
        refine->threshold = wordsmog_pat_list_threshold(gen, refine);
      }
    }
    return true;
  }
  move_set_equity(move, gen_pat_contender_equity(gen, move, &gen->leave,
                                                 equity_without_pat));
  if (compare_moves(move, gen_get_readonly_best_move(gen), true) == 1) {
    gen_switch_best_move_and_current_move(gen);
    gen_update_cutoff_equity_or_score(gen);
  }
  return true;
}

// Prices the groups' plays a round at a time (see WordsmogPATGroup) until
// gen->wordsmog_pat_evals plays have been priced, counting under
// MOVE_RECORD_BEST the one of each group move generation recorded, or no play
// left could beat the best. Under MOVE_RECORD_ALL, then records each group's
// best play.
void wordsmog_pat_refine(MoveGen *gen) {
  WordsmogPATRefine *refine = gen->wordsmog_pat;
  if (refine == NULL || refine->num_groups == 0) {
    return;
  }
  int evals_left = gen->wordsmog_pat_evals;
  if (evals_left != INT_MAX && !refine->record_all) {
    evals_left -= refine->num_groups;
  }
  if (refine->record_all) {
    refine->threshold = wordsmog_pat_list_threshold(gen, refine);
  }
  while (evals_left > 0 && refine->heap_count > 0) {
    WordsmogPATNode node = wordsmog_pat_pop(refine);
    WordsmogPATGroup *group = &refine->groups[node.group_idx];
    if (evals_left != INT_MAX && node.round != group->evals) {
      // Its group has had a play priced since: back in line for the next
      // round.
      node.round = group->evals;
      wordsmog_pat_push(refine, node);
      continue;
    }
    if (wordsmog_pat_prunes(gen, group, node.bound, node.max_score)) {
      continue;
    }
    bool undesignated_blank = false;
    for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
      undesignated_blank |= get_is_blanked(node.tiles[square_idx]);
    }
    undesignated_blank &= !node.designated;
    if (node.num_pinned < group->num_squares || undesignated_blank) {
      wordsmog_pat_expand(gen, refine, &node);
      continue;
    }
    if (memcmp(node.tiles, group->recorded,
               (size_t)group->num_squares * sizeof(MachineLetter)) == 0) {
      continue;
    }
    if (wordsmog_pat_price(gen, group, node.tiles, node.score) &&
        evals_left != INT_MAX) {
      group->evals++;
      evals_left--;
    }
  }
  if (refine->record_all) {
    for (int group_idx = 0; group_idx < refine->num_groups; group_idx++) {
      const WordsmogPATGroup *group = &refine->groups[group_idx];
      Move *move = move_list_get_spare_move(gen->move_list);
      wordsmog_pat_write_play(group, group->best_tiles, group->best_score,
                              move);
      move_list_insert_spare_move(gen->move_list, group->best_equity);
    }
  }
  wordsmog_pat_refine_reset(refine);
}

// Records the best arrangement of tiles at each of the anchor's start
// columns, and with the PAT term on under MOVE_RECORD_BEST keeps its group
// for wordsmog_pat_refine; under MOVE_RECORD_ALL it keeps the group instead,
// for wordsmog_pat_refine to record. Returns true once the recording
// threshold has been exceeded.
static bool wordsmog_record_placements(MoveGen *gen, const Anchor *anchor,
                                       WordsmogTiles *tiles,
                                       Equity anchor_pat_bound) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  const bool pat_refine = gen->move_sort_type == MOVE_SORT_EQUITY &&
                          gen_pat_is_active(gen) &&
                          (gen->move_record_type == MOVE_RECORD_BEST ||
                           gen->move_record_type == MOVE_RECORD_ALL) &&
                          !gen->stop_on_threshold;
  int tiles_idx = -1;
  for (int start_col = anchor->leftmost_start_col;
       start_col <= anchor->rightmost_start_col; start_col++) {
    WordsmogSquare squares[RACK_SIZE];
    int square_letter_idxs[RACK_SIZE];
    Equity base_score;
    const int num_squares = wordsmog_placement_squares(
        gen, start_col, squares, square_letter_idxs, &base_score);
    assert(num_squares == anchor->tiles_to_play);
    const Equity score_bound =
        wordsmog_score_bound(squares, num_squares, tiles);
    if (wordsmog_placement_pruned(gen, base_score + score_bound,
                                  anchor_pat_bound) ||
        wordsmog_placement_pruned(
            gen,
            base_score + wordsmog_cross_set_bound(squares, num_squares, tiles,
                                                  score_bound),
            anchor_pat_bound)) {
      continue;
    }
    WordsmogArrangement arrangement;
    if (!wordsmog_best_arrangement(squares, num_squares, tiles, NULL,
                                   &arrangement)) {
      continue;
    }
    for (int square_idx = 0; square_idx < num_squares; square_idx++) {
      gen->playthrough_marked[square_letter_idxs[square_idx]] =
          arrangement.tiles[square_idx];
    }
    // A play's score is the placement's part plus its tiles', which the
    // prune above and the play graph rely on.
    assert(wmp_play_score(gen, start_col) == base_score + arrangement.score);
    if (!pat_refine || gen->move_record_type != MOVE_RECORD_ALL) {
      record_wmp_play(gen, start_col, wgen->leave_value);
    }
    if (gen->threshold_exceeded) {
      return true;
    }
    if (pat_refine) {
      wordsmog_pat_add_group(gen, start_col, squares, square_letter_idxs,
                             num_squares, tiles, &tiles_idx, base_score,
                             score_bound, &arrangement);
    }
  }
  return false;
}

// As wordmap_gen_record_subrack, but plays the subrack's best arrangements
// instead of its words. Returns true once the recording threshold has been
// exceeded.
static bool wordsmog_record_subrack(MoveGen *gen, const Anchor *anchor,
                                    int subrack_idx, bool lazy,
                                    Equity anchor_pat_bound) {
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  if (gen->wmp_prune_subracks_by_leave) {
    wgen->leave_value = wmp_move_gen_get_leave_value(wgen, subrack_idx);
  }
  if (wordmap_gen_leave_prunes_subrack(gen, anchor, subrack_idx,
                                       anchor_pat_bound)) {
    return false;
  }
  BitRack main_word;
  const WMPEntry *entry =
      wmp_move_gen_get_subrack_entry(wgen, subrack_idx, lazy, &main_word);
  if (entry == NULL) {
    return false;
  }
  if (gen->number_of_tiles_in_bag == 0) {
    wordmap_gen_set_empty_bag_leave(gen, anchor, subrack_idx);
  }
  WordsmogTiles tiles;
  wordsmog_tiles_init(
      gen, wmp_move_gen_get_nonplaythrough_subrack(wgen, subrack_idx),
      &main_word, entry, &tiles);
  return wordsmog_record_placements(gen, anchor, &tiles, anchor_pat_bound);
}

// As wordmap_gen for wordsmog: the anchor's candidate subracks, each played in
// its best arrangement. The word info table is off in wordsmog, and
// the RIT's bingo words are spellings, so their prunes and fast path are
// skipped.
void wordsmog_wmp_gen(MoveGen *gen, const Anchor *anchor, bool lazy) {
  gen->max_tiles_to_play = anchor->tiles_to_play;
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  // Upper bound on the PAT term of every move from this anchor (zero when
  // the term is off). See wordmap_gen_leave_prunes_subrack.
  const Equity anchor_pat_bound =
      gen_wmp_anchor_pat_bound(gen, anchor->dir, anchor->row, anchor);
  wmp_move_gen_set_playthrough_bit_rack(wgen, anchor, gen->row_squares, NULL,
                                        gen->wit_len_lane, NULL);
  wmp_move_gen_playthrough_metadata_init(wgen, anchor);
  wmp_move_gen_build_playthrough_subracks(wgen);
  const int num_subrack_combinations =
      wmp_move_gen_get_num_subrack_combinations(wgen);
  for (int first_idx = 0; first_idx < num_subrack_combinations;
       first_idx += WMP_MOVE_GEN_SUBRACKS_PER_MASK) {
    const int remaining = num_subrack_combinations - first_idx;
    const int count = remaining < WMP_MOVE_GEN_SUBRACKS_PER_MASK
                          ? remaining
                          : WMP_MOVE_GEN_SUBRACKS_PER_MASK;
    uint64_t filter_misses = 0;
    uint64_t candidates = wordmap_gen_candidate_subracks(
        gen, anchor, first_idx, count, lazy, anchor_pat_bound, &filter_misses);
    while (candidates != 0) {
      const int bit_idx = wmp_move_gen_get_lowest_set_bit(candidates);
      candidates &= candidates - 1;
      const uint64_t misses_below = filter_misses & ((1ULL << bit_idx) - 1);
      if (misses_below != 0) {
        wordmap_gen_resolve_filter_misses(gen, anchor, first_idx, misses_below,
                                          anchor_pat_bound);
        filter_misses ^= misses_below;
      }
      if (wordsmog_record_subrack(gen, anchor, first_idx + bit_idx, lazy,
                                  anchor_pat_bound)) {
        return;
      }
    }
    if (filter_misses != 0) {
      wordmap_gen_resolve_filter_misses(gen, anchor, first_idx, filter_misses,
                                        anchor_pat_bound);
    }
  }
}
