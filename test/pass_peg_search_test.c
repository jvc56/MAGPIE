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
#include "../src/impl/bai_peg.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
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

// ---------------------------------------------------------------------------
// FORCED-rack search: pre-stack the bag, play to natural end-of-game,
// require both racks AND bag to be empty at end (so all 85 non-reserved
// tiles are on the board), then overwrite racks+bag with R, R, {Q}.
// ---------------------------------------------------------------------------

// Convert a 7-char rack signature like "AEINRST" into a per-letter count
// array and the matching MachineLetter sequence.
static int parse_rack_sig(const char *sig, const LetterDistribution *ld,
                          int letter_counts_out[26], MachineLetter ml_seq[7]) {
  for (int i = 0; i < 26; i++) {
    letter_counts_out[i] = 0;
  }
  int n = 0;
  for (int i = 0; i < 7 && sig[i] != '\0'; i++) {
    char c = sig[i];
    if (c < 'A' || c > 'Z') {
      return -1;
    }
    letter_counts_out[c - 'A']++;
    char hl[2] = {c, '\0'};
    MachineLetter mls[1];
    int m = ld_str_to_mls(ld, hl, false, mls, 1);
    if (m != 1) {
      return -1;
    }
    ml_seq[n++] = mls[0];
  }
  return n;
}

// Plays best-by-equity until the bag is empty, both racks are empty, or
// the consecutive-zero-turns end condition fires.
static void play_until_natural_end(Game *game, MoveList *move_list) {
  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
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

// Returns true iff Q has no playable move on the current board across
// every rack opp could actually hold in this scenario set. With
// mover_rack=R and unseen = R+Q, opp's actual scenario rack is
// always Q + 6-from-R (mover's bingo-tile is whatever opp doesn't
// see). For R with 7 unique letters that's 7 racks (each with one
// letter of R dropped). For R with duplicates, dropping each rack
// slot can produce equivalent multisets — we redundantly try all 7
// drops and rely on movegen being cheap.
static bool condition_q_unplayable(Game *game, int mover_idx,
                                   MachineLetter q_ml,
                                   const MachineLetter rack_ml[7]) {
  Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  Rack saved_rack;
  rack_copy(&saved_rack, mover_rack);

  uint64_t q_bit = (uint64_t)1 << q_ml;
  bool q_playable = false;
  // movegen still needs a valid MoveList in TILES_PLAYED mode (it builds
  // small-move records internally even though we don't read them).
  MoveList *probe_ml = move_list_create(64);

  for (int drop = 0; drop < 7 && !q_playable; drop++) {
    rack_reset(mover_rack);
    rack_add_letter(mover_rack, q_ml);
    for (int i = 0; i < 7; i++) {
      if (i == drop) {
        continue;
      }
      rack_add_letter(mover_rack, rack_ml[i]);
    }
    uint64_t tiles_played_bv = 0;
    const MoveGenArgs args = {
        .game = game,
        .move_list = probe_ml,
        .move_record_type = MOVE_RECORD_TILES_PLAYED,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .tiles_played_bv = &tiles_played_bv,
        .initial_tiles_bv = 0,
    };
    generate_moves(&args);
    if (tiles_played_bv & q_bit) {
      q_playable = true;
    }
  }

  move_list_destroy(probe_ml);
  rack_copy(mover_rack, &saved_rack);
  return !q_playable;
}

// Generate moves for the player on turn (using their actual rack) and
// return all 7-tile bingos. Caller owns the move list and just iterates
// it.
static int gen_bingos_for_current_rack(Game *game, MoveList *ml) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = ml,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  return move_list_get_count(ml);
}

// Returns true if every 7-tile bingo in `ml` shares the same (row_start,
// col_start, dir). The bingo count is also written to *out_bingo_count.
static bool condition_single_bingo_lane(MoveList *ml, int *out_bingo_count) {
  int n = move_list_get_count(ml);
  int row0 = -1;
  int col0 = -1;
  int dir0 = -1;
  int bingo_count = 0;
  for (int i = 0; i < n; i++) {
    const Move *m = move_list_get_move(ml, i);
    if (move_get_tiles_played(m) != 7) {
      continue;
    }
    bingo_count++;
    if (row0 < 0) {
      row0 = m->row_start;
      col0 = m->col_start;
      dir0 = m->dir;
    } else if (m->row_start != row0 || m->col_start != col0 || m->dir != dir0) {
      *out_bingo_count = bingo_count;
      return false;
    }
  }
  *out_bingo_count = bingo_count;
  return bingo_count > 0;
}

