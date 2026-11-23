// Checks that the leaves of a pool (PoolLeaves in pool_shift.h) and pool
// shift tables are built value for value as they are defined, against
// references that share no code with the builders:
//
// - A brute force that, for every leave L, sums over every draw D that fills
//   it to a full rack, in exact integers:
//
//     S(L) = sum over D with D_t <= pool_t and L_t + D_t <= dist_t for every
//            t, and the full rack L + D valued, of
//            prod_t C(pool_t, D_t) * value(L + D)
//     N(L) = the same sum of prod_t C(pool_t, D_t)
//
//   where pool_t counts the bag, the opponent's rack and, for the public
//   pool, the mover's rack. The empty leave is worth E = S / N, or 0 where
//   N is 0. A leave of 1 to RACK_SIZE - 1 tiles that the distribution can
//   deal is worth llround(S / N - E) in the pool's KLV, or its stock value
//   where N is 0, and every other KLV entry keeps its stock value. The
//   table's reference_exact_offset is llround(E). The walk sums in doubles
//   and pool_leaves_create in int64, and both are exact integers for every
//   input here (checked), so they and the brute force must agree to the
//   millipoint.
// - A table built cell by cell with klv_get_leave_value, its rows numbered
//   by the census formula in pool_shift.h: the value of each dealable leave
//   with one more of each tile it does not hold every copy of, and 0 in every
//   other cell, padding included.
// - pool_leaves_create_by_walk, the original walk, compared with
//   pool_leaves_create on every value.
//
// The synthetic cases make their own letter distributions, KLVs (every
// leave, at random nonzero values) and full racks' values (random, some
// unvalued), and need no data files. The CSW24 pool cases need
// data/lexica/CSW24.rvt and say in one line when they are skipped.
//
// "poolleavesequiv", the unit test: the synthetic cases, the stock CSW24
// table, and a sparse CSW24 pool against the brute force.
// "poolleavesequiv:games=<n>[,first=<g>]", on demand: constructed CSW24
// pools, a mid-game position, then every position of self-play games
// first..first+n-1 whose mover holds a full rack with at least a rack in the
// bag, each with the public pool and the private one.

#include "pool_leaves_equivalence_test.h"

#include "../src/compat/cfile.h"
#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/klv_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/klv_csv.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/pool_shift.h"
#include "../src/ent/rack.h"
#include "../src/ent/rack_value_table.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // The longest leave a pool's KLV values.
  PLE_MAX_LEAVE_SIZE = RACK_SIZE - 1,
  // The numbers of threads the builds are also run on: 2, then every
  // PLE_BUILD_THREADS_STEP more up to PLE_MAX_BUILD_THREADS.
  PLE_MAX_BUILD_THREADS = 12,
  PLE_BUILD_THREADS_STEP = 5,
  // The synthetic leaves' values and full racks' values, in millipoints.
  PLE_LEAVE_VALUE_RANGE = 30000,
  PLE_RACK_VALUE_RANGE = 60000,
  PLE_TIE_VALUE_RANGE = 3,
  // One synthetic rack in this many is unvalued, and one valued rack in this
  // many is at a limit of what a rack value table holds.
  PLE_UNVALUED_ONE_IN = 6,
  PLE_EXTREME_ONE_IN = 40,
  PLE_EXTREME_MIN_ONE_IN = 4,
  // Room for a leave's letters in a failure message.
  PLE_LEAVE_STRING_SIZE = 64,
  // The rack size of the tables in the data directory.
  PLE_DATA_RACK_SIZE = 7,
  // Self-play game g is seeded with PLE_FIRST_GAME_SEED + g. The constructed
  // mid-game position is the one after PLE_MID_GAME_MOVES moves of the game
  // seeded with PLE_MID_GAME_SEED.
  PLE_FIRST_GAME_SEED = 7100,
  PLE_MID_GAME_SEED = 7099,
  PLE_MID_GAME_MOVES = 6,
  // The brute force lists the draws of a pool: sparse pools have few.
  PLE_MAX_BRUTE_FORCE_DRAWS = 1 << 22,
  PLE_SPEC_GAMES_ONLY_FIELDS = 1,
  PLE_SPEC_ALL_FIELDS = 2,
};

// The builders' sums are exact in doubles below this.
#define PLE_EXACT_DOUBLE_LIMIT (INT64_C(1) << 53)

// How far a value rounded to a millipoint may be from its mean summed in
// doubles: half a millipoint, and the error of the doubles' sums.
#define PLE_MEAN_TOLERANCE 0.51

typedef enum {
  // Random values, about one rack in PLE_UNVALUED_ONE_IN unvalued and a few
  // at the limits of what a table holds.
  PLE_RACK_VALUES_RANDOM,
  // Every rack valued, from -PLE_TIE_VALUE_RANGE to PLE_TIE_VALUE_RANGE
  // millipoints: means that end in exactly a half are common, and llround
  // takes them away from zero.
  PLE_RACK_VALUES_TIES,
  // As random, but every rack holding the case's letter is unvalued: no draw
  // values a leave holding it, which keeps its stock value.
  PLE_RACK_VALUES_WITHOUT_LETTER,
  // Every rack unvalued: every leave keeps its stock value.
  PLE_RACK_VALUES_NONE,
  // Every rack at a limit of what a table holds, the lower one in
  // PLE_EXTREME_MIN_ONE_IN, so that sums cancel little: with a large pool
  // the builders' sums pass 2^53, or 2^63.
  PLE_RACK_VALUES_EXTREME,
} PleRackValues;

// A pool, as the tiles of the bag, the opponent's rack and the mover's rack.
// A NULL bag is every tile of the distribution on neither rack, less every
// copy of the letters in absent_letters.
typedef struct PlePool {
  const char *name;
  const char *bag;
  const char *absent_letters;
  const char *opponent_rack;
  const char *mover_rack;
  // Whether the brute force is cheap enough for this pool.
  bool brute_force;
} PlePool;

// The cases below are easier to read in this order than in the tightest.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
typedef struct PleSyntheticCase {
  const char *name;
  // A letter distribution file's contents.
  const char *distribution;
  // The distribution the KLV's leaves are dealt from, when it is not the one
  // above: the KLV then lacks some leaves the distribution deals, or holds
  // some it does not.
  const char *klv_distribution;
  PleRackValues rack_values;
  // For PLE_RACK_VALUES_WITHOUT_LETTER.
  MachineLetter unvalued_letter;
  uint64_t seed;
  // Whether the builders' sums are exact integers, so that the brute force
  // applies; otherwise only the walk does.
  bool sums_are_exact;
  const PlePool *pools;
  int number_of_pools;
  // Whether the pools have more unseen tiles than the table's exact leave
  // values can sum in int64 (see exact_sums_max_unseen).
  bool root_sums_are_wide;
} PleSyntheticCase;

// What was compared.
typedef struct PleTally {
  int builds;
  // Pool KLV values, table cells and reference pool fields.
  uint64_t values;
} PleTally;

typedef struct PleBuildTimes {
  double create_seconds;
  double walk_seconds;
  // The table cell by cell, and the brute force.
  double table_check_seconds;
  double brute_force_seconds;
} PleBuildTimes;

// Seven letters, 14 tiles: two blanks, letters of 1 to 4 copies, and a row
// padded to 8 values.
static const char *const ple_mixed_distribution = "?,?,2,0,0\n"
                                                  "A,a,3,1,1\n"
                                                  "B,b,1,3,0\n"
                                                  "C,c,2,3,0\n"
                                                  "D,d,1,2,0\n"
                                                  "E,e,4,1,1\n"
                                                  "F,f,1,4,0\n";

// The same letters with more copies (the KLV holds leaves the distribution
// cannot deal) and with fewer (it lacks some the distribution deals).
static const char *const ple_mixed_more_distribution = "?,?,2,0,0\n"
                                                       "A,a,3,1,1\n"
                                                       "B,b,2,3,0\n"
                                                       "C,c,2,3,0\n"
                                                       "D,d,1,2,0\n"
                                                       "E,e,4,1,1\n"
                                                       "F,f,2,4,0\n";

static const char *const ple_mixed_fewer_distribution = "?,?,1,0,0\n"
                                                        "A,a,2,1,1\n"
                                                        "B,b,1,3,0\n"
                                                        "C,c,1,3,0\n"
                                                        "D,d,1,2,0\n"
                                                        "E,e,3,1,1\n"
                                                        "F,f,1,4,0\n";

// Three letters, 9 tiles, rows of 4 values with one of padding.
static const char *const ple_three_letter_distribution = "?,?,2,0,0\n"
                                                         "A,a,6,1,1\n"
                                                         "B,b,1,3,0\n";

// Thirteen letters, 35 tiles, one with 9 copies.
static const char *const ple_wide_distribution = "?,?,2,0,0\n"
                                                 "A,a,5,1,1\n"
                                                 "B,b,1,3,0\n"
                                                 "C,c,2,3,0\n"
                                                 "D,d,3,2,0\n"
                                                 "E,e,9,1,1\n"
                                                 "F,f,1,4,0\n"
                                                 "G,g,2,2,0\n"
                                                 "H,h,1,4,0\n"
                                                 "I,i,4,1,1\n"
                                                 "J,j,1,8,0\n"
                                                 "K,k,1,5,0\n"
                                                 "L,l,3,1,0\n";

// Exactly a rack's tiles: one full rack.
static const char *const ple_one_rack_distribution = "?,?,2,0,0\n"
                                                     "A,a,3,1,1\n"
                                                     "B,b,2,3,0\n";

// Fewer tiles than a rack: no full rack at all.
static const char *const ple_short_distribution = "?,?,1,0,0\n"
                                                  "A,a,2,1,1\n"
                                                  "B,b,1,3,0\n";

// Pools of 50, 82 and 202 tiles. With every rack at a limit of what a table
// holds, the largest sum of the first stays below 2^53, so that the
// builders' doubles are exact, the second's passes 2^53, and the third's
// passes 2^63: (2^23 - 1) * C(202, 7) is about 2.3e19.
static const char *const ple_near_bound_distribution = "?,?,2,0,0\n"
                                                       "A,a,16,1,1\n"
                                                       "B,b,16,3,0\n"
                                                       "C,c,16,3,0\n";

