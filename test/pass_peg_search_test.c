#include "pass_peg_search_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// English distribution: A=9, B=2, C=2, D=4, E=12, F=2, G=3, H=2, I=9, J=1,
// K=1, L=4, M=2, N=6, O=8, P=2, Q=1, R=6, S=4, T=6, U=4, V=2, W=2, X=1,
// Y=2, Z=1, blank=2 (total 100).
//
// For a rack to be reservable as "two copies + one Q held back", the
// rack's per-letter count cannot exceed floor(dist/2). Q/J/K/X/Z are
// excluded entirely (we want the Q reserved separately, and J/K/X/Z each
// have only one copy in the distribution).
static const int kMaxRackCount[26] = {
    /*A*/ 4, /*B*/ 1, /*C*/ 1, /*D*/ 2,
    /*E*/ 6, /*F*/ 1, /*G*/ 1, /*H*/ 1,
    /*I*/ 4, /*J*/ 0, /*K*/ 0, /*L*/ 2,
    /*M*/ 1, /*N*/ 3, /*O*/ 4, /*P*/ 1,
    /*Q*/ 0, /*R*/ 3, /*S*/ 2, /*T*/ 3,
    /*U*/ 2, /*V*/ 1, /*W*/ 1, /*X*/ 0,
    /*Y*/ 1, /*Z*/ 0,
};

static int cmp_strs(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void sort_letters(char *s) {
  // 7 chars; bubble sort is fine.
  int n = (int)strlen(s);
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      if (s[j] < s[i]) {
        char t = s[i];
        s[i] = s[j];
        s[j] = t;
      }
    }
  }
}

void test_pass_peg_enumerate_bingo_racks(void) {
  fprintf(stderr, "[passpegracks] starting\n");
  fflush(stderr);
  const char *path = "data/lexica/TWL98.txt";
  FILE *f = fopen(path, "re");
  if (!f) {
    fprintf(stderr, "[passpegracks] cannot open %s\n", path);
    fflush(stderr);
    log_fatal("cannot open %s", path);
  }
  fprintf(stderr, "[passpegracks] opened %s\n", path);
  fflush(stderr);

  // Dynamic array of sorted 7-letter rack signatures.
  int cap = 1024;
  int count = 0;
  char **racks = malloc_or_die((size_t)cap * sizeof(char *));

  char line[64];
  int total_7letter = 0;
  int kept_after_letter_filter = 0;
  while (fgets(line, sizeof(line), f)) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len != 7) {
      continue;
    }
    total_7letter++;

    int counts[26] = {0};
    bool ok = true;
    for (int i = 0; i < 7; i++) {
      char c = line[i];
      if (c < 'A' || c > 'Z') {
        ok = false;
        break;
      }
      counts[c - 'A']++;
    }
    if (!ok) {
      continue;
    }
    for (int i = 0; i < 26 && ok; i++) {
      if (counts[i] > kMaxRackCount[i]) {
        ok = false;
      }
    }
    if (!ok) {
      continue;
    }
    kept_after_letter_filter++;

    char *sig = malloc_or_die(8);
    memcpy(sig, line, 7);
    sig[7] = '\0';
    sort_letters(sig);

    if (count == cap) {
      cap *= 2;
      char **new_racks = realloc(racks, (size_t)cap * sizeof(char *));
      if (!new_racks) {
        log_fatal("realloc failed");
      }
      racks = new_racks;
    }
    racks[count++] = sig;
    if (count % 1000 == 0) {
      fprintf(stderr, "[passpegracks] kept %d racks so far\n", count);
      fflush(stderr);
    }
  }
  (void)fclose(f);
  fprintf(stderr, "[passpegracks] loop done: total_7=%d kept=%d count=%d\n",
          total_7letter, kept_after_letter_filter, count);
  fflush(stderr);

  qsort(racks, (size_t)count, sizeof(char *), cmp_strs);
  fprintf(stderr, "[passpegracks] qsort done\n");
  fflush(stderr);

  // Dedup adjacent equal signatures (anagrams collapse to one entry).
  // Be careful with the in-place compaction: don't double-free a slot.
  int unique = 0;
  for (int i = 0; i < count; i++) {
    if (unique == 0 || strcmp(racks[i], racks[unique - 1]) != 0) {
      racks[unique++] = racks[i];
    } else {
      free(racks[i]);
    }
  }
  fprintf(stderr, "[passpegracks] dedup done: unique=%d\n", unique);
  fflush(stderr);

  printf("\n=== Pass-PEG bingo rack enumeration (TWL98) ===\n");
  printf("Total 7-letter words: %d\n", total_7letter);
  printf("Survived letter+count filter (no Q/J/K/X/Z, <= dist/2): %d\n",
         kept_after_letter_filter);
  printf("Unique sorted-letter signatures: %d\n", unique);
  printf("\nFirst 30 racks:\n");
  int show = unique < 30 ? unique : 30;
  for (int i = 0; i < show; i++) {
    printf("  %s\n", racks[i]);
  }
  if (unique > 30) {
    printf("  ... and %d more\n", unique - 30);
  }
  printf("================================================\n");

  // Optional dump to file for downstream use.
  const char *out_path = "/tmp/pass_peg_bingo_racks.txt";
  FILE *out = fopen(out_path, "we");
  if (out) {
    for (int i = 0; i < unique; i++) {
      fprintf(out, "%s\n", racks[i]);
    }
    (void)fclose(out);
    printf("Wrote %d racks to %s\n", unique, out_path);
  }

  for (int i = 0; i < unique; i++) {
    free(racks[i]);
  }
  free(racks);
}