// For each mover bingo, check that opp (with rack already set to R) can
// also play a 7-tile bingo on the resulting board. Returns true iff every
// mover bingo has at least one opp bingo response.
static bool condition_all_bingos_answerable(const Game *game, MoveList *ml,
                                            int opp_idx) {
  // Snapshot mover bingos.
  int n = move_list_get_count(ml);
  Move bingos[64];
  int bingo_count = 0;
  for (int i = 0; i < n && bingo_count < 64; i++) {
    const Move *m = move_list_get_move(ml, i);
    if (move_get_tiles_played(m) == 7) {
      bingos[bingo_count++] = *m;
    }
  }
  if (bingo_count == 0) {
    return false;
  }
  MoveList *resp_ml = move_list_create(20);
  bool all_answerable = true;
  for (int b = 0; b < bingo_count && all_answerable; b++) {
    Game *post = game_duplicate(game);
    game_set_endgame_solving_mode(post);
    game_set_backup_mode(post, BACKUP_MODE_OFF);
    play_move_without_drawing_tiles(&bingos[b], post);
    game_set_game_end_reason(post, GAME_END_REASON_NONE);
    // Opp is now on turn (play_move_without_drawing flipped it).
    const MoveGenArgs args = {
        .game = post,
        .move_list = resp_ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);
    int rn = move_list_get_count(resp_ml);
    bool found_resp = false;
    for (int j = 0; j < rn; j++) {
      const Move *rm = move_list_get_move(resp_ml, j);
      if (move_get_tiles_played(rm) == 7) {
        found_resp = true;
        break;
      }
    }
    if (!found_resp) {
      all_answerable = false;
    }
    (void)opp_idx;
    game_destroy(post);
  }
  move_list_destroy(resp_ml);
  return all_answerable;
}

// Play through the 85-tile-system game while suppressing end-of-game,
// until both racks AND the bag are empty (= all 85 reserved-removed
// tiles are on the board). Returns true on success. Bails out if a
// player has tiles but no legal move (rare; e.g. an unplayable rack
// like five Vs).
static bool play_85_system_to_drain(Game *game, MoveList *ml) {
  // Cap iterations to avoid infinite loops if both players have only
  // unplayable racks.
  const int max_iters = 200;
  for (int iter = 0; iter < max_iters; iter++) {
    Rack *r0 = player_get_rack(game_get_player(game, 0));
    Rack *r1 = player_get_rack(game_get_player(game, 1));
    int bag_n = bag_get_letters(game_get_bag(game));
    if (rack_get_total_letters(r0) == 0 && rack_get_total_letters(r1) == 0 &&
        bag_n == 0) {
      return true;
    }
    int turn = game_get_player_on_turn_index(game);
    Rack *cur_rack = player_get_rack(game_get_player(game, turn));

    const MoveGenArgs args = {
        .game = game,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);
    if (move_list_get_count(ml) == 0) {
      // Player can't move. If they also have tiles, we're stuck — abort.
      if (rack_get_total_letters(cur_rack) > 0) {
        return false;
      }
      // Empty rack and no moves: just advance the turn.
      game_start_next_player_turn(game);
      continue;
    }
    play_move(move_list_get_move(ml, 0), game, NULL);
    // Suppress whatever end-of-game state the play may have triggered.
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    game_set_consecutive_scoreless_turns(game, 0);
  }
  return false;
}