static const char *const ple_large_distribution = "?,?,2,0,0\n"
                                                  "A,a,30,1,1\n"
                                                  "B,b,30,3,0\n"
                                                  "C,c,20,3,0\n";

static const char *const ple_huge_distribution = "?,?,2,0,0\n"
                                                 "A,a,100,1,1\n"
                                                 "B,b,100,3,0\n";

// More tiles than fit the sums of the exact leave values whatever a table
// holds, even split into two digits.
static const char *const ple_vast_distribution = "?,?,2,0,0\n"
                                                 "A,a,250,1,1\n"
                                                 "B,b,250,3,0\n";

static const PlePool ple_mixed_pools[] = {
    {"every tile in the bag", "??AAABCCDEEEEF", NULL, "", "", true},
    // The private pool is exactly a rack, the public one every tile.
    {"both racks full", "AEF", NULL, "?ACE", "?ABCDEE", true},
    // The private pool holds four letters, no blank, no D and no F.
    {"blanks on the mover's rack", "AACEE", NULL, "BE", "??ACDEF", true},
    {"two letters, every A and E", "AAAEEEE", NULL, "", "", true},
    {"partial racks", "?ABCE", NULL, "DE", "AEF", true},
    {"fewer tiles than a rack", "?E", NULL, "C", "", true},
    {"nothing unseen", "", NULL, "", "", true},
};

static const PlePool ple_mixed_some_pools[] = {
    {"every tile in the bag", "??AAABCCDEEEEF", NULL, "", "", true},
    {"both racks full", "AEF", NULL, "?ACE", "?ABCDEE", true},
    {"blanks on the mover's rack", "AACEE", NULL, "BE", "??ACDEF", true},
    {"partial racks", "?ABCE", NULL, "DE", "AEF", true},
};

static const PlePool ple_three_letter_pools[] = {
    {"every tile in the bag", "??AAAAAAB", NULL, "", "", true},
    {"blanks on the mover's rack", "AAA", NULL, "AB", "??AA", true},
    {"one letter", "AAAAAA", NULL, "", "??B", true},
};

static const PlePool ple_wide_pools[] = {
    {"both racks full", "AAACDDEEEEEFGGHIIIJLL", NULL, "ABCDEEK", "??AEEIL",
     true},
    {"partial bag", "AEEGIL", NULL, "DEJ", "??ABEIK", true},
    // The private pool has no blank, B, F, H, J or K.
    {"letters missing", "ACCDDDEEEEGGIIILLL", NULL, "AE", "??BEFHJ", true},
};

static const PlePool ple_one_rack_pools[] = {
    {"every tile in the bag", "??AAABB", NULL, "", "", true},
    {"blanks on the mover's rack", "AB", NULL, "A", "??AB", true},
};

static const PlePool ple_short_pools[] = {
    {"every tile in the bag", "?AAB", NULL, "", "", true},
};

static const PlePool ple_large_pools[] = {
    {"every tile in the bag", NULL, "", "", "", true},
    {"both racks full", NULL, "", "?AABBCC", "?AAABBB", true},
    {"no blank or C", NULL, "?C", "AAABBBB", "", true},
};

static const PlePool ple_huge_pools[] = {
    {"every tile in the bag", NULL, "", "", "", true},
    {"both racks full", NULL, "", "?AABBBB", "?AAABBB", true},
};

#define PLE_POOLS(pools) (pools), (int)(sizeof(pools) / sizeof((pools)[0]))

static const PleSyntheticCase ple_synthetic_cases[] = {
    {"mixed, random", ple_mixed_distribution, NULL, PLE_RACK_VALUES_RANDOM, 0,
     11, true, PLE_POOLS(ple_mixed_pools), false},
    {"mixed, ties", ple_mixed_distribution, NULL, PLE_RACK_VALUES_TIES, 0, 12,
     true, PLE_POOLS(ple_mixed_pools), false},
    // D has one copy: no rack holding it is valued.
    {"mixed, no rack with D valued", ple_mixed_distribution, NULL,
     PLE_RACK_VALUES_WITHOUT_LETTER, 4, 13, true,
     PLE_POOLS(ple_mixed_some_pools), false},
    {"mixed, no rack valued", ple_mixed_distribution, NULL,
     PLE_RACK_VALUES_NONE, 0, 14, true, PLE_POOLS(ple_mixed_some_pools), false},
    {"mixed, KLV of more copies", ple_mixed_distribution,
     ple_mixed_more_distribution, PLE_RACK_VALUES_RANDOM, 0, 15, true,
     PLE_POOLS(ple_mixed_some_pools), false},
    {"mixed, KLV of fewer copies", ple_mixed_distribution,
     ple_mixed_fewer_distribution, PLE_RACK_VALUES_RANDOM, 0, 16, true,
     PLE_POOLS(ple_mixed_some_pools), false},
    {"three letters, random", ple_three_letter_distribution, NULL,
     PLE_RACK_VALUES_RANDOM, 0, 17, true, PLE_POOLS(ple_three_letter_pools),
     false},
    {"wide, random", ple_wide_distribution, NULL, PLE_RACK_VALUES_RANDOM, 0, 18,
     true, PLE_POOLS(ple_wide_pools), false},
    // No rack holding a blank is valued.
    {"wide, no rack with a blank valued", ple_wide_distribution, NULL,
     PLE_RACK_VALUES_WITHOUT_LETTER, 0, 19, true, PLE_POOLS(ple_wide_pools),
     false},
    {"one rack, ties", ple_one_rack_distribution, NULL, PLE_RACK_VALUES_TIES, 0,
     20, true, PLE_POOLS(ple_one_rack_pools), false},
    {"fewer tiles than a rack", ple_short_distribution, NULL,
     PLE_RACK_VALUES_RANDOM, 0, 21, true, PLE_POOLS(ple_short_pools), false},
    {"50 tiles, extreme values", ple_near_bound_distribution, NULL,
     PLE_RACK_VALUES_EXTREME, 0, 22, true, PLE_POOLS(ple_large_pools), false},
    {"82 tiles, ties", ple_large_distribution, NULL, PLE_RACK_VALUES_TIES, 0,
     23, true, PLE_POOLS(ple_large_pools), false},
    // Past 2^53 a builder that sums exactly can differ from the walk's
    // doubles, and past 2^63 overflow: either must build as the walk does.
    {"82 tiles, extreme values", ple_large_distribution, NULL,
     PLE_RACK_VALUES_EXTREME, 0, 24, false, PLE_POOLS(ple_large_pools), false},
    // The sums of the exact leave values pass 2^63 as well.
    {"202 tiles, extreme values", ple_huge_distribution, NULL,
     PLE_RACK_VALUES_EXTREME, 0, 25, false, PLE_POOLS(ple_huge_pools), true},
    // Small values, which a table's own values show to fit: split by force,
    // the exact leave values take two digits and three.
    {"202 tiles, ties", ple_huge_distribution, NULL, PLE_RACK_VALUES_TIES, 0,
     26, false, PLE_POOLS(ple_huge_pools), false},
    {"502 tiles, ties", ple_vast_distribution, NULL, PLE_RACK_VALUES_TIES, 0,
     27, false, PLE_POOLS(ple_huge_pools), false},
};

// CSW24 pools for the on-demand test. The unit test checks the private pool
// of the first.
static const PlePool ple_csw24_pools[] = {
    {"sparse, seven tiles", "EINRT", NULL, "?Q", "AADEILS", true},
    {"sparse, ten tiles", "ADEILNR", NULL, "?QZ", "EIOSTUU", true},
    {"exactly a rack", "AEIORST", NULL, "", "??EEINR", true},
    {"no blank, E, S, X or Z", NULL, "?ESXZ", "AINORTT", "DGILNOU", false},
    {"both blanks in the pool", "??AABCDEEEEFGHIILMNNOOPRRSTTUVWY", NULL,
     "AEIKNOT", "BDEJLRS", false},
    {"both blanks on the mover's rack", "AABDEEEEGIILMNOOPRSTTUWY", NULL,
     "CEHIKNO", "??AEIRS", false},
    {"Q without U", "AADEEEGIILNOQRST", NULL, "BEHINOR", "ACEFLMT", true},
};

static int64_t ple_binomial(int count, int chosen) {
  if (chosen < 0 || chosen > count) {
    return 0;
  }
  int64_t result = 1;
  for (int factor_idx = 0; factor_idx < chosen; factor_idx++) {
    result = result * (count - factor_idx) / (factor_idx + 1);
  }
  return result;
}

// The number of multisets of tiles tiles over symbols symbols.
static int64_t ple_count_multisets(int tiles, int symbols) {
  if (symbols == 0) {
    return tiles == 0 ? 1 : 0;
  }
  return ple_binomial(symbols + tiles - 1, tiles);
}

// The row of a leave by the numbering pool_shift.h describes: the start of
// the block of its size, plus sum_i C(a_i + i, i + 1) over its tiles
// numbered in reverse alphabet order, a_0 <= ... <= a_(k-1).
static int64_t ple_census_rank(const Rack *leave, int ld_size) {
  int64_t rank = 0;
  for (int size = 0; size < rack_get_total_letters(leave); size++) {
    rank += ple_count_multisets(size, ld_size);
  }
  int tile_idx = 0;
  for (int ml = ld_size - 1; ml >= 0; ml--) {
    const int reverse_number = ld_size - 1 - ml;
    for (int copy_idx = 0; copy_idx < rack_get_letter(leave, (MachineLetter)ml);
         copy_idx++) {
      rank += ple_binomial(reverse_number + tile_idx, tile_idx + 1);
      tile_idx++;
    }
  }
  return rank;
}

static void ple_get_dist_counts(const LetterDistribution *ld,
                                int dist_counts[MAX_ALPHABET_SIZE]) {
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    dist_counts[ml] =
        ml < ld_get_size(ld) ? ld_get_dist(ld, (MachineLetter)ml) : 0;
  }
}

static bool ple_is_dealable(const Rack *leave, const LetterDistribution *ld) {
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    if (rack_get_letter(leave, (MachineLetter)ml) >
        ld_get_dist(ld, (MachineLetter)ml)) {
      return false;
    }
  }
  return true;
}

typedef void (*PleLeaveVisitor)(void *context, const Rack *leave);