// ---------------------------------------------------------------------------
// Pass-PEG position search
// ---------------------------------------------------------------------------

// Bring mover's rack tile multiset out as a sorted A-Z signature like
// "AEINRST". Caller's buffer must be at least 8 bytes.
static void rack_to_sorted_sig(const Rack *rack, const LetterDistribution *ld,
                               int ld_size, char out[8]) {
  int idx = 0;
  for (int ml = 0; ml < ld_size && idx < 7; ml++) {
    if (ml == BLANK_MACHINE_LETTER) {
      continue;
    }
    int n = (int)rack_get_letter(rack, (MachineLetter)ml);
    for (int j = 0; j < n && idx < 7; j++) {
      const char *hl = ld->ld_ml_to_hl[ml];
      if (hl && hl[0] >= 'A' && hl[0] <= 'Z') {
        out[idx++] = hl[0];
      }
    }
  }
  out[idx] = '\0';
}

// Iterate the unseen pool (= 100 - mover_rack - board) into a sorted sig
// of length 8. Caller's buffer must be 9+.
static void unseen_sig_from_game(const Game *game, int mover_idx, char out[9]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mr, (MachineLetter)ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter on_board = board_get_letter(board, row, col);
      if (on_board == 0) {
        continue;
      }
      if (get_is_blanked(on_board)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0) {
          unseen[BLANK_MACHINE_LETTER]--;
        }
      } else {
        if (unseen[on_board] > 0) {
          unseen[on_board]--;
        }
      }
    }
  }
  int idx = 0;
  for (int ml = 0; ml < ld_size && idx < 8; ml++) {
    if (ml == BLANK_MACHINE_LETTER) {
      continue;
    }
    int n = (int)unseen[ml];
    for (int j = 0; j < n && idx < 8; j++) {
      const char *hl = ld->ld_ml_to_hl[ml];
      if (hl && hl[0] >= 'A' && hl[0] <= 'Z') {
        out[idx++] = hl[0];
      }
    }
  }
  out[idx] = '\0';
}

// Random play to 1-in-bag using best-by-equity moves.
static void play_until_1_in_bag(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 1 &&
         game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    const MoveGenArgs args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);
    if (move_list_get_count(move_list) == 0) {
      break;
    }
    play_move(move_list_get_move(move_list, 0), game, NULL);
  }
}

// Load the valid bingo-rack signatures into an in-memory sorted array.
// Returns the array length (caller frees `*out_array`).
static int load_valid_rack_sigs(const char *path, char ***out_array) {
  FILE *f = fopen(path, "re");
  if (!f) {
    *out_array = NULL;
    return 0;
  }
  int cap = 1024;
  int n = 0;
  char **arr = malloc_or_die((size_t)cap * sizeof(char *));
  char line[64];
  while (fgets(line, sizeof(line), f)) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len != 7) {
      continue;
    }
    if (n == cap) {
      cap *= 2;
      char **new_arr = realloc(arr, (size_t)cap * sizeof(char *));
      if (!new_arr) {
        log_fatal("realloc");
      }
      arr = new_arr;
    }
    arr[n] = malloc_or_die(8);
    memcpy(arr[n], line, 8);
    n++;
  }
  (void)fclose(f);
  qsort(arr, (size_t)n, sizeof(char *), cmp_strs);
  *out_array = arr;
  return n;
}