void test_pass_peg_search_forced(void) {
  fprintf(stderr, "[passpegforce] starting\n");
  fflush(stderr);

  const char *rack_env = getenv("PASSPEG_FORCE_RACK");
  const char *target_rack = rack_env && *rack_env ? rack_env : "AEINRST";
  if (strlen(target_rack) != 7) {
    log_fatal("PASSPEG_FORCE_RACK must be a 7-letter sorted signature");
  }
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
          "[passpegforce] R=%s target_count=%d max_attempts=%d "
          "seed_offset=%llu\n",
          target_rack, target_count, max_attempts,
          (unsigned long long)seed_offset);
  fflush(stderr);

  // Multi-rack mode: cycle through every valid bingo rack signature
  // (one per attempt) so candidates come from varied racks rather than a
  // fixed R. Triggered when PASSPEG_FORCE_RACK is unset / empty.
  bool multi_rack = (rack_env == NULL || *rack_env == '\0');
  char **valid_sigs = NULL;
  int n_sigs = 0;
  if (multi_rack) {
    const char *racks_path = "/tmp/pass_peg_bingo_racks.txt";
    n_sigs = load_valid_rack_sigs(racks_path, &valid_sigs);
    if (n_sigs == 0) {
      log_fatal("could not load %s; run pegracks first", racks_path);
    }
    fprintf(stderr,
            "[passpegforce] multi-rack mode: cycling through %d racks\n",
            n_sigs);
    fflush(stderr);
  }

  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex TWL98");
  Game *game = config_get_game(config);
  MoveList *ml = move_list_create(20);
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  // Parse the seed rack signature (only the static fixed-R mode uses this
  // up front — multi-rack reparses on every attempt).
  int rack_counts[26];
  MachineLetter rack_ml[7];
  if (!multi_rack) {
    int n_rack_letters = parse_rack_sig(target_rack, ld, rack_counts, rack_ml);
    if (n_rack_letters != 7) {
      log_fatal("could not parse target rack '%s'", target_rack);
    }
  }
  // The Q ML.
  MachineLetter q_ml = (MachineLetter)0;
  {
    char hl[2] = {'Q', '\0'};
    MachineLetter mls[1];
    int m = ld_str_to_mls(ld, hl, false, mls, 1);
    if (m != 1) {
      log_fatal("could not find Q in TWL98 LD");
    }
    q_ml = mls[0];
  }

  // Filter funnel counters.
  int n_attempts = 0;
  int n_drained = 0;   // 85-tile autoplay reached both-racks-empty
  int n_lead_band = 0; // mover lead in [1, 25]
  int n_q_unplayable = 0;
  int n_single_lane = 0;
  int n_all_answerable = 0;
  int found = 0;
  // Lead distribution buckets.
  int n_lead_neg = 0;
  int n_lead_zero = 0;
  int n_lead_high = 0;

  for (int attempt = 0; attempt < max_attempts && found < target_count;
       attempt++) {
    n_attempts++;
    if (attempt > 0 && attempt % 1000 == 0) {
      fprintf(stderr,
              "[passpegforce] attempt=%d drained=%d band=%d "
              "found=%d\n",
              attempt, n_drained, n_lead_band, found);
      fflush(stderr);
    }
    if (multi_rack) {
      target_rack = valid_sigs[attempt % n_sigs];
      int n_rack_letters =
          parse_rack_sig(target_rack, ld, rack_counts, rack_ml);
      if (n_rack_letters != 7) {
        continue;
      }
    }
    game_reset(game);
    game_seed(game, seed_offset + (uint64_t)attempt);

    // Drain 2*R + Q from the bag (leaving 85 tiles).
    Bag *bag = game_get_bag(game);
    bool drain_ok = true;
    for (int li = 0; li < 26 && drain_ok; li++) {
      int n_to_drain = rack_counts[li] * 2;
      if (n_to_drain == 0) {
        continue;
      }
      char hl[2] = {(char)('A' + li), '\0'};
      MachineLetter mls[1];
      if (ld_str_to_mls(ld, hl, false, mls, 1) != 1) {
        drain_ok = false;
        break;
      }
      for (int j = 0; j < n_to_drain; j++) {
        if (!bag_draw_letter(bag, mls[0], 0)) {
          drain_ok = false;
          break;
        }
      }
    }
    if (drain_ok) {
      if (!bag_draw_letter(bag, q_ml, 0)) {
        drain_ok = false;
      }
    }
    if (!drain_ok) {
      continue;
    }

    // Now bag has 85 tiles. Draw starting racks + run the 85-tile-system
    // autoplay with end-of-game suppressed, until both racks AND bag are
    // empty (= all 85 reserved-removed tiles played onto the board).
    draw_starting_racks(game);
    if (!play_85_system_to_drain(game, ml)) {
      // Got stuck (e.g. unplayable rack). Skip.
      continue;
    }
    n_drained++;

    // Read the reserved tiles back into the bag (= 14 + 1 = 15 tiles).
    for (int i = 0; i < 7; i++) {
      bag_add_letter(bag, rack_ml[i], 0); // R copy 1
    }
    for (int i = 0; i < 7; i++) {
      bag_add_letter(bag, rack_ml[i], 1); // R copy 2 (opposite end for variety)
    }
    bag_add_letter(bag, q_ml, 0);

    // Override racks: mover = R, opp = R, bag = {Q}. We just placed
    // R+R+Q in the bag; manually pull them into the assigned slots so
    // the bag ends with exactly one Q.
    int mover_idx = game_get_player_on_turn_index(game);
    int opp_idx = 1 - mover_idx;
    Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
    Rack *opp_rack = player_get_rack(game_get_player(game, opp_idx));
    bool deal_ok = true;
    for (int i = 0; i < 7 && deal_ok; i++) {
      if (!bag_draw_letter(bag, rack_ml[i], 0)) {
        deal_ok = false;
        break;
      }
      rack_add_letter(mover_rack, rack_ml[i]);
    }
    for (int i = 0; i < 7 && deal_ok; i++) {
      if (!bag_draw_letter(bag, rack_ml[i], 1)) {
        deal_ok = false;
        break;
      }
      rack_add_letter(opp_rack, rack_ml[i]);
    }
    if (!deal_ok) {
      continue;
    }
    // Bag should now have just the Q.
    if (bag_get_letters(bag) != 1) {
      continue;
    }
    // Clear any lingering end-of-game signals from the drain phase.
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    game_set_consecutive_scoreless_turns(game, 0);

    int mover_score =
        equity_to_int(player_get_score(game_get_player(game, mover_idx)));
    int opp_score =
        equity_to_int(player_get_score(game_get_player(game, opp_idx)));
    int lead = mover_score - opp_score;
    if (lead < 0) {
      n_lead_neg++;
    } else if (lead == 0) {
      n_lead_zero++;
    } else if (lead > 25) {
      n_lead_high++;
    } else {
      n_lead_band++;
    }
    if (lead < 1 || lead > 25) {
      // Out of band — keep counting for the funnel, but skip emit.
      continue;
    }

    // Condition 2: Q is unplayable on the current board.
    if (!condition_q_unplayable(game, mover_idx, q_ml, rack_ml)) {
      continue;
    }
    n_q_unplayable++;

    // Generate mover bingos. Required for conditions 3 and 4.
    int n_moves = gen_bingos_for_current_rack(game, ml);
    if (n_moves == 0) {
      continue;
    }
    int bingo_count = 0;
    if (!condition_single_bingo_lane(ml, &bingo_count)) {
      continue;
    }
    n_single_lane++;
    if (!condition_all_bingos_answerable(game, ml, opp_idx)) {
      continue;
    }
    n_all_answerable++;

    char *cgp = game_get_cgp(game, true);
    fprintf(stderr,
            "[passpegforce] HIT seed=%llu R=%s lead=%+d bingos=%d "
            "(mover=%d opp=%d)\n",
            (unsigned long long)(seed_offset + (uint64_t)attempt), target_rack,
            lead, bingo_count, mover_score, opp_score);
    fprintf(stderr, "  CGP: %s\n", cgp);
    fflush(stderr);
    // Append candidate to /tmp/passpeg_candidates.txt (one row each).
    {
      static FILE *out_file = NULL;
      if (out_file == NULL) {
        out_file = fopen("/tmp/passpeg_candidates.txt", "we");
      }
      if (out_file) {
        fprintf(out_file, "%s\tR=%s\tlead=%+d\tbingos=%d\n", cgp, target_rack,
                lead, bingo_count);
        fflush(out_file);
      }
    }
    free(cgp);
    found++;
  }

  if (valid_sigs) {
    for (int i = 0; i < n_sigs; i++) {
      free(valid_sigs[i]);
    }
    free(valid_sigs);
  }
  (void)ld_size;

  printf("\n=== Pass-PEG forced search filter funnel (%s) ===\n",
         multi_rack ? "multi-rack" : target_rack);
  printf("  attempts                                      : %d\n", n_attempts);
  printf("  85-system drained (both racks + bag empty)    : %d\n", n_drained);
  printf("  lead distribution within drained:\n");
  printf("    < 0                                         : %d\n", n_lead_neg);
  printf("    == 0                                        : %d\n", n_lead_zero);
  printf("    1..25 (target band)                         : %d\n", n_lead_band);
  printf("    > 25                                        : %d\n", n_lead_high);
  printf("  cond 2: Q unplayable                          : %d\n",
         n_q_unplayable);
  printf("  cond 3: single bingo lane                     : %d\n",
         n_single_lane);
  printf("  cond 4: all bingos answerable                 : %d\n",
         n_all_answerable);
  printf("  positions emitted (passes 1..4)               : %d\n", found);
  printf("====================================================\n");

  (void)ld_size;
  move_list_destroy(ml);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Sample solve: run bai_peg_solve on the first N CGPs from