static void ple_visit_multisets_from(Rack *leave, int first_ml, int max_size,
                                     const int *limits, PleLeaveVisitor visitor,
                                     void *context) {
  visitor(context, leave);
  if (rack_get_total_letters(leave) == max_size) {
    return;
  }
  for (int ml = first_ml; ml < rack_get_dist_size(leave); ml++) {
    if (limits != NULL &&
        rack_get_letter(leave, (MachineLetter)ml) >= limits[ml]) {
      continue;
    }
    rack_add_letter(leave, (MachineLetter)ml);
    ple_visit_multisets_from(leave, ml, max_size, limits, visitor, context);
    rack_take_letter(leave, (MachineLetter)ml);
  }
}

// Visits every multiset of at most max_size tiles of the ld_size letters,
// each once and the empty one first, with at most limits[ml] tiles of each
// letter ml, or any number with NULL limits.
static void ple_visit_multisets(int ld_size, int max_size, const int *limits,
                                PleLeaveVisitor visitor, void *context) {
  Rack leave;
  rack_set_dist_size_and_reset(&leave, ld_size);
  ple_visit_multisets_from(&leave, 0, max_size, limits, visitor, context);
}

// The leave's letters, for a failure message.
static const char *ple_leave_string(const Rack *leave,
                                    const LetterDistribution *ld, char *buffer,
                                    size_t buffer_size) {
  if (rack_is_empty(leave)) {
    return "(empty)";
  }
  rack_get_string(leave, ld, true, buffer, buffer_size);
  return buffer;
}

// A table column's letter, for a failure message.
static const char *ple_letter_string(int column, const LetterDistribution *ld,
                                     char *buffer, size_t buffer_size) {
  if (column >= ld_get_size(ld)) {
    return "(padding)";
  }
  Rack letter;
  rack_set_dist_size_and_reset(&letter, ld_get_size(ld));
  rack_add_letter(&letter, (MachineLetter)column);
  return ple_leave_string(&letter, ld, buffer, buffer_size);
}

// Finds the leave at a KLV index, or with a NULL klv the leave of a table
// row, for a failure message.
typedef struct PleLeaveSearch {
  const KLV *klv;
  int ld_size;
  int64_t target;
  bool found;
  Rack leave;
} PleLeaveSearch;

static void ple_search_leave(void *context, const Rack *leave) {
  PleLeaveSearch *search = (PleLeaveSearch *)context;
  if (search->found) {
    return;
  }
  int64_t key = 0;
  if (search->klv != NULL) {
    if (rack_is_empty(leave)) {
      return;
    }
    key = (int64_t)klv_get_word_index(search->klv, leave);
  } else {
    key = ple_census_rank(leave, search->ld_size);
  }
  if (key == search->target) {
    search->found = true;
    rack_copy(&search->leave, leave);
  }
}

static const char *ple_describe_leave(const KLV *klv, int64_t target,
                                      const LetterDistribution *ld,
                                      char *buffer, size_t buffer_size) {
  PleLeaveSearch search = {
      .klv = klv, .ld_size = ld_get_size(ld), .target = target};
  ple_visit_multisets(ld_get_size(ld),
                      klv != NULL ? PLE_MAX_LEAVE_SIZE
                                  : POOL_SHIFT_MAX_LEAVE_SIZE,
                      NULL, ple_search_leave, &search);
  if (!search.found) {
    return "(no leave)";
  }
  return ple_leave_string(&search.leave, ld, buffer, buffer_size);
}

// The KLV of a pool keeps the stock KLV's graph, word counts, name and
// number of leaves.
static void ple_assert_klv_shape(const char *label, const KLV *klv,
                                 const KLV *stock) {
  if (klv_get_number_of_leaves(klv) != klv_get_number_of_leaves(stock)) {
    log_fatal("%s: the pool KLV has %u leaves, the stock KLV %u", label,
              klv_get_number_of_leaves(klv), klv_get_number_of_leaves(stock));
  }
  const KWG *kwg = klv_get_kwg(klv);
  const KWG *stock_kwg = klv_get_kwg(stock);
  const int number_of_nodes = kwg_get_number_of_nodes(stock_kwg);
  if (kwg_get_number_of_nodes(kwg) != number_of_nodes) {
    log_fatal("%s: the pool KLV's graph has %d nodes, the stock's %d", label,
              kwg_get_number_of_nodes(kwg), number_of_nodes);
  }
  for (int node_idx = 0; node_idx < number_of_nodes; node_idx++) {
    if (kwg_node(kwg, (uint32_t)node_idx) !=
        kwg_node(stock_kwg, (uint32_t)node_idx)) {
      log_fatal("%s: KWG node %d: expected %08x, got %08x", label, node_idx,
                kwg_node(stock_kwg, (uint32_t)node_idx),
                kwg_node(kwg, (uint32_t)node_idx));
    }
    if (klv->word_counts[node_idx] != stock->word_counts[node_idx]) {
      log_fatal("%s: word count of node %d: expected %u, got %u", label,
                node_idx, stock->word_counts[node_idx],
                klv->word_counts[node_idx]);
    }
  }
  if (!strings_equal(klv_get_name(klv), klv_get_name(stock))) {
    log_fatal("%s: the pool KLV is named %s, the stock KLV %s", label,
              klv_get_name(klv), klv_get_name(stock));
  }
}

// Whether the leaves of a pool leave out a leave: left_out_counts are the
// tiles of a pool that holds the mover's rack, and it has none of a letter
// the leave holds. With NULL left_out_counts no leave is left out.
static bool ple_is_left_out(const Rack *leave, int ld_size,
                            const int *left_out_counts) {
  if (left_out_counts == NULL) {
    return false;
  }
  for (int ml = 0; ml < ld_size; ml++) {
    if (left_out_counts[ml] == 0 &&
        rack_get_letter(leave, (MachineLetter)ml) > 0) {
      return true;
    }
  }
  return false;
}

typedef struct PleTableCheck {
  const char *label;
  const PoolShiftTable *table;
  const KLV *klv;
  const LetterDistribution *ld;
  // See ple_is_left_out.
  const int *left_out_counts;
  bool *row_seen;
  int64_t rows_seen;
  uint64_t cells_compared;
} PleTableCheck;

static void ple_check_table_row(void *context, const Rack *leave) {
  PleTableCheck *check = (PleTableCheck *)context;
  const PoolShiftTable *table = check->table;
  const LetterDistribution *ld = check->ld;
  char leave_string[PLE_LEAVE_STRING_SIZE];
  const int64_t rank = ple_census_rank(leave, table->ld_size);
  if (rank < 0 || rank >= (int64_t)table->number_of_rows ||
      check->row_seen[rank]) {
    log_fatal("%s: leave %s has census rank %" PRId64
              ", outside the table's %u rows or repeated",
              check->label,
              ple_leave_string(leave, ld, leave_string, sizeof(leave_string)),
              rank, table->number_of_rows);
  }
  const uint32_t row = pool_shift_table_get_leave_row(table, leave);
  if ((int64_t)row != rank) {
    log_fatal("%s: leave %s: census rank %" PRId64 ", table row %u",
              check->label,
              ple_leave_string(leave, ld, leave_string, sizeof(leave_string)),
              rank, row);
  }
  check->row_seen[rank] = true;
  check->rows_seen++;
  // A leave with a letter the pool has none of has no row, and no row has a
  // value for such a letter.
  const bool dealable =
      ple_is_dealable(leave, ld) &&
      !ple_is_left_out(leave, table->ld_size, check->left_out_counts);
  Rack extended;
  rack_copy(&extended, leave);
  const Equity *row_values =
      table->values + ((size_t)rank * (size_t)table->stride);
  for (int column = 0; column < table->stride; column++) {
    Equity expected = 0;
    const bool column_left_out = column < table->ld_size &&
                                 check->left_out_counts != NULL &&
                                 check->left_out_counts[column] == 0;
    if (!dealable || column >= table->ld_size || column_left_out) {
      expected = 0;
    } else if (rack_get_letter(leave, (MachineLetter)column) <
               ld_get_dist(ld, (MachineLetter)column)) {
      rack_add_letter(&extended, (MachineLetter)column);
      expected = klv_get_leave_value(check->klv, &extended);
      rack_take_letter(&extended, (MachineLetter)column);
    } else if (table->has_reference_pool) {
      // The exhausted-tile fill: a tile the leave holds every copy of takes
      // the leave's own value in a pool's table.
      expected = klv_get_leave_value(check->klv, leave);
    }
    if (row_values[column] != expected) {
      char letter_string[PLE_LEAVE_STRING_SIZE];
      log_fatal(
          "%s: table cell (row %" PRId64
          ", column %d): leave %s plus %s: expected %d, got %d",
          check->label, rank, column,
          ple_leave_string(leave, ld, leave_string, sizeof(leave_string)),
          ple_letter_string(column, ld, letter_string, sizeof(letter_string)),
          expected, row_values[column]);
    }
    check->cells_compared++;
  }
}

// Every value of the table, cell by cell, against klv_get_leave_value, and
// its numbering against pool_shift.h's.
static void
ple_assert_table_matches_klv(const char *label, const PoolShiftTable *table,
                             const KLV *klv, const LetterDistribution *ld,
                             const int *left_out_counts, PleTally *tally) {
  const int ld_size = ld_get_size(ld);
  if (table->ld_size != ld_size || !pool_shift_table_matches_ld(table, ld)) {
    log_fatal("%s: the table is for another letter distribution", label);
  }
  // Rows are padded to a multiple of four values.
  if (table->stride != (ld_size + 3) / 4 * 4) {
    log_fatal("%s: table stride %d for %d letters", label, table->stride,
              ld_size);
  }
  int64_t expected_rows = 0;
  for (int size = 0; size <= POOL_SHIFT_MAX_LEAVE_SIZE; size++) {
    expected_rows += ple_count_multisets(size, ld_size);
  }
  if ((int64_t)table->number_of_rows != expected_rows) {
    log_fatal("%s: the table has %u rows, expected %" PRId64, label,
              table->number_of_rows, expected_rows);
  }
  for (int size = 0; size < RACK_SIZE; size++) {
    for (int ml = 0; ml < ld_size; ml++) {
      const int64_t expected_increment =
          ple_count_multisets(size, ld_size) +
          ple_binomial(ld_size - 1 - ml + size, size + 1);
      if ((int64_t)table->rank_increments[size][ml] != expected_increment) {
        log_fatal("%s: rank increment [%d][%d]: expected %" PRId64 ", got %u",
                  label, size, ml, expected_increment,
                  table->rank_increments[size][ml]);
      }
    }
  }
  if (table->klv != klv || !pool_shift_table_matches_klv(table, klv)) {
    log_fatal(
        "%s: the table is not bound to its KLV as it is (instance %" PRIu64
        " of %" PRIu64 ", mutation %" PRIu64 " of %" PRIu64 ")",
        label, table->klv_instance_id, klv_get_instance_id(klv),
        table->klv_mutation_counter, klv_get_mutation_counter(klv));
  }
  PleTableCheck check = {
      .label = label,
      .table = table,
      .klv = klv,
      .ld = ld,
      .left_out_counts = left_out_counts,
      .row_seen = calloc_or_die(table->number_of_rows, sizeof(bool)),
  };
  ple_visit_multisets(ld_size, POOL_SHIFT_MAX_LEAVE_SIZE, NULL,
                      ple_check_table_row, &check);
  if (check.rows_seen != (int64_t)table->number_of_rows) {
    log_fatal("%s: %" PRId64 " leaves numbered, the table has %u rows", label,
              check.rows_seen, table->number_of_rows);
  }
  free(check.row_seen);
  tally->values += check.cells_compared;
}