static bool sig_in_sorted_array(char *const *arr, int n, const char *sig) {
  int lo = 0;
  int hi = n - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int c = strcmp(arr[mid], sig);
    if (c == 0) {
      return true;
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return false;
}

void test_pass_peg_search(void) {
  fprintf(stderr, "[passpegsearch] starting\n");
  fflush(stderr);

  // Load (or generate) the valid bingo-rack signatures.
  const char *racks_path = "/tmp/pass_peg_bingo_racks.txt";
  char **valid_sigs = NULL;
  int n_sigs = load_valid_rack_sigs(racks_path, &valid_sigs);
  if (n_sigs == 0) {
    fprintf(stderr, "[passpegsearch] %s missing or empty; run pegracks first\n",
            racks_path);
    fflush(stderr);
    log_fatal("rack signatures not loaded");
  }
  fprintf(stderr, "[passpegsearch] loaded %d valid rack sigs\n", n_sigs);

  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex TWL98");
  Game *game = config_get_game(config);
  MoveList *ml = move_list_create(20);

  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  const char *target_env = getenv("PASSPEG_TARGET_COUNT");
  int target_count = target_env && *target_env ? atoi(target_env) : 1;
  if (target_count < 1) {
    target_count = 1;
  }
  const char *max_attempts_env = getenv("PASSPEG_MAX_ATTEMPTS");
  int max_attempts =
      max_attempts_env && *max_attempts_env ? atoi(max_attempts_env) : 200000;
  const char *seed_offset_env = getenv("PASSPEG_SEED_OFFSET");
  uint64_t seed_offset = seed_offset_env && *seed_offset_env
                             ? strtoull(seed_offset_env, NULL, 10)
                             : 1ULL;

  fprintf(stderr,
          "[passpegsearch] target_count=%d max_attempts=%d seed_offset=%llu\n",
          target_count, max_attempts, (unsigned long long)seed_offset);
  fflush(stderr);

  int found = 0;
  int peg_state_hits = 0;
  int q_in_unseen_hits = 0;
  int structural_hits = 0;
  for (int attempt = 0; attempt < max_attempts && found < target_count;
       attempt++) {
    if (attempt > 0 && attempt % 5000 == 0) {
      fprintf(stderr,
              "[passpegsearch] attempt=%d  peg_states=%d q_in_unseen=%d "
              "structural=%d found=%d\n",
              attempt, peg_state_hits, q_in_unseen_hits, structural_hits,
              found);
      fflush(stderr);
    }
    game_reset(game);
    game_seed(game, seed_offset + (uint64_t)attempt);
    draw_starting_racks(game);
    play_until_1_in_bag(game, ml);

    if (bag_get_letters(game_get_bag(game)) != 1) {
      continue;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }
    int mover_idx = game_get_player_on_turn_index(game);
    const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
    const Rack *opp_r = player_get_rack(game_get_player(game, 1 - mover_idx));
    if (rack_is_empty(mr) || rack_is_empty(opp_r)) {
      continue;
    }
    if (rack_get_total_letters(mr) != RACK_SIZE) {
      continue;
    }
    peg_state_hits++;

    // Check unseen contains exactly one Q.
    char unseen_sig[9];
    unseen_sig_from_game(game, mover_idx, unseen_sig);
    int q_count = 0;
    for (int i = 0; unseen_sig[i] != '\0'; i++) {
      if (unseen_sig[i] == 'Q') {
        q_count++;
      }
    }
    if (q_count != 1) {
      continue;
    }
    q_in_unseen_hits++;

    // The 15 unseen tiles must split as Q + 2*R for some bingo rack R.
    // Equivalently: count each non-Q letter in unseen; all counts must be
    // even (so the 14 non-Q tiles form 7 doubled letters), and the 7
    // distinct letters form a sorted signature that's in valid_sigs.
    // Mover's actual rack does NOT need to match — once we have a
    // structural fit, we rewrite the racks and bag to put R on mover and
    // R+Q across opp+bag.
    int letter_counts[26] = {0};
    for (int i = 0; unseen_sig[i] != '\0'; i++) {
      letter_counts[unseen_sig[i] - 'A']++;
    }
    char rack_sig[8];
    int ri2 = 0;
    bool ok_structure = true;
    for (int li = 0; li < 26 && ok_structure; li++) {
      if (li == ('Q' - 'A')) {
        continue;
      }
      int n = letter_counts[li];
      if (n == 0) {
        continue;
      }
      if (n % 2 != 0) {
        ok_structure = false;
        break;
      }
      int doubled = n / 2;
      // Each pair contributes one letter to the rack signature.
      for (int j = 0; j < doubled && ri2 < 7; j++) {
        rack_sig[ri2++] = (char)('A' + li);
      }
      if (ri2 > 7) {
        ok_structure = false;
        break;
      }
    }
    rack_sig[ri2] = '\0';
    if (!ok_structure || ri2 != 7) {
      continue;
    }
    structural_hits++;

    // The 7-letter signature must be a real bingo rack (some 7-letter
    // TWL98 word matches it).
    if (!sig_in_sorted_array(valid_sigs, n_sigs, rack_sig)) {
      continue;
    }
    char mover_sig[8];
    memcpy(mover_sig, rack_sig, 8);

    char *cgp = game_get_cgp(game, true);
    int mover_score =
        equity_to_int(player_get_score(game_get_player(game, mover_idx)));
    int opp_score =
        equity_to_int(player_get_score(game_get_player(game, 1 - mover_idx)));
    int lead = mover_score - opp_score;

    fprintf(stderr,
            "[passpegsearch] HIT seed=%llu mover_rack=%s unseen=%s lead=%+d\n",
            (unsigned long long)(seed_offset + (uint64_t)attempt), mover_sig,
            unseen_sig, lead);
    fprintf(stderr, "  CGP: %s\n", cgp);
    fflush(stderr);
    free(cgp);
    found++;
  }

  fprintf(stderr,
          "[passpegsearch] DONE: attempts=%d peg_states=%d q_in_unseen=%d "
          "structural=%d found=%d\n",
          max_attempts, peg_state_hits, q_in_unseen_hits, structural_hits,
          found);
  fflush(stderr);

  for (int i = 0; i < n_sigs; i++) {
    free(valid_sigs[i]);
  }
  free(valid_sigs);
  move_list_destroy(ml);
  config_destroy(config);
}