// /tmp/passpeg_candidates.txt (one row per candidate from passpegforce),
// dumping per-cand visit/depth_evaluated stats so we can see how PUCT
// allocates compute on these engineered pass-PEG positions.
// ---------------------------------------------------------------------------

void test_pass_peg_sample_solve(void) {
  const char *n_env = getenv("PASSPEG_SAMPLE_N");
  int sample_n = n_env && *n_env ? atoi(n_env) : 10;
  if (sample_n < 1) {
    sample_n = 1;
  }
  const char *time_env = getenv("PASSPEG_SAMPLE_TIME");
  double time_budget = time_env && *time_env ? atof(time_env) : 15.0;
  const char *workers_env = getenv("PASSPEG_SAMPLE_WORKERS");
  int workers = workers_env && *workers_env ? atoi(workers_env) : 8;
  const char *topk_env = getenv("PASSPEG_SAMPLE_TOP_K");
  int top_k = topk_env && *topk_env ? atoi(topk_env) : 32;

  const char *cgps_path = "/tmp/passpeg_candidates.txt";
  FILE *f = fopen(cgps_path, "re");
  if (!f) {
    log_fatal("could not open %s", cgps_path);
  }

  fprintf(stderr, "[passpegsample] n=%d time/pos=%.1fs workers=%d top_k=%d\n",
          sample_n, time_budget, workers, top_k);
  fflush(stderr);

  Config *config = config_create_or_die("set -s1 score -s2 score");
  BaiPegExecutor *executor =
      workers > 0 ? bai_peg_executor_create(workers, 100) : NULL;

  printf("\n%-3s %-12s %5s %5s %3s %4s %5s %4s %s\n", "Pos", "R", "lead",
         "evals", "B?", "best", "win%", "dpth", "best_move");
  printf("--- ------------ ----- ----- --- ---- ----- ---- "
         "------------------------------\n");

  // Per-cand visit/depth aggregate stats across all sampled positions.
  long total_visits = 0;
  long total_evals = 0;
  int total_cands = 0;
  int max_depth_seen = 0;
  int min_depth_seen = 1000;
  long sum_depth = 0;
  int pass_best_count = 0;

  char line[8192];
  int processed = 0;
  while (processed < sample_n && fgets(line, sizeof(line), f)) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    // Split on tab: CGP\tR=...\tlead=...\tbingos=...
    char *tab = strchr(line, '\t');
    char rack_letter[16] = "?";
    int lead = 0;
    if (tab) {
      *tab = '\0';
      const char *meta = tab + 1;
      // R=XXXX
      const char *r_eq = strstr(meta, "R=");
      if (r_eq) {
        sscanf(r_eq + 2, "%15s", rack_letter);
      }
      const char *lead_eq = strstr(meta, "lead=");
      if (lead_eq) {
        sscanf(lead_eq + 5, "%d", &lead);
      }
    }

    char load_cmd[10240];
    snprintf(load_cmd, sizeof(load_cmd), "cgp %s -lex TWL98", line);
    load_and_exec_config_or_die(config, load_cmd);
    Game *game = config_get_game(config);

    BaiPegArgs args = {
        .game = game,
        .thread_control = config_get_thread_control(config),
        .num_threads = 1,
        .tt_fraction_of_mem = 0.0,
        .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
        .initial_top_k = top_k,
        .max_depth = BAI_PEG_MAX_DEPTH,
        .endgame_time_per_solve = 5.0,
        .time_budget_seconds = time_budget,
        .puct_c = 1.0,
        .utility_alpha = 1e-4,
        .progressive_widening = false,
        .min_active = 0,
        .sweep_max_depth = 0, // normal PUCT, not sweep
        .include_pass = true,
        .pass_opp_max_depth = 0, // coupled mode tracks mover PUCT depth
        .executor = executor,
        .log_solve_details = false,
        .request_cand_stats = true,
    };
    BaiPegResult result;
    ErrorStack *err = error_stack_create();
    bai_peg_solve(&args, &result, err);
    assert(error_stack_is_empty(err));

    bool best_is_pass = small_move_is_pass(&result.best_move);
    if (best_is_pass) {
      pass_best_count++;
    }

    // Format best move text.
    char best_text[64];
    if (best_is_pass) {
      snprintf(best_text, sizeof(best_text), "(Pass)");
    } else {
      Move m;
      small_move_to_move(&m, &result.best_move, game_get_board(game));
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(game), &m, game_get_ld(game),
                              /*add_score=*/true);
      const char *peek = string_builder_peek(sb);
      size_t pn = strlen(peek);
      if (pn >= sizeof(best_text)) {
        pn = sizeof(best_text) - 1;
      }
      memcpy(best_text, peek, pn);
      best_text[pn] = '\0';
      string_builder_destroy(sb);
    }

    printf("%-3d %-12s %+5d %5d %3s %s %5.3f %4d %-30s\n", processed + 1,
           rack_letter, lead, result.evaluations_done, best_is_pass ? "Y" : "N",
           best_is_pass ? "PASS" : "play", result.best_win_pct,
           result.best_depth_evaluated, best_text);

    // Aggregate across cands.
    if (result.cand_stats) {
      for (int i = 0; i < result.candidates_considered; i++) {
        const BaiCandStats *s = &result.cand_stats[i];
        total_visits += s->visits;
        if (s->depth_evaluated > 0) {
          if (s->depth_evaluated > max_depth_seen) {
            max_depth_seen = s->depth_evaluated;
          }
          if (s->depth_evaluated < min_depth_seen) {
            min_depth_seen = s->depth_evaluated;
          }
        }
        sum_depth += s->depth_evaluated;
        total_cands++;
      }
    }
    total_evals += result.evaluations_done;

    error_stack_destroy(err);
    bai_cand_stats_free(result.cand_stats);
    processed++;
  }
  (void)fclose(f);

  printf("\n=== Sample solve aggregate (%d positions) ===\n", processed);
  printf("  pass_best_count        : %d / %d\n", pass_best_count, processed);
  printf("  total cand evaluations : %ld\n", total_evals);
  printf("  total cand visits      : %ld\n", total_visits);
  printf("  total cands seen       : %d\n", total_cands);
  if (total_cands > 0) {
    printf("  avg visits per cand    : %.2f\n",
           (double)total_visits / total_cands);
    printf("  avg depth per cand     : %.2f\n",
           (double)sum_depth / total_cands);
  }
  printf("  min/max depth seen     : %d / %d\n", min_depth_seen,
         max_depth_seen);
  printf("============================================\n");

  if (executor) {
    bai_peg_executor_destroy(executor);
  }
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Random 1-peg generator (no engineering): autoplay-to-1peg starting from
// random seeds, output CGPs that PASS the basic 1-peg shape (bag=1, both
// racks full).
// ---------------------------------------------------------------------------