// A table as pool_shift_table_create leaves it, with the full racks' values
// given since.
static void ple_assert_stock_table(const char *label,
                                   const PoolShiftTable *table, const KLV *klv,
                                   const LetterDistribution *ld,
                                   const RackValueTable *exact_values,
                                   PleTally *tally) {
  if (table->exact_values != exact_values || table->has_reference_pool ||
      table->reference_exact_offset != 0) {
    log_fatal("%s: a stock table has a reference pool or offset", label);
  }
  ple_assert_table_matches_klv(label, table, klv, ld, NULL, tally);
}

// The leaves of a pool keep the stock KLV's shape, and their table is bound
// to them and remembers the pool.
static void ple_assert_pool_leaves_bound(const char *label,
                                         const PoolLeaves *pool_leaves,
                                         const PoolShiftTable *stock_table,
                                         const LetterDistribution *ld,
                                         const int *pool_counts,
                                         PleTally *tally) {
  const PoolShiftTable *table = pool_leaves->table;
  ple_assert_klv_shape(label, pool_leaves->klv, stock_table->klv);
  if (table->klv != pool_leaves->klv ||
      table->klv_instance_id != klv_get_instance_id(pool_leaves->klv) ||
      table->klv_mutation_counter !=
          klv_get_mutation_counter(pool_leaves->klv)) {
    log_fatal("%s: the pool table is not bound to the pool KLV", label);
  }
  if (!table->has_reference_pool ||
      table->exact_values != stock_table->exact_values) {
    log_fatal("%s: the pool table lacks its reference pool or full racks' "
              "values",
              label);
  }
  int total = 0;
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    if (table->reference_counts[ml] != pool_counts[ml]) {
      char letter_string[PLE_LEAVE_STRING_SIZE];
      log_fatal("%s: reference count of %s (letter %d): expected %d, got %d",
                label,
                ple_letter_string(ml, ld, letter_string, sizeof(letter_string)),
                ml, pool_counts[ml], table->reference_counts[ml]);
    }
    total += pool_counts[ml];
  }
  if (table->reference_total != total) {
    log_fatal("%s: reference total: expected %d, got %d", label, total,
              table->reference_total);
  }
  tally->values += MAX_ALPHABET_SIZE + 1;
}

// Two builds of the same pool, value for value.
static void ple_assert_same_pool_leaves(const char *label,
                                        const PoolLeaves *expected,
                                        const PoolLeaves *actual,
                                        const LetterDistribution *ld,
                                        PleTally *tally) {
  char leave_string[PLE_LEAVE_STRING_SIZE];
  const uint32_t number_of_leaves = klv_get_number_of_leaves(expected->klv);
  if (klv_get_number_of_leaves(actual->klv) != number_of_leaves) {
    log_fatal("%s: %u leaves by the walk, %u by pool_leaves_create", label,
              number_of_leaves, klv_get_number_of_leaves(actual->klv));
  }
  for (uint32_t leave_idx = 0; leave_idx < number_of_leaves; leave_idx++) {
    const Equity expected_value = expected->klv->leave_values[leave_idx];
    const Equity actual_value = actual->klv->leave_values[leave_idx];
    if (expected_value != actual_value) {
      log_fatal("%s: leave %s (KLV index %u): the walk gives %d, "
                "pool_leaves_create %d",
                label,
                ple_describe_leave(expected->klv, leave_idx, ld, leave_string,
                                   sizeof(leave_string)),
                leave_idx, expected_value, actual_value);
    }
  }
  if (klv_get_mutation_counter(expected->klv) !=
      klv_get_mutation_counter(actual->klv)) {
    log_fatal("%s: KLV mutation counter: the walk's %" PRIu64
              ", pool_leaves_create's %" PRIu64,
              label, klv_get_mutation_counter(expected->klv),
              klv_get_mutation_counter(actual->klv));
  }
  const PoolShiftTable *expected_table = expected->table;
  const PoolShiftTable *actual_table = actual->table;
  if (expected_table->ld_size != actual_table->ld_size ||
      expected_table->stride != actual_table->stride ||
      expected_table->number_of_rows != actual_table->number_of_rows ||
      expected_table->ld_hash != actual_table->ld_hash) {
    log_fatal("%s: the tables are numbered differently", label);
  }
  for (int size = 0; size < RACK_SIZE; size++) {
    for (int ml = 0; ml < expected_table->ld_size; ml++) {
      if (expected_table->rank_increments[size][ml] !=
          actual_table->rank_increments[size][ml]) {
        log_fatal("%s: rank increment [%d][%d]: the walk's %u, "
                  "pool_leaves_create's %u",
                  label, size, ml, expected_table->rank_increments[size][ml],
                  actual_table->rank_increments[size][ml]);
      }
    }
  }
  if (expected_table->has_reference_pool != actual_table->has_reference_pool ||
      expected_table->reference_total != actual_table->reference_total ||
      expected_table->exact_values != actual_table->exact_values ||
      memcmp(expected_table->reference_counts, actual_table->reference_counts,
             sizeof(expected_table->reference_counts)) != 0) {
    log_fatal("%s: the tables' reference pools differ", label);
  }
  if (expected_table->reference_exact_offset !=
      actual_table->reference_exact_offset) {
    log_fatal("%s: reference_exact_offset: the walk's %d, "
              "pool_leaves_create's %d",
              label, expected_table->reference_exact_offset,
              actual_table->reference_exact_offset);
  }
  const int stride = expected_table->stride;
  for (uint32_t row = 0; row < expected_table->number_of_rows; row++) {
    for (int column = 0; column < stride; column++) {
      const size_t cell = ((size_t)row * (size_t)stride) + (size_t)column;
      if (expected_table->values[cell] != actual_table->values[cell]) {
        char letter_string[PLE_LEAVE_STRING_SIZE];
        log_fatal(
            "%s: table cell (row %u, column %d): leave %s plus %s: the "
            "walk gives %d, pool_leaves_create %d",
            label, row, column,
            ple_describe_leave(NULL, row, ld, leave_string,
                               sizeof(leave_string)),
            ple_letter_string(column, ld, letter_string, sizeof(letter_string)),
            expected_table->values[cell], actual_table->values[cell]);
      }
    }
  }
  tally->values +=
      (uint64_t)number_of_leaves +
      ((uint64_t)expected_table->number_of_rows * (uint64_t)stride) +
      MAX_ALPHABET_SIZE + 2;
}

// A draw: tiles taken from the pool, and the number of ways the pool gives
// them, prod_t C(pool_t, drawn_t).
typedef struct PleDraw {
  BitRack tiles;
  int64_t ways;
  int number_of_letters;
  MachineLetter letters[RACK_SIZE];
  int counts[RACK_SIZE];
} PleDraw;

// Sums over the draws that fill a leave to a full rack, one by one.
typedef struct PleBruteForce {
  const LetterDistribution *ld;
  const RackValueTable *rvt;
  // Every draw of 1 to RACK_SIZE tiles from the pool, by its size.
  PleDraw *draws_by_size[RACK_SIZE + 1];
  int number_of_draws[RACK_SIZE + 1];
  int draw_capacity[RACK_SIZE + 1];
  int total_draws;
  // S and N of the last leave.
  int64_t value_sum;
  int64_t draws;
} PleBruteForce;

static void ple_list_draws(PleBruteForce *brute_force, const int *pool_counts,
                           int ml, const PleDraw *draw, int size) {
  if (ml == ld_get_size(brute_force->ld)) {
    if (size == 0) {
      return;
    }
    if (brute_force->total_draws == PLE_MAX_BRUTE_FORCE_DRAWS) {
      log_fatal("the pool has too many draws for the brute force");
    }
    if (brute_force->number_of_draws[size] ==
        brute_force->draw_capacity[size]) {
      brute_force->draw_capacity[size] =
          (brute_force->draw_capacity[size] * 2) + 16;
      brute_force->draws_by_size[size] = realloc_or_die(
          brute_force->draws_by_size[size],
          sizeof(PleDraw) * (size_t)brute_force->draw_capacity[size]);
    }
    brute_force->draws_by_size[size][brute_force->number_of_draws[size]++] =
        *draw;
    brute_force->total_draws++;
    return;
  }
  ple_list_draws(brute_force, pool_counts, ml + 1, draw, size);
  if (pool_counts[ml] == 0 || size == RACK_SIZE) {
    return;
  }
  PleDraw extended = *draw;
  extended.letters[extended.number_of_letters] = (MachineLetter)ml;
  extended.number_of_letters++;
  for (int drawn = 1; drawn <= pool_counts[ml] && size + drawn <= RACK_SIZE;
       drawn++) {
    bit_rack_add_letter(&extended.tiles, (MachineLetter)ml);
    extended.counts[extended.number_of_letters - 1] = drawn;
    extended.ways = draw->ways * ple_binomial(pool_counts[ml], drawn);
    ple_list_draws(brute_force, pool_counts, ml + 1, &extended, size + drawn);
  }
}

static void ple_brute_force_create(PleBruteForce *brute_force,
                                   const LetterDistribution *ld,
                                   const RackValueTable *rvt,
                                   const int *pool_counts) {
  memset(brute_force, 0, sizeof(PleBruteForce));
  brute_force->ld = ld;
  brute_force->rvt = rvt;
  const PleDraw no_draw = {.tiles = bit_rack_create_empty(), .ways = 1};
  ple_list_draws(brute_force, pool_counts, 0, &no_draw, 0);
}

static void ple_brute_force_destroy(PleBruteForce *brute_force) {
  for (int size = 0; size <= RACK_SIZE; size++) {
    free(brute_force->draws_by_size[size]);
    brute_force->draws_by_size[size] = NULL;
    brute_force->number_of_draws[size] = 0;
  }
}

// Whether the draw fills leave, whose tiles are leave_tiles, to a full rack
// that the distribution deals and the table values: *value is then the
// rack's value.
static bool ple_get_drawn_rack_value(const PleBruteForce *brute_force,
                                     const Rack *leave,
                                     const BitRack *leave_tiles,
                                     const PleDraw *draw, Equity *value) {
  for (int letter_idx = 0; letter_idx < draw->number_of_letters; letter_idx++) {
    const MachineLetter ml = draw->letters[letter_idx];
    if (rack_get_letter(leave, ml) + draw->counts[letter_idx] >
        ld_get_dist(brute_force->ld, ml)) {
      return false;
    }
  }
  BitRack full_rack = *leave_tiles;
  bit_rack_add_bit_rack(&full_rack, &draw->tiles);
  return rack_value_table_lookup(brute_force->rvt, &full_rack, value);
}

// Sets *mean to S(leave) / N(leave), summed in doubles, and returns true,
// or returns false if no draw gives a valued rack. Unlike
// ple_brute_force_leave's, the sums have no limit, and are exact only as far
// as doubles are.
static bool ple_brute_force_leave_mean(const PleBruteForce *brute_force,
                                       const Rack *leave, double *mean) {
  const int tiles_to_draw = RACK_SIZE - rack_get_total_letters(leave);
  const BitRack leave_tiles = bit_rack_create_from_rack(brute_force->ld, leave);
  double value_sum = 0;
  double draws = 0;
  for (int draw_idx = 0; draw_idx < brute_force->number_of_draws[tiles_to_draw];
       draw_idx++) {
    const PleDraw *draw = &brute_force->draws_by_size[tiles_to_draw][draw_idx];
    Equity value = 0;
    if (ple_get_drawn_rack_value(brute_force, leave, &leave_tiles, draw,
                                 &value)) {
      draws += (double)draw->ways;
      value_sum += (double)draw->ways * (double)value;
    }
  }
  if (draws == 0) {
    return false;
  }
  *mean = value_sum / draws;
  return true;
}

// Sets the brute force's value_sum and draws to S(leave) and N(leave).
static void ple_brute_force_leave(PleBruteForce *brute_force, const Rack *leave,
                                  const char *label) {
  const int tiles_to_draw = RACK_SIZE - rack_get_total_letters(leave);
  const BitRack leave_tiles = bit_rack_create_from_rack(brute_force->ld, leave);
  brute_force->value_sum = 0;
  brute_force->draws = 0;
  for (int draw_idx = 0; draw_idx < brute_force->number_of_draws[tiles_to_draw];
       draw_idx++) {
    const PleDraw *draw = &brute_force->draws_by_size[tiles_to_draw][draw_idx];
    Equity value = 0;
    if (ple_get_drawn_rack_value(brute_force, leave, &leave_tiles, draw,
                                 &value)) {
      brute_force->draws += draw->ways;
      brute_force->value_sum += draw->ways * (int64_t)value;
    }
  }
  if (brute_force->draws >= PLE_EXACT_DOUBLE_LIMIT ||
      brute_force->value_sum >= PLE_EXACT_DOUBLE_LIMIT ||
      brute_force->value_sum <= -PLE_EXACT_DOUBLE_LIMIT) {
    log_fatal("%s: a leave's sums reach 2^53, where the builders' doubles "
              "need not be exact: the case is too large for the brute force",
              label);
  }
}

// A leave's value from its sums, in the builders' arithmetic.
static Equity ple_expected_leave_value(int64_t value_sum, int64_t draws,
                                       double empty_value, Equity stock_value) {
  if (draws == 0) {
    return stock_value;
  }
  return (Equity)llround(((double)value_sum / (double)draws) - empty_value);
}

typedef struct PleBruteForceCheck {
  const char *label;
  PleBruteForce brute_force;
  const KLV *stock;
  const KLV *actual;
  // See ple_is_left_out.
  const int *left_out_counts;
  double empty_value;
  bool *index_seen;
  uint64_t values_compared;
} PleBruteForceCheck;

static void ple_check_leave_by_brute_force(void *context, const Rack *leave) {
  PleBruteForceCheck *check = (PleBruteForceCheck *)context;
  if (rack_is_empty(leave)) {
    return;
  }
  const uint32_t klv_index = klv_get_word_index(check->stock, leave);
  if (klv_index == KLV_UNFOUND_INDEX) {
    return;
  }
  char leave_string[PLE_LEAVE_STRING_SIZE];
  if (check->index_seen[klv_index]) {
    log_fatal("%s: leave %s shares KLV index %u", check->label,
              ple_leave_string(leave, check->brute_force.ld, leave_string,
                               sizeof(leave_string)),
              klv_index);
  }
  check->index_seen[klv_index] = true;
  ple_brute_force_leave(&check->brute_force, leave, check->label);
  // A leave left out keeps its stock value.
  const Equity expected =
      ple_is_left_out(leave, rack_get_dist_size(leave), check->left_out_counts)
          ? check->stock->leave_values[klv_index]
          : ple_expected_leave_value(
                check->brute_force.value_sum, check->brute_force.draws,
                check->empty_value, check->stock->leave_values[klv_index]);
  const Equity actual = check->actual->leave_values[klv_index];
  if (actual != expected) {
    log_fatal(
        "%s: leave %s (KLV index %u): the brute force gives %d (S %" PRId64
        ", N %" PRId64 ", empty leave %.17g, stock %d), "
        "pool_leaves_create %d",
        check->label,
        ple_leave_string(leave, check->brute_force.ld, leave_string,
                         sizeof(leave_string)),
        klv_index, expected, check->brute_force.value_sum,
        check->brute_force.draws, check->empty_value,
        check->stock->leave_values[klv_index], actual);
  }
  check->values_compared++;
}

// Every value of the pool's KLV, and its table's offset, against the brute
// force.
static void
ple_assert_brute_force(const char *label, const PoolLeaves *pool_leaves,
                       const PoolShiftTable *stock_table,
                       const LetterDistribution *ld, const int *pool_counts,
                       const int *left_out_counts, PleTally *tally) {
  const int ld_size = ld_get_size(ld);
  PleBruteForceCheck check = {.label = label,
                              .stock = stock_table->klv,
                              .actual = pool_leaves->klv,
                              .left_out_counts = left_out_counts};
  ple_brute_force_create(&check.brute_force, ld, stock_table->exact_values,
                         pool_counts);
  Rack empty_leave;
  rack_set_dist_size_and_reset(&empty_leave, ld_size);
  ple_brute_force_leave(&check.brute_force, &empty_leave, label);
  check.empty_value = check.brute_force.draws > 0
                          ? (double)check.brute_force.value_sum /
                                (double)check.brute_force.draws
                          : 0.0;
  const Equity expected_offset = (Equity)llround(check.empty_value);
  if (pool_leaves->table->reference_exact_offset != expected_offset) {
    log_fatal("%s: reference_exact_offset: the brute force gives %d (S %" PRId64
              ", N %" PRId64 "), pool_leaves_create %d",
              label, expected_offset, check.brute_force.value_sum,
              check.brute_force.draws,
              pool_leaves->table->reference_exact_offset);
  }
  const uint32_t number_of_leaves = klv_get_number_of_leaves(check.stock);
  check.index_seen = calloc_or_die(number_of_leaves, sizeof(bool));
  int dist_counts[MAX_ALPHABET_SIZE];
  ple_get_dist_counts(ld, dist_counts);
  ple_visit_multisets(ld_size, PLE_MAX_LEAVE_SIZE, dist_counts,
                      ple_check_leave_by_brute_force, &check);
  // The leaves the distribution cannot deal keep their stock values.
  for (uint32_t leave_idx = 0; leave_idx < number_of_leaves; leave_idx++) {
    if (check.index_seen[leave_idx]) {
      continue;
    }
    if (pool_leaves->klv->leave_values[leave_idx] !=
        check.stock->leave_values[leave_idx]) {
      char leave_string[PLE_LEAVE_STRING_SIZE];
      log_fatal("%s: leave %s (KLV index %u), which the distribution cannot "
                "deal: expected its stock value %d, got %d",
                label,
                ple_describe_leave(check.stock, leave_idx, ld, leave_string,
                                   sizeof(leave_string)),
                leave_idx, check.stock->leave_values[leave_idx],
                pool_leaves->klv->leave_values[leave_idx]);
    }
    check.values_compared++;
  }
  free(check.index_seen);
  ple_brute_force_destroy(&check.brute_force);
  tally->values += check.values_compared + 1;
}

// The build on several threads gives the leaves of the build on one, which
// are expected.
static void
ple_assert_threaded_builds(const char *label, const PoolShiftTable *stock_table,
                           const LetterDistribution *ld, const Bag *bag,
                           const Rack *opponent_rack, const Rack *mover_rack,
                           const int *pool_counts, const PoolLeaves *expected,
                           PleTally *tally) {
  for (int number_of_threads = 2; number_of_threads <= PLE_MAX_BUILD_THREADS;
       number_of_threads += PLE_BUILD_THREADS_STEP) {
    PoolLeaves *actual = pool_leaves_create(stock_table, ld, bag, opponent_rack,
                                            mover_rack, number_of_threads);
    ple_assert_pool_leaves_bound(label, actual, stock_table, ld, pool_counts,
                                 tally);
    ple_assert_same_pool_leaves(label, expected, actual, ld, tally);
    pool_leaves_destroy(actual);
  }
}