void test_pass_peg_generate_random_1pegs(void) {
  const char *n_env = getenv("PASSPEG_RAND_N");
  int target_n = n_env && *n_env ? atoi(n_env) : 100;
  if (target_n < 1) {
    target_n = 1;
  }
  const char *seed_env = getenv("PASSPEG_RAND_SEED");
  uint64_t seed_offset =
      seed_env && *seed_env ? strtoull(seed_env, NULL, 10) : 100001ULL;

  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex TWL98");
  Game *game = config_get_game(config);
  MoveList *ml = move_list_create(20);

  const char *out_path = "/tmp/random_1pegs.txt";
  FILE *f = fopen(out_path, "we");
  if (!f) {
    log_fatal("cannot open %s", out_path);
  }
  int found = 0;
  int attempt = 0;
  fprintf(stderr, "[passpegrand] generating %d random 1-pegs\n", target_n);
  fflush(stderr);
  while (found < target_n && attempt < 1000000) {
    game_reset(game);
    game_seed(game, seed_offset + (uint64_t)attempt);
    draw_starting_racks(game);
    play_until_1_in_bag(game, ml);
    attempt++;
    if (bag_get_letters(game_get_bag(game)) != 1) {
      continue;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }
    int mover_idx = game_get_player_on_turn_index(game);
    const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
    const Rack *opp_r = player_get_rack(game_get_player(game, 1 - mover_idx));
    if (rack_get_total_letters(mr) != RACK_SIZE) {
      continue;
    }
    if (rack_is_empty(opp_r)) {
      continue;
    }
    char *cgp = game_get_cgp(game, true);
    fprintf(f, "%s\n", cgp);
    free(cgp);
    found++;
    if (found % 10 == 0) {
      fprintf(stderr, "[passpegrand] %d/%d (after %d attempts)\n", found,
              target_n, attempt);
      fflush(stderr);
    }
  }
  (void)fclose(f);
  fprintf(stderr, "[passpegrand] DONE: %d positions in %d attempts -> %s\n",
          found, attempt, out_path);
  fflush(stderr);
  move_list_destroy(ml);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Bench: run bai_peg_solve in 4 variants × 3 wall budgets across the
// 100 engineered + 100 random 1-pegs.
// Variants: {include_pass=true, include_pass=false} × {algo=PUCT, algo=SH}.
// Budgets: 10, 20, 30 seconds.
// CSV out: /tmp/passpeg_bench.csv
// ---------------------------------------------------------------------------

static int load_cgp_lines(const char *path, char ***out, int max_n) {
  FILE *f = fopen(path, "re");
  if (!f) {
    return 0;
  }
  char **arr = malloc_or_die((size_t)max_n * sizeof(char *));
  int n = 0;
  char line[8192];
  while (n < max_n && fgets(line, sizeof(line), f)) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }
    char *tab = strchr(line, '\t');
    if (tab) {
      *tab = '\0';
    }
    arr[n] = string_duplicate(line);
    n++;
  }
  (void)fclose(f);
  *out = arr;
  return n;
}

void test_pass_peg_bench(void) {
  const char *n_pass_env = getenv("PASSPEG_BENCH_N_PASS");
  const char *n_rand_env = getenv("PASSPEG_BENCH_N_RAND");
  int n_pass = n_pass_env && *n_pass_env ? atoi(n_pass_env) : 100;
  int n_rand = n_rand_env && *n_rand_env ? atoi(n_rand_env) : 100;

  // Comma-separated budgets; default 10,20,30.
  const char *budgets_env = getenv("PASSPEG_BENCH_BUDGETS");
  const char *budgets_str =
      budgets_env && *budgets_env ? budgets_env : "10,20,30";
  double budgets[8] = {0};
  int num_budgets = 0;
  {
    const char *p = budgets_str;
    while (*p && num_budgets < 8) {
      char *end;
      double v = strtod(p, &end);
      if (end == p) {
        break;
      }
      budgets[num_budgets++] = v;
      p = (*end == ',') ? end + 1 : end;
    }
  }
  if (num_budgets == 0) {
    log_fatal("no budgets parsed");
  }

  const char *workers_env = getenv("PASSPEG_BENCH_WORKERS");
  int workers = workers_env && *workers_env ? atoi(workers_env) : 8;
  const char *topk_env = getenv("PASSPEG_BENCH_TOP_K");
  int top_k = topk_env && *topk_env ? atoi(topk_env) : 32;

  // Optional: skip variants for partial reruns.
  const char *skip_sh_env = getenv("PASSPEG_BENCH_SKIP_SH");
  bool skip_sh = skip_sh_env && *skip_sh_env && atoi(skip_sh_env) != 0;
  const char *skip_puct_env = getenv("PASSPEG_BENCH_SKIP_PUCT");
  bool skip_puct = skip_puct_env && *skip_puct_env && atoi(skip_puct_env) != 0;

  char **pass_cgps = NULL;
  int loaded_pass =
      load_cgp_lines("/tmp/passpeg_candidates.txt", &pass_cgps, n_pass);
  char **rand_cgps = NULL;
  int loaded_rand = load_cgp_lines("/tmp/random_1pegs.txt", &rand_cgps, n_rand);
  if (loaded_pass < n_pass || loaded_rand < n_rand) {
    fprintf(stderr,
            "[passpegbench] WARN loaded pass=%d/%d rand=%d/%d (continuing)\n",
            loaded_pass, n_pass, loaded_rand, n_rand);
    if (loaded_pass < n_pass) {
      n_pass = loaded_pass;
    }
    if (loaded_rand < n_rand) {
      n_rand = loaded_rand;
    }
  }
  fprintf(stderr,
          "[passpegbench] pass=%d rand=%d budgets=%d workers=%d top_k=%d\n",
          n_pass, n_rand, num_budgets, workers, top_k);
  fflush(stderr);

  const char *csv_path = "/tmp/passpeg_bench.csv";
  FILE *csv = fopen(csv_path, "we");
  if (!csv) {
    log_fatal("cannot open %s", csv_path);
  }
  fprintf(csv, "pos_id,source,algo,include_pass,budget_secs,wall_secs,"
               "evals_done,best_is_pass,best_win_pct,best_spread,"
               "best_depth,cands_considered\n");
  fflush(csv);

  Config *config = config_create_or_die("set -s1 score -s2 score");
  BaiPegExecutor *executor =
      workers > 0 ? bai_peg_executor_create(workers, 100) : NULL;

  // Variant matrix: (include_pass, algo) × budgets.
  // algo: 0 = PUCT, 1 = Sequential Halving.
  int total_variants = 0;
  if (!skip_puct) {
    total_variants += 2; // PUCT × {pass, no-pass}
  }
  if (!skip_sh) {
    total_variants += 2;
  }
  int total_runs = (n_pass + n_rand) * total_variants * num_budgets;
  int run_idx = 0;
  fprintf(stderr, "[passpegbench] total_runs=%d\n", total_runs);
  fflush(stderr);

  for (int src = 0; src < 2; src++) {
    int n_total = (src == 0) ? n_pass : n_rand;
    char **arr = (src == 0) ? pass_cgps : rand_cgps;
    const char *src_label = (src == 0) ? "passpeg" : "random";
    for (int p = 0; p < n_total; p++) {
      const char *cgp = arr[p];
      char load_cmd[10240];
      snprintf(load_cmd, sizeof(load_cmd), "cgp %s -lex TWL98", cgp);
      // (variant configs)
      for (int algo = 0; algo < 2; algo++) {
        if (algo == 0 && skip_puct) {
          continue;
        }
        if (algo == 1 && skip_sh) {
          continue;
        }
        for (int incp = 0; incp < 2; incp++) {
          for (int b = 0; b < num_budgets; b++) {
            double budget = budgets[b];
            // Reload CGP fresh for each run.
            load_and_exec_config_or_die(config, load_cmd);
            Game *game = config_get_game(config);
            BaiPegArgs args = {
                .game = game,
                .thread_control = config_get_thread_control(config),
                .num_threads = 1,
                .tt_fraction_of_mem = 0.0,
                .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
                .initial_top_k = top_k,
                .max_depth = BAI_PEG_MAX_DEPTH,
                .endgame_time_per_solve = 5.0,
                .time_budget_seconds = budget,
                .puct_c = 1.0,
                .utility_alpha = 1e-4,
                .progressive_widening = false,
                .min_active = 0,
                .sweep_max_depth = 0,
                .include_pass = (incp == 1),
                .pass_opp_max_depth = 0,
                .executor = executor,
                .log_solve_details = false,
                .request_cand_stats = false,
                .sequential_halving = (algo == 1),
            };
            BaiPegResult result;
            ErrorStack *err = error_stack_create();
            bai_peg_solve(&args, &result, err);
            assert(error_stack_is_empty(err));
            bool best_is_pass = small_move_is_pass(&result.best_move);
            fprintf(csv, "%d,%s,%s,%d,%.0f,%.3f,%d,%d,%.4f,%.4f,%d,%d\n", p,
                    src_label, algo == 0 ? "PUCT" : "SH", incp, budget,
                    result.seconds_elapsed, result.evaluations_done,
                    best_is_pass ? 1 : 0, result.best_win_pct,
                    result.best_mean_spread, result.best_depth_evaluated,
                    result.candidates_considered);
            fflush(csv);
            error_stack_destroy(err);
            bai_cand_stats_free(result.cand_stats);
            run_idx++;
            if (run_idx % 20 == 0) {
              fprintf(stderr,
                      "[passpegbench] progress %d / %d (src=%s p=%d "
                      "algo=%s pass=%d budget=%.0f)\n",
                      run_idx, total_runs, src_label, p,
                      algo == 0 ? "PUCT" : "SH", incp, budget);
              fflush(stderr);
            }
          }
        }
      }
    }
  }

  (void)fclose(csv);
  fprintf(stderr, "[passpegbench] DONE: %d runs -> %s\n", run_idx, csv_path);
  fflush(stderr);

  if (executor) {
    bai_peg_executor_destroy(executor);
  }
  for (int i = 0; i < n_pass; i++) {
    free(pass_cgps[i]);
  }
  free(pass_cgps);
  for (int i = 0; i < n_rand; i++) {
    free(rand_cgps[i]);
  }
  free(rand_cgps);
  config_destroy(config);
}