// Builds the leaves of a pool with pool_leaves_create and checks every value
// they hold: against the walk when compare_with_walk, against the brute
// force when brute_force, and the table against its KLV always.
static PleBuildTimes
ple_check_pool(const char *label, const PoolShiftTable *stock_table,
               const LetterDistribution *ld, const Bag *bag,
               const Rack *opponent_rack, const Rack *mover_rack,
               bool compare_with_walk, bool brute_force, PleTally *tally) {
  int pool_counts[MAX_ALPHABET_SIZE] = {0};
  bag_increment_unseen_count(bag, pool_counts);
  rack_increment_unseen_count(opponent_rack, pool_counts);
  if (mover_rack != NULL) {
    rack_increment_unseen_count(mover_rack, pool_counts);
  }
  // A pool with the mover's rack leaves out the letters it has none of.
  const int *left_out_counts = mover_rack != NULL ? pool_counts : NULL;
  PleBuildTimes times = {0};
  Timer timer;
  ctimer_start(&timer);
  PoolLeaves *actual =
      pool_leaves_create(stock_table, ld, bag, opponent_rack, mover_rack, 1);
  times.create_seconds = ctimer_elapsed_seconds(&timer);
  ple_assert_pool_leaves_bound(label, actual, stock_table, ld, pool_counts,
                               tally);
  ple_assert_threaded_builds(label, stock_table, ld, bag, opponent_rack,
                             mover_rack, pool_counts, actual, tally);
  ctimer_start(&timer);
  ple_assert_table_matches_klv(label, actual->table, actual->klv, ld,
                               left_out_counts, tally);
  times.table_check_seconds = ctimer_elapsed_seconds(&timer);
  if (compare_with_walk) {
    ctimer_start(&timer);
    PoolLeaves *reference = pool_leaves_create_by_walk(
        stock_table, ld, bag, opponent_rack, mover_rack);
    times.walk_seconds = ctimer_elapsed_seconds(&timer);
    ple_assert_pool_leaves_bound(label, reference, stock_table, ld, pool_counts,
                                 tally);
    ple_assert_same_pool_leaves(label, reference, actual, ld, tally);
    pool_leaves_destroy(reference);
  }
  if (brute_force) {
    ctimer_start(&timer);
    ple_assert_brute_force(label, actual, stock_table, ld, pool_counts,
                           left_out_counts, tally);
    times.brute_force_seconds = ctimer_elapsed_seconds(&timer);
  }
  pool_leaves_destroy(actual);
  tally->builds++;
  return times;
}

static LetterDistribution *ple_create_ld(const char *name,
                                         const char *contents) {
  ErrorStack *error_stack = error_stack_create();
  StringSplitter *lines = split_string_by_newline(contents, true);
  LetterDistribution *ld = calloc_or_die(1, sizeof(LetterDistribution));
  ld_create_internal(name, lines, ld, error_stack);
  string_splitter_destroy(lines);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not make the letter distribution of case %s", name);
  }
  error_stack_destroy(error_stack);
  return ld;
}

// Every leave a random value from -PLE_LEAVE_VALUE_RANGE to
// PLE_LEAVE_VALUE_RANGE but 0, so that no table cell is 0 by chance.
static void ple_set_random_leave_values(KLV *klv, XoshiroPRNG *prng) {
  for (uint32_t leave_idx = 0; leave_idx < klv_get_number_of_leaves(klv);
       leave_idx++) {
    Equity value = (Equity)prng_get_random_number(
                       prng, 2 * (uint64_t)PLE_LEAVE_VALUE_RANGE) -
                   PLE_LEAVE_VALUE_RANGE;
    if (value >= 0) {
      value++;
    }
    klv->leave_values[leave_idx] = value;
  }
  klv_mark_leave_values_mutated(klv);
}

typedef struct PleRackValuesFill {
  RackValueTable *rvt;
  const LetterDistribution *ld;
  PleRackValues rack_values;
  MachineLetter unvalued_letter;
  XoshiroPRNG *prng;
  bool *rack_seen;
  uint32_t racks_seen;
} PleRackValuesFill;

static int32_t ple_random_rack_value(const PleRackValuesFill *fill,
                                     const Rack *rack) {
  switch (fill->rack_values) {
  case PLE_RACK_VALUES_NONE:
    return RACK_VALUE_TABLE_UNVALUED;
  case PLE_RACK_VALUES_EXTREME:
    return prng_get_random_number(fill->prng, PLE_EXTREME_MIN_ONE_IN) == 0
               ? RACK_VALUE_TABLE_MIN_VALUE
               : RACK_VALUE_TABLE_MAX_VALUE;
  case PLE_RACK_VALUES_TIES:
    return (int32_t)prng_get_random_number(fill->prng,
                                           (2 * PLE_TIE_VALUE_RANGE) + 1) -
           PLE_TIE_VALUE_RANGE;
  case PLE_RACK_VALUES_WITHOUT_LETTER:
    if (rack_get_letter(rack, fill->unvalued_letter) > 0) {
      return RACK_VALUE_TABLE_UNVALUED;
    }
    break;
  case PLE_RACK_VALUES_RANDOM:
    break;
  }
  if (prng_get_random_number(fill->prng, PLE_UNVALUED_ONE_IN) == 0) {
    return RACK_VALUE_TABLE_UNVALUED;
  }
  if (prng_get_random_number(fill->prng, PLE_EXTREME_ONE_IN) == 0) {
    return prng_get_random_number(fill->prng, 2) == 0
               ? RACK_VALUE_TABLE_MAX_VALUE
               : RACK_VALUE_TABLE_MIN_VALUE;
  }
  return (int32_t)prng_get_random_number(fill->prng,
                                         (2 * PLE_RACK_VALUE_RANGE) + 1) -
         PLE_RACK_VALUE_RANGE;
}

static void ple_fill_rack_value(void *context, const Rack *rack) {
  PleRackValuesFill *fill = (PleRackValuesFill *)context;
  if (rack_get_total_letters(rack) != RACK_SIZE) {
    return;
  }
  const BitRack bit_rack = bit_rack_create_from_rack(fill->ld, rack);
  uint32_t rack_index = 0;
  if (!rack_value_table_get_rack_index(fill->rvt, &bit_rack, &rack_index) ||
      fill->rack_seen[rack_index]) {
    log_fatal("a full rack has no index of its own in the rack value table");
  }
  fill->rack_seen[rack_index] = true;
  fill->racks_seen++;
  rack_value_table_write_stored(fill->rvt, rack_index,
                                ple_random_rack_value(fill, rack));
}

// The full racks' values of the case, for its KLV.
static RackValueTable *ple_create_rack_values(const LetterDistribution *ld,
                                              const KLV *klv,
                                              const PleSyntheticCase *spec,
                                              XoshiroPRNG *prng) {
  uint8_t letter_counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    letter_counts[ml] = (uint8_t)ld_get_dist(ld, (MachineLetter)ml);
  }
  RackValueTable *rvt = rack_value_table_create_unvalued(
      letter_counts, ld_get_size(ld), klv_get_hash(klv));
  assert(rvt != NULL);
  const uint32_t number_of_racks = rack_value_table_get_number_of_racks(rvt);
  PleRackValuesFill fill = {
      .rvt = rvt,
      .ld = ld,
      .rack_values = spec->rack_values,
      .unvalued_letter = spec->unvalued_letter,
      .prng = prng,
      .rack_seen = calloc_or_die(number_of_racks > 0 ? number_of_racks : 1,
                                 sizeof(bool)),
  };
  int dist_counts[MAX_ALPHABET_SIZE];
  ple_get_dist_counts(ld, dist_counts);
  ple_visit_multisets(ld_get_size(ld), RACK_SIZE, dist_counts,
                      ple_fill_rack_value, &fill);
  if (fill.racks_seen != number_of_racks) {
    log_fatal("%s: %u full racks dealt, the table has %u", spec->name,
              fill.racks_seen, number_of_racks);
  }
  free(fill.rack_seen);
  return rvt;
}

static void ple_set_rack(const LetterDistribution *ld, const char *tiles,
                         Rack *rack) {
  if (rack_set_to_string(ld, rack, tiles) < 0) {
    log_fatal("bad tiles in a test pool: %s", tiles);
  }
}

// Sets the racks of a pool, and the tiles of its bag.
static void ple_set_pool_racks(const LetterDistribution *ld,
                               const PlePool *pool, Rack *bag_tiles,
                               Rack *opponent_rack, Rack *mover_rack) {
  const int ld_size = ld_get_size(ld);
  ple_set_rack(ld, pool->opponent_rack, opponent_rack);
  ple_set_rack(ld, pool->mover_rack, mover_rack);
  if (pool->bag != NULL) {
    ple_set_rack(ld, pool->bag, bag_tiles);
  } else {
    Rack absent;
    ple_set_rack(ld, pool->absent_letters != NULL ? pool->absent_letters : "",
                 &absent);
    rack_set_dist_size_and_reset(bag_tiles, ld_size);
    for (int ml = 0; ml < ld_size; ml++) {
      const int left = ld_get_dist(ld, (MachineLetter)ml) -
                       rack_get_letter(opponent_rack, (MachineLetter)ml) -
                       rack_get_letter(mover_rack, (MachineLetter)ml);
      if (rack_get_letter(&absent, (MachineLetter)ml) == 0 && left > 0) {
        rack_add_letters(bag_tiles, (MachineLetter)ml, left);
      }
    }
  }
  for (int ml = 0; ml < ld_size; ml++) {
    if (rack_get_letter(bag_tiles, (MachineLetter)ml) +
            rack_get_letter(opponent_rack, (MachineLetter)ml) +
            rack_get_letter(mover_rack, (MachineLetter)ml) >
        ld_get_dist(ld, (MachineLetter)ml)) {
      log_fatal("test pool %s holds more of letter %d than the distribution",
                pool->name, ml);
    }
  }
}

static void ple_set_bag(Bag *bag, const Rack *bag_tiles) {
  MachineLetter tiles[MAX_BAG_SIZE] = {0};
  int number_of_tiles = 0;
  for (int ml = 0; ml < rack_get_dist_size(bag_tiles); ml++) {
    for (int copy_idx = 0;
         copy_idx < rack_get_letter(bag_tiles, (MachineLetter)ml); copy_idx++) {
      tiles[number_of_tiles++] = (MachineLetter)ml;
    }
  }
  bag_set_to_tiles(bag, tiles, number_of_tiles);
}

// Checks the pool, public and private.
// The exact shifts pool_shift_fill_rack gives every leave of rack, which it
// computes for all of them at once, against each leave's own exact shift,
// which pool_shift_get_leave_shift computes alone.
static void ple_check_rack_shifts(const char *label,
                                  const PoolShiftTable *table,
                                  const LetterDistribution *ld, const Bag *bag,
                                  const Rack *opponent_rack, const Rack *rack,
                                  PleTally *tally) {
  PoolShiftRack result;
  pool_shift_fill_rack(table, ld, bag, opponent_rack,
                       POOL_SHIFT_EXACT_WEIGHT_MILLI, rack, &result);
  Rack leave;
  for (int subset_idx = 0; subset_idx < result.count; subset_idx++) {
    rack_set_dist_size_and_reset(&leave, ld_get_size(ld));
    for (int bit_idx = 0; bit_idx < RACK_SIZE; bit_idx++) {
      if ((result.map_indices[subset_idx] & (1 << bit_idx)) != 0) {
        rack_add_letter(&leave, result.bit_tiles[bit_idx]);
      }
    }
    // A full rack and a bag of a rack, so that the shift applies.
    const Equity expected = pool_shift_get_leave_shift(
        table, table->klv, POOL_SHIFT_EXACT_WEIGHT_MILLI, ld, bag,
        opponent_rack, &leave, RACK_SIZE, RACK_SIZE);
    if (result.shifts[subset_idx] != expected) {
      char leave_string[PLE_LEAVE_STRING_SIZE];
      char rack_string[PLE_LEAVE_STRING_SIZE];
      log_fatal(
          "%s: the exact shift of leave %s of rack %s is %d, but the "
          "leave alone gives %d",
          label,
          ple_leave_string(&leave, ld, leave_string, sizeof(leave_string)),
          ple_leave_string(rack, ld, rack_string, sizeof(rack_string)),
          (int)result.shifts[subset_idx], (int)expected);
    }
  }
  tally->values += (uint64_t)result.count;
}

// Sets leave to the subset of the rack's tiles that the result numbers
// subset_idx.
static void ple_set_rack_subset(const PoolShiftRack *result, int subset_idx,
                                int ld_size, Rack *leave) {
  rack_set_dist_size_and_reset(leave, ld_size);
  for (int bit_idx = 0; bit_idx < RACK_SIZE; bit_idx++) {
    if ((result->map_indices[subset_idx] & (1 << bit_idx)) != 0) {
      rack_add_letter(leave, result->bit_tiles[bit_idx]);
    }
  }
}

// The exact shifts pool_shift_fill_rack gives every leave of rack once the
// table takes no pool's sums to fit int64, so that every leave's values are
// split into digits (see exact_sums_max_unseen): they must be the shifts
// the table gives as it is.
static void ple_check_wide_rack_shifts(const char *label, PoolShiftTable *table,
                                       const LetterDistribution *ld,
                                       const Bag *bag,
                                       const Rack *opponent_rack,
                                       const Rack *rack, PleTally *tally) {
  PoolShiftRack expected;
  pool_shift_fill_rack(table, ld, bag, opponent_rack,
                       POOL_SHIFT_EXACT_WEIGHT_MILLI, rack, &expected);
  const int max_unseen = table->exact_sums_max_unseen;
  table->exact_sums_max_unseen = 0;
  PoolShiftRack actual;
  pool_shift_fill_rack(table, ld, bag, opponent_rack,
                       POOL_SHIFT_EXACT_WEIGHT_MILLI, rack, &actual);
  table->exact_sums_max_unseen = max_unseen;
  if (actual.count != expected.count) {
    log_fatal("%s: %d leaves with split sums, %d without", label, actual.count,
              expected.count);
  }
  Rack leave;
  for (int subset_idx = 0; subset_idx < expected.count; subset_idx++) {
    if (actual.shifts[subset_idx] != expected.shifts[subset_idx]) {
      char leave_string[PLE_LEAVE_STRING_SIZE];
      ple_set_rack_subset(&expected, subset_idx, ld_get_size(ld), &leave);
      log_fatal(
          "%s: the exact shift of leave %s is %d with split sums, %d without",
          label,
          ple_leave_string(&leave, ld, leave_string, sizeof(leave_string)),
          (int)actual.shifts[subset_idx], (int)expected.shifts[subset_idx]);
    }
  }
  tally->values += (uint64_t)expected.count;
}

// The exact value pool_shift_fill_rack gives every leave of rack against
// the mean of the leave's draws, which see unseen_counts, summed here in
// doubles one draw at a time. That holds for sums of any size, to within the
// rounding to a millipoint.
static void
ple_check_rack_shifts_by_draws(const char *label, const PoolShiftTable *table,
                               const LetterDistribution *ld, const Bag *bag,
                               const Rack *opponent_rack, const Rack *rack,
                               const int *unseen_counts, PleTally *tally) {
  PleBruteForce brute_force;
  ple_brute_force_create(&brute_force, ld, table->exact_values, unseen_counts);
  PoolShiftRack result;
  pool_shift_fill_rack(table, ld, bag, opponent_rack,
                       POOL_SHIFT_EXACT_WEIGHT_MILLI, rack, &result);
  Rack leave;
  for (int subset_idx = 0; subset_idx < result.count; subset_idx++) {
    ple_set_rack_subset(&result, subset_idx, ld_get_size(ld), &leave);
    if (rack_get_total_letters(&leave) >= RACK_SIZE) {
      continue;
    }
    // Without a valued draw, a leave has no exact value and is not shifted.
    double mean = 0;
    const bool has_mean =
        ple_brute_force_leave_mean(&brute_force, &leave, &mean);
    const double actual = (double)result.shifts[subset_idx] +
                          (double)klv_get_leave_value(table->klv, &leave);
    if (has_mean ? fabs(actual - mean) > PLE_MEAN_TOLERANCE
                 : result.shifts[subset_idx] != 0) {
      char leave_string[PLE_LEAVE_STRING_SIZE];
      log_fatal(
          "%s: leave %s is shifted by %d to %.0f, and the mean of its "
          "draws is %s%.3f",
          label,
          ple_leave_string(&leave, ld, leave_string, sizeof(leave_string)),
          (int)result.shifts[subset_idx], actual,
          has_mean ? "" : "of no valued rack: ", mean);
    }
    tally->values++;
  }
  ple_brute_force_destroy(&brute_force);
}

// The exact shifts of the mover's rack in a pool of a synthetic case, whose
// table the checks may change: whether the pool's sums are as wide as the
// case says, the shifts with every leave's sums split, and the shifts
// against each leave's draws.
static void ple_check_pool_rack_shifts(const PleSyntheticCase *spec,
                                       PoolShiftTable *table,
                                       const LetterDistribution *ld,
                                       const PlePool *pool, Bag *bag,
                                       PleTally *tally) {
  Rack bag_tiles;
  Rack opponent_rack;
  Rack mover_rack;
  ple_set_pool_racks(ld, pool, &bag_tiles, &opponent_rack, &mover_rack);
  ple_set_bag(bag, &bag_tiles);
  int unseen_counts[MAX_ALPHABET_SIZE] = {0};
  int unseen_total = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    unseen_counts[ml] = rack_get_letter(&bag_tiles, (MachineLetter)ml) +
                        rack_get_letter(&opponent_rack, (MachineLetter)ml);
    unseen_total += unseen_counts[ml];
  }
  char *label = get_formatted_string("%s, pool \"%s\", rack shifts", spec->name,
                                     pool->name);
  if ((unseen_total > table->exact_sums_max_unseen) !=
      spec->root_sums_are_wide) {
    log_fatal("%s: %d unseen tiles, and the table's sums fit int64 up to %d: "
              "the case expects %s",
              label, unseen_total, table->exact_sums_max_unseen,
              spec->root_sums_are_wide ? "more than fit" : "no more than fit");
  }
  ple_check_wide_rack_shifts(label, table, ld, bag, &opponent_rack, &mover_rack,
                             tally);
  if (pool->brute_force) {
    ple_check_rack_shifts_by_draws(label, table, ld, bag, &opponent_rack,
                                   &mover_rack, unseen_counts, tally);
  }
  free(label);
}

static void ple_check_constructed_pool(const char *case_name,
                                       const PoolShiftTable *stock_table,
                                       const LetterDistribution *ld,
                                       const PlePool *pool, Bag *bag,
                                       bool compare_with_walk,
                                       bool sums_are_exact, bool verbose,
                                       PleTally *tally) {
  Rack bag_tiles;
  Rack opponent_rack;
  Rack mover_rack;
  ple_set_pool_racks(ld, pool, &bag_tiles, &opponent_rack, &mover_rack);
  ple_set_bag(bag, &bag_tiles);
  for (int view_idx = 0; view_idx < 2; view_idx++) {
    const bool is_public = view_idx == 0;
    char *label =
        get_formatted_string("%s, pool \"%s\", %s", case_name, pool->name,
                             is_public ? "public" : "private");
    const PleBuildTimes times =
        ple_check_pool(label, stock_table, ld, bag, &opponent_rack,
                       is_public ? &mover_rack : NULL, compare_with_walk,
                       sums_are_exact && pool->brute_force, tally);
    if (verbose) {
      printf("poolleavesequiv: %s: %d tiles, create %.3f s, walk %.3f s, "
             "table check %.3f s, brute force %.3f s: equal\n",
             label,
             bag_get_letters(bag) + rack_get_total_letters(&opponent_rack) +
                 (is_public ? rack_get_total_letters(&mover_rack) : 0),
             times.create_seconds, times.walk_seconds,
             times.table_check_seconds, times.brute_force_seconds);
      (void)fflush(stdout);
    }
    free(label);
  }
  char *rack_label = get_formatted_string("%s, pool \"%s\", rack shifts",
                                          case_name, pool->name);
  ple_check_rack_shifts(rack_label, stock_table, ld, bag, &opponent_rack,
                        &mover_rack, tally);
  free(rack_label);
}

static void ple_run_synthetic_case(const PleSyntheticCase *spec,
                                   PleTally *tally) {
  LetterDistribution *ld = ple_create_ld(spec->name, spec->distribution);
  LetterDistribution *klv_ld =
      spec->klv_distribution != NULL
          ? ple_create_ld(spec->name, spec->klv_distribution)
          : NULL;
  KLV *klv = klv_create_empty(klv_ld != NULL ? klv_ld : ld, spec->name);
  XoshiroPRNG *prng = prng_create(spec->seed);
  ple_set_random_leave_values(klv, prng);
  PoolShiftTable *table = pool_shift_table_create(klv, ld);
  char *stock_label = get_formatted_string("%s, stock table", spec->name);
  ple_assert_stock_table(stock_label, table, klv, ld, NULL, tally);
  free(stock_label);
  RackValueTable *rvt = ple_create_rack_values(ld, klv, spec, prng);
  if (!pool_shift_table_set_exact_values(table, rvt, ld)) {
    log_fatal("%s: the table refuses the case's full racks' values",
              spec->name);
  }
  Bag *bag = bag_create(ld, spec->seed);
  for (int pool_idx = 0; pool_idx < spec->number_of_pools; pool_idx++) {
    ple_check_constructed_pool(spec->name, table, ld, &spec->pools[pool_idx],
                               bag, true, spec->sums_are_exact, false, tally);
    ple_check_pool_rack_shifts(spec, table, ld, &spec->pools[pool_idx], bag,
                               tally);
  }
  bag_destroy(bag);
  pool_shift_table_destroy(table);
  rack_value_table_destroy(rvt);
  prng_destroy(prng);
  klv_destroy(klv);
  ld_destroy(klv_ld);
  ld_destroy(ld);
}

static bool ple_have_csw24_rack_values(void) {
  // NOLINTNEXTLINE(misc-redundant-expression)
  const bool is_standard_board = BOARD_DIM == DEFAULT_BOARD_DIM;
  return is_standard_board && RACK_SIZE == PLE_DATA_RACK_SIZE &&
         cfile_access("./data/lexica/CSW24.rvt", CFILE_ACCESS_READ);
}

static const PoolShiftTable *ple_get_exact_table(const Config *config) {
  const PoolShiftTable *table =
      players_data_get_pool_shift_table(config_get_players_data(config), 0);
  if (table == NULL || table->exact_values == NULL) {
    log_fatal("poolleavesequiv: CSW24's table has no full racks' values");
  }
  return table;
}

// The stock CSW24 table cell by cell and, given the full racks' values, one
// sparse pool against the brute force.
static void ple_test_csw24(PleTally *tally) {
  const bool have_rack_values = ple_have_csw24_rack_values();
  Config *config = config_create_or_die(
      have_rack_values ? "set -lex CSW24 -wmp false -s1 equity -s2 equity "
                         "-threads 1 -poolshift exact"
                       : "set -lex CSW24 -wmp false -threads 1");
  const LetterDistribution *ld = config_get_ld(config);
  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  Timer timer;
  ctimer_start(&timer);
  if (have_rack_values) {
    const PoolShiftTable *table = ple_get_exact_table(config);
    ple_assert_stock_table("CSW24, stock table", table, klv, ld,
                           table->exact_values, tally);
    printf("poolleavesequiv: CSW24 stock table equal, %.3f s\n",
           ctimer_elapsed_seconds(&timer));
    ctimer_start(&timer);
    const PlePool *pool = &ple_csw24_pools[0];
    Rack bag_tiles;
    Rack opponent_rack;
    Rack mover_rack;
    ple_set_pool_racks(ld, pool, &bag_tiles, &opponent_rack, &mover_rack);
    Bag *bag = bag_create(ld, 0);
    ple_set_bag(bag, &bag_tiles);
    char *label =
        get_formatted_string("CSW24, pool \"%s\", private", pool->name);
    const PleBuildTimes times = ple_check_pool(
        label, table, ld, bag, &opponent_rack, NULL, false, true, tally);
    printf("poolleavesequiv: %s equal to the brute force, %.3f s (create "
           "%.3f s, table check %.3f s, brute force %.3f s)\n",
           label, ctimer_elapsed_seconds(&timer), times.create_seconds,
           times.table_check_seconds, times.brute_force_seconds);
    free(label);
    bag_destroy(bag);
  } else {
    PoolShiftTable *table = pool_shift_table_create(klv, ld);
    ple_assert_stock_table("CSW24, stock table", table, klv, ld, NULL, tally);
    pool_shift_table_destroy(table);
    printf("poolleavesequiv: CSW24 stock table equal, %.3f s\n",
           ctimer_elapsed_seconds(&timer));
    printf("poolleavesequiv: skipped the CSW24 pools: no "
           "data/lexica/CSW24.rvt for this build\n");
  }
  (void)fflush(stdout);
  config_destroy(config);
}

void test_pool_leaves_equivalence(void) {
  Timer timer;
  ctimer_start(&timer);
  PleTally tally = {0};
  const int number_of_cases =
      (int)(sizeof(ple_synthetic_cases) / sizeof(ple_synthetic_cases[0]));
  for (int case_idx = 0; case_idx < number_of_cases; case_idx++) {
    ple_run_synthetic_case(&ple_synthetic_cases[case_idx], &tally);
  }
  printf("poolleavesequiv: %d synthetic cases, %d builds, %" PRIu64
         " values equal, %.3f s\n",
         number_of_cases, tally.builds, tally.values,
         ctimer_elapsed_seconds(&timer));
  (void)fflush(stdout);
  ple_test_csw24(&tally);
}

// Checks the position's pool, public and private, against the walk.
static void ple_check_position(const Game *game, const char *position_name,
                               PleTally *tally) {
  const int mover_idx = game_get_player_on_turn_index(game);
  const Player *mover = game_get_player(game, mover_idx);
  const Rack *mover_rack = player_get_rack(mover);
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, 1 - mover_idx));
  const PoolShiftTable *table = player_get_pool_shift_table(mover);
  if (table == NULL || table->exact_values == NULL) {
    log_fatal("poolleavesequiv: the mover's table has no full racks' values");
  }
  for (int view_idx = 0; view_idx < 2; view_idx++) {
    const bool is_public = view_idx == 0;
    char *label = get_formatted_string("%s, %s", position_name,
                                       is_public ? "public" : "private");
    const PleBuildTimes times = ple_check_pool(
        label, table, game_get_ld(game), game_get_bag(game), opponent_rack,
        is_public ? mover_rack : NULL, true, false, tally);
    printf("poolleavesequiv: %s: bag %d, create %.3f s, walk %.3f s, table "
           "check %.3f s: equal\n",
           label, game_get_real_bag_size(game), times.create_seconds,
           times.walk_seconds, times.table_check_seconds);
    (void)fflush(stdout);
    free(label);
  }
  char *rack_label = get_formatted_string("%s, rack shifts", position_name);
  ple_check_rack_shifts(rack_label, table, game_get_ld(game),
                        game_get_bag(game), opponent_rack, mover_rack, tally);
  free(rack_label);
}

static bool ple_is_eligible(const Game *game) {
  const Player *mover =
      game_get_player(game, game_get_player_on_turn_index(game));
  return rack_get_total_letters(player_get_rack(mover)) == RACK_SIZE &&
         game_get_real_bag_size(game) >= RACK_SIZE;
}

void pool_leaves_equivalence_run_spec(const char *spec) {
  int games = 0;
  int first_game = 0;
  // The counts are test parameters, and a malformed spec fails the field
  // count or the range check below.
  // NOLINTNEXTLINE(cert-err34-c)
  const int fields = sscanf(spec, "games=%d,first=%d", &games, &first_game);
  if ((fields != PLE_SPEC_GAMES_ONLY_FIELDS && fields != PLE_SPEC_ALL_FIELDS) ||
      games < 0 || first_game < 0) {
    log_fatal("poolleavesequiv: bad spec %s (games=<n>[,first=<g>])", spec);
  }
  if (!ple_have_csw24_rack_values()) {
    printf("poolleavesequiv: skipped: no data/lexica/CSW24.rvt for this "
           "build\n");
    return;
  }
  Timer timer;
  ctimer_start(&timer);
  PleTally tally = {0};
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -savesettings false -poolshift 1.2");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const PoolShiftTable *table = ple_get_exact_table(config);
  Bag *bag = bag_create(ld, 0);
  const int number_of_pools =
      (int)(sizeof(ple_csw24_pools) / sizeof(ple_csw24_pools[0]));
  for (int pool_idx = 0; pool_idx < number_of_pools; pool_idx++) {
    ple_check_constructed_pool("CSW24", table, ld, &ple_csw24_pools[pool_idx],
                               bag, true, true, true, &tally);
  }
  bag_destroy(bag);
  game_reset(game);
  game_seed(game, PLE_MID_GAME_SEED);
  draw_starting_racks(game);
  for (int move_idx = 0; move_idx < PLE_MID_GAME_MOVES; move_idx++) {
    play_top_n_equity_move(game, 0);
  }
  if (!ple_is_eligible(game)) {
    log_fatal("poolleavesequiv: the mid-game position has no full rack or "
              "too few tiles in the bag");
  }
  ple_check_position(game, "CSW24, mid-game", &tally);
  int positions = 0;
  for (int game_idx = first_game; game_idx < first_game + games; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)PLE_FIRST_GAME_SEED + (uint64_t)game_idx);
    draw_starting_racks(game);
    int move_idx = 0;
    while (!game_over(game)) {
      if (ple_is_eligible(game)) {
        char *position_name =
            get_formatted_string("game %d, move %d", game_idx, move_idx);
        ple_check_position(game, position_name, &tally);
        free(position_name);
        positions++;
      }
      play_top_n_equity_move(game, 0);
      move_idx++;
    }
  }
  printf("poolleavesequiv: %d constructed pools, a mid-game position and %d "
         "positions of %d self-play games from game %d: %d builds, %" PRIu64
         " values compared, %.1f s\n",
         number_of_pools, positions, games, first_game, tally.builds,
         tally.values, ctimer_elapsed_seconds(&timer));
  printf("poolleavesequiv: verdict: all equal: pool_leaves_create builds as "
         "pool_leaves_create_by_walk does, value for value, and as the brute "
         "force does on the sparse pools, and every table holds its KLV's "
         "values cell by cell\n");
  (void)fflush(stdout);
  game_destroy(game);
  config_destroy(config);
}
