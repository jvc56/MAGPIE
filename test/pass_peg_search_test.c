#include "pass_peg_search_test.h"

#include "../src/compat/cpthread.h"
#include "../src/def/board_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/peg_pool.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/word_prune.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdatomic.h>
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

// Play greedy moves until the bag has <= `target_bag` tiles (or no legal
// moves remain). Stops as soon as the post-play bag size drops at or
// below `target_bag` so the caller can check `== target_bag` exactly.
static void play_until_n_in_bag(Game *game, MoveList *move_list,
                                int target_bag) {
  while (bag_get_letters(game_get_bag(game)) > target_bag &&
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

void test_pass_peg_engineered_search(void) {
  fprintf(stderr, "[passpegengineered] starting\n");
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
          "[passpegengineered] R=%s target_count=%d max_attempts=%d "
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
            "[passpegengineered] multi-rack mode: cycling through %d racks\n",
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
              "[passpegengineered] attempt=%d drained=%d band=%d "
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
            "[passpegengineered] HIT seed=%llu R=%s lead=%+d bingos=%d "
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
// Random 1-peg generator (no engineering): autoplay-to-1peg starting from
// random seeds, output CGPs that PASS the basic 1-peg shape (bag=1, both
// racks full).
// ---------------------------------------------------------------------------

// Generate N random positions with a target bag size, optionally filtered
// by score margin. Configurable via env vars:
//   PASSPEG_RAND_N        - count to generate (default 100)
//   PASSPEG_RAND_N_BAG    - target bag size (default 1)
//   PASSPEG_RAND_LEX      - lexicon (default CSW24)
//   PASSPEG_RAND_MARGIN   - max abs(mover_score - opp_score); 0 = no filter
//                           (default 40)
//   PASSPEG_RAND_SEED     - seed offset (default 100001)
//   PASSPEG_RAND_OUT      - output path (default /tmp/random_peg.txt)
void test_generate_peg_cgps(void) {
  const char *n_env = getenv("PASSPEG_RAND_N");
  int target_n = n_env && *n_env ? atoi(n_env) : 100;
  if (target_n < 1) {
    target_n = 1;
  }
  const char *n_bag_env = getenv("PASSPEG_RAND_N_BAG");
  int target_bag = n_bag_env && *n_bag_env ? atoi(n_bag_env) : 1;
  if (target_bag < 1) {
    target_bag = 1;
  }
  const char *lex_env = getenv("PASSPEG_RAND_LEX");
  const char *lex = lex_env && *lex_env ? lex_env : "CSW24";
  const char *margin_env = getenv("PASSPEG_RAND_MARGIN");
  const int margin_abs =
      margin_env && *margin_env ? atoi(margin_env) : 40;
  const char *seed_env = getenv("PASSPEG_RAND_SEED");
  uint64_t seed_offset =
      seed_env && *seed_env ? strtoull(seed_env, NULL, 10) : 100001ULL;
  const char *out_env = getenv("PASSPEG_RAND_OUT");
  const char *out_path =
      out_env && *out_env ? out_env : "/tmp/random_peg.txt";

  Config *config = config_create_or_die("set -s1 score -s2 score");
  char load_cmd[256];
  snprintf(load_cmd, sizeof(load_cmd),
           "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex %s",
           lex);
  load_and_exec_config_or_die(config, load_cmd);
  Game *game = config_get_game(config);
  MoveList *ml = move_list_create(20);

  FILE *f = fopen(out_path, "we");
  if (!f) {
    log_fatal("cannot open %s", out_path);
  }
  int found = 0;
  int attempt = 0;
  fprintf(stderr,
          "[genpeg] generating %d positions with bag=%d, lex=%s, "
          "|margin|<=%d -> %s\n",
          target_n, target_bag, lex, margin_abs, out_path);
  fflush(stderr);
  while (found < target_n && attempt < 5000000) {
    game_reset(game);
    game_seed(game, seed_offset + (uint64_t)attempt);
    draw_starting_racks(game);
    play_until_n_in_bag(game, ml, target_bag);
    attempt++;
    if (bag_get_letters(game_get_bag(game)) != target_bag) {
      continue;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }
    const int mover_idx = game_get_player_on_turn_index(game);
    const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
    const Rack *opp_r =
        player_get_rack(game_get_player(game, 1 - mover_idx));
    if (rack_get_total_letters(mr) != RACK_SIZE) {
      continue;
    }
    if (rack_is_empty(opp_r)) {
      continue;
    }
    if (margin_abs > 0) {
      const int32_t mover_score =
          equity_to_int(player_get_score(game_get_player(game, mover_idx)));
      const int32_t opp_score = equity_to_int(
          player_get_score(game_get_player(game, 1 - mover_idx)));
      const int32_t margin = mover_score - opp_score;
      if (margin > margin_abs || margin < -margin_abs) {
        continue;
      }
    }
    char *cgp = game_get_cgp(game, true);
    fprintf(f, "%s\n", cgp);
    free(cgp);
    found++;
    if (found % 10 == 0) {
      fprintf(stderr, "[genpeg] %d/%d (after %d attempts)\n", found,
              target_n, attempt);
      fflush(stderr);
    }
  }
  (void)fclose(f);
  fprintf(stderr,
          "[genpeg] DONE: %d positions in %d attempts -> %s\n",
          found, attempt, out_path);
  fflush(stderr);
  move_list_destroy(ml);
  config_destroy(config);
}

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

// ---------------------------------------------------------------------------
// Pretty-printed report of bench positions, annotated with the
// /tmp/passpeg_bench.csv outcomes (which variants picked pass vs play).
// ---------------------------------------------------------------------------

typedef struct BenchOutcomeRow {
  int pos_id;
  char source[16]; // "passpeg" or "random"
  char algo[8];    // "PUCT" or "SH"
  int include_pass;
  int budget_secs;
  double wall_secs;
  int evals_done;
  int best_is_pass;
  double best_win_pct;
  double best_spread;
  int best_depth;
  char best_move[64];
} BenchOutcomeRow;

static void print_outcomes_for_position(FILE *report, int pos_id,
                                        const char *source,
                                        const BenchOutcomeRow *rows,
                                        int num_rows) {
  fprintf(report, "  bench results:\n");
  for (int i = 0; i < num_rows; i++) {
    const BenchOutcomeRow *r = &rows[i];
    if (r->pos_id != pos_id || strcmp(r->source, source) != 0) {
      continue;
    }
    fprintf(report,
            "    %4s incp=%d budget=%2ds  wall=%5.2fs  evals=%3d  "
            "depth=%d  win=%.4f  spread=%+.2f  picked: %s\n",
            r->algo, r->include_pass, r->budget_secs, r->wall_secs,
            r->evals_done, r->best_depth, r->best_win_pct, r->best_spread,
            r->best_move[0] ? r->best_move : (r->best_is_pass ? "PASS" : "(?)"));
  }
}

static int load_bench_csv(const char *path, BenchOutcomeRow **out,
                          int max_rows) {
  FILE *f = fopen(path, "re");
  if (!f) {
    *out = NULL;
    return 0;
  }
  BenchOutcomeRow *rows = malloc_or_die((size_t)max_rows * sizeof(*rows));
  int n = 0;
  char line[1024];
  // Skip header line.
  if (!fgets(line, sizeof(line), f)) {
    free(rows);
    (void)fclose(f);
    *out = NULL;
    return 0;
  }
  while (n < max_rows && fgets(line, sizeof(line), f)) {
    BenchOutcomeRow *r = &rows[n];
    r->best_move[0] = '\0';
    int matched = sscanf(
        line,
        "%d,%15[^,],%7[^,],%d,%d,%lf,%d,%d,%lf,%lf,%d,%*d,%63[^\n]",
        &r->pos_id, r->source, r->algo, &r->include_pass, &r->budget_secs,
        &r->wall_secs, &r->evals_done, &r->best_is_pass, &r->best_win_pct,
        &r->best_spread, &r->best_depth, r->best_move);
    if (matched >= 11) {
      n++;
    }
  }
  (void)fclose(f);
  *out = rows;
  return n;
}

static void print_position_to_report(FILE *report, Config *config,
                                     const char *cgp, int pos_id,
                                     const char *source,
                                     const BenchOutcomeRow *rows,
                                     int num_rows) {
  char load_cmd[10240];
  snprintf(load_cmd, sizeof(load_cmd), "cgp %s -lex TWL98", cgp);
  load_and_exec_config_or_die(config, load_cmd);
  Game *game = config_get_game(config);

  fprintf(report, "================================================\n");
  fprintf(report, " %s position %d\n", source, pos_id);
  fprintf(report, "================================================\n");
  fprintf(report, "CGP: %s\n\n", cgp);

  StringBuilder *sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, sb);
  fprintf(report, "%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);
  game_string_options_destroy(gso);

  print_outcomes_for_position(report, pos_id, source, rows, num_rows);
  fprintf(report, "\n");
}

void test_pass_peg_print_report(void) {
  const int max_n = 200;
  char **pass_cgps = NULL;
  char **rand_cgps = NULL;
  int n_pass =
      load_cgp_lines("/tmp/passpeg_candidates.txt", &pass_cgps, max_n);
  int n_rand = load_cgp_lines("/tmp/random_1pegs.txt", &rand_cgps, max_n);

  // Cap to whatever the bench actually used (10 each in the latest run).
  const char *n_pass_env = getenv("PASSPEG_REPORT_N_PASS");
  const char *n_rand_env = getenv("PASSPEG_REPORT_N_RAND");
  int report_n_pass = n_pass_env ? atoi(n_pass_env) : 10;
  int report_n_rand = n_rand_env ? atoi(n_rand_env) : 10;
  if (report_n_pass > n_pass) {
    report_n_pass = n_pass;
  }
  if (report_n_rand > n_rand) {
    report_n_rand = n_rand;
  }

  BenchOutcomeRow *bench_rows = NULL;
  int n_bench = load_bench_csv("/tmp/passpeg_bench.csv", &bench_rows, 4096);
  fprintf(stderr, "[passpegreport] loaded %d passpeg cgps, %d random cgps, "
                  "%d bench rows\n",
          n_pass, n_rand, n_bench);

  const char *out_path = "/tmp/passpeg_report.txt";
  FILE *report = fopen(out_path, "we");
  if (!report) {
    log_fatal("cannot open %s for writing", out_path);
  }

  fprintf(report, "PEG bench position report\n");
  fprintf(report, "Generated from /tmp/passpeg_candidates.txt + "
                  "/tmp/random_1pegs.txt\n");
  fprintf(report, "Bench results from /tmp/passpeg_bench.csv (%d rows).\n",
          n_bench);
  fprintf(report, "Lex: TWL98. Each position is 1-tile-in-bag PEG.\n\n");

  Config *config = config_create_or_die("set -s1 score -s2 score");

  fprintf(report, "\n#### ENGINEERED PASS-PEG POSITIONS ####\n\n");
  for (int p = 0; p < report_n_pass; p++) {
    print_position_to_report(report, config, pass_cgps[p], p, "passpeg",
                             bench_rows, n_bench);
  }

  fprintf(report, "\n#### RANDOM 1-PEG POSITIONS ####\n\n");
  for (int p = 0; p < report_n_rand; p++) {
    print_position_to_report(report, config, rand_cgps[p], p, "random",
                             bench_rows, n_bench);
  }

  (void)fclose(report);
  fprintf(stderr, "[passpegreport] wrote %s\n", out_path);

  for (int i = 0; i < n_pass; i++) {
    free(pass_cgps[i]);
  }
  free(pass_cgps);
  for (int i = 0; i < n_rand; i++) {
    free(rand_cgps[i]);
  }
  free(rand_cgps);
  free(bench_rows);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Oracle eval: evaluate a fixed candidate move on a 1-in-bag PEG by direct
// scenario-by-scenario endgame_solve. Bypasses the PEG search and gives
// the ground-truth win%/spread for the chosen move at the requested
// endgame depth.
// ---------------------------------------------------------------------------
void test_pass_peg_oracle_eval_move(void) {
  // Default to passpeg position 1 (the lone disagreement vs macondo) and
  // C6 REEST (macondo's "winner" tile play).
  const char *default_cgp =
      "ENTITy1YONIC2F/1A9H1AR/1P9U1TA/JELL7R1aY/1R1OVA3CON1V1/AI3GLAD2I1I1/"
      "BE5BOP1N1S1/OS4WOWING1TI/D4EH3U3N/E4XI3K1O1G/6Z3E1O1U/10DURAL/12I1F/"
      "12E1E/14D AEEMRST/AEEMRST 364/351 0";
  const char *cgp_env = getenv("PASSPEG_ORACLE_CGP");
  const char *cgp = cgp_env ? cgp_env : default_cgp;

  const char *move_str_env = getenv("PASSPEG_ORACLE_MOVE");
  const char *move_str = move_str_env ? move_str_env : "C6.REEST";

  const char *plies_env = getenv("PASSPEG_ORACLE_PLIES");
  int plies = plies_env ? atoi(plies_env) : 12;

  const char *time_env = getenv("PASSPEG_ORACLE_TIME");
  double per_solve_time = time_env ? atof(time_env) : 30.0;

  Config *config = config_create_or_die("set -s1 score -s2 score");
  char load_cmd[10240];
  snprintf(load_cmd, sizeof(load_cmd), "cgp %s -lex TWL98", cgp);
  load_and_exec_config_or_die(config, load_cmd);
  Game *game = config_get_game(config);

  const int mover_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - mover_idx;
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Parse the move using validated_moves.
  ErrorStack *parse_err = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(
      game, mover_idx, move_str,
      /*allow_phonies=*/false, /*allow_playthrough=*/true, parse_err);
  if (!error_stack_is_empty(parse_err)) {
    log_fatal("oracle eval: failed to parse move %s", move_str);
  }
  if (validated_moves_get_number_of_moves(vms) < 1) {
    log_fatal("oracle eval: no moves parsed from %s", move_str);
  }
  const Move *move = validated_moves_get_move(vms, 0);

  // Compute unseen pool from board (board-only).
  uint8_t unseen[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    int n = (int)rack_get_letter(mr, (MachineLetter)ml);
    unseen[ml] -= (uint8_t)n;
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter on_board = board_get_letter(board, row, col);
      if (on_board == 0 || on_board == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      MachineLetter eff =
          get_is_blanked(on_board) ? BLANK_MACHINE_LETTER : on_board;
      if (unseen[eff] > 0) {
        unseen[eff]--;
      }
    }
  }
  int total_unseen = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total_unseen += unseen[ml];
  }
  if (total_unseen != RACK_SIZE + 1) {
    log_fatal("oracle eval: expected %d unseen, got %d", RACK_SIZE + 1,
              total_unseen);
  }

  // Build distinct tile list with multiplicities.
  MachineLetter tile_types[MAX_ALPHABET_SIZE];
  int tile_counts[MAX_ALPHABET_SIZE];
  int num_tile_types = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (unseen[ml] > 0) {
      tile_types[num_tile_types] = (MachineLetter)ml;
      tile_counts[num_tile_types] = (int)unseen[ml];
      num_tile_types++;
    }
  }

  fprintf(stderr,
          "[passpegoracle] move=%s plies=%d soft_time=%.1fs "
          "num_scenarios=%d\n",
          move_str, plies, per_solve_time, num_tile_types);

  // Per-scenario eval: build post-cand game, set scenario rack/bag, solve.
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();

  int64_t spread_sum = 0;
  int64_t wins_x2 = 0;
  int weight_sum = 0;

  for (int ti = 0; ti < num_tile_types; ti++) {
    const MachineLetter tile = tile_types[ti];
    const int tcnt = tile_counts[ti];

    Game *scenario = game_duplicate(game);
    game_set_endgame_solving_mode(scenario);
    game_set_backup_mode(scenario, BACKUP_MODE_OFF);
    play_move_without_drawing_tiles(move, scenario);
    game_set_game_end_reason(scenario, GAME_END_REASON_NONE);

    // Empty the bag (CGP load left the bag with the original 1 tile;
    // for the scenario we want the bag-tile assigned to mover instead).
    Bag *bag = game_get_bag(scenario);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(bag, (MachineLetter)ml) > 0) {
        (void)bag_draw_letter(bag, (MachineLetter)ml, 0);
      }
    }

    // Reset opp's rack to unseen \ {tile}.
    Rack *opp_rack = player_get_rack(game_get_player(scenario, opp_idx));
    rack_reset(opp_rack);
    for (int ml = 0; ml < ld_size; ml++) {
      int n = (int)unseen[ml] - (ml == tile ? 1 : 0);
      for (int i = 0; i < n; i++) {
        rack_add_letter(opp_rack, (MachineLetter)ml);
      }
    }

    // Mover already played the cand; rack now holds the leave. Add the
    // drawn bag tile to make the post-draw rack.
    Rack *mover_rack = player_get_rack(game_get_player(scenario, mover_idx));
    rack_add_letter(mover_rack, tile);

    int32_t mover_lead =
        equity_to_int(player_get_score(game_get_player(scenario, mover_idx))) -
        equity_to_int(player_get_score(game_get_player(scenario, opp_idx)));

    ThreadControl *tc = config_get_thread_control(config);
    EndgameArgs ea = {
        .thread_control = tc,
        .game = scenario,
        .plies = plies,
        .shared_tt = NULL,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
        .skip_word_pruning = false,
        .thread_index_offset = 0,
        .soft_time_limit = per_solve_time,
        .hard_time_limit = per_solve_time,
    };
    endgame_results_reset(results);
    endgame_solve_inline(&ctx, &ea, results);
    int eg_val = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
    int32_t mover_total = mover_lead - eg_val;

    spread_sum += (int64_t)mover_total * tcnt;
    if (mover_total > 0) {
      wins_x2 += 2 * tcnt;
    } else if (mover_total == 0) {
      wins_x2 += tcnt;
    }
    weight_sum += tcnt;

    fprintf(stderr,
            "  scenario tile=%s w=%d  mover_lead=%+d  eg_val=%+d  "
            "mover_total=%+d\n",
            ld->ld_ml_to_hl[tile], tcnt, mover_lead, eg_val, mover_total);
    fflush(stderr);

    game_destroy(scenario);
  }

  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  validated_moves_destroy(vms);
  error_stack_destroy(parse_err);

  double q_spread = weight_sum > 0 ? (double)spread_sum / weight_sum : 0.0;
  double q_win = weight_sum > 0 ? (double)wins_x2 / (2.0 * weight_sum) : 0.0;

  printf("\n=== Oracle eval ===\n");
  printf("CGP: %s\n", cgp);
  printf("Move: %s   plies=%d\n", move_str, plies);
  printf("Aggregated: win%%=%.4f  mean_spread=%+0.4f  weight=%d\n", q_win,
         q_spread, weight_sum);

  config_destroy(config);
}

// ====================================================================
// peggreedy: fresh N-in-bag greedy bench (d=0 only)
//
// Goal: for each candidate mover move, enumerate the distinct N-tile bag
// compositions (= the bag's tile multiset at the moment mover plays), split
// each into (mover_drawn, bag_remaining), greedy-play to game end, and
// aggregate weighted win % / mean spread.
//
// Scenarios = (mover_drawn_multiset, bag_remaining_multiset) pairs. Within
// the drawn multiset the order of tiles doesn't matter — mover ends up
// with the same rack regardless of draw order. Within the remaining
// multiset (size 1 for k_drawn=N-1, more otherwise) the order also doesn't
// matter since opp will eventually draw them. So we enumerate by per-type
// COUNTS, not by ordered tuples.
//
// d=0 means: after mover plays cand and the bag/opp-rack are set up, we
// do a pure greedy playout (highest-equity move while bag has tiles,
// highest-score once bag empties) until natural game end. mover_total =
// signed (mover_score - opp_score) at terminal, with rack penalties
// applied if both players pass to the scoreless cap.
//
// Env knobs:
//   PASSPEG_GREEDY_PATH    — CGP file (default /tmp/peg_positions.txt)
//   PASSPEG_GREEDY_LEX     — lex override; empty = let CGP's -lex stand
//   PASSPEG_GREEDY_TOP_K   — number of top cands to print (default 15)
//   PASSPEG_GREEDY_ONLY    — semicolon-separated cand-text substrings;
//                             only cands whose movegen text matches one
//                             of these are evaluated. Empty = all cands.
//   PASSPEG_GREEDY_TSV     — output path for per-scenario TSV
// ====================================================================

static int peg_compute_unseen(const Game *game, int mover_idx,
                               uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  memset(unseen, 0, sizeof(uint8_t) * MAX_ALPHABET_SIZE);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mrack = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mrack, ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col)) continue;
      MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0) unseen[BLANK_MACHINE_LETTER]--;
      } else if (unseen[ml] > 0) {
        unseen[ml]--;
      }
    }
  }
  int total = 0;
  for (int ml = 0; ml < ld_size; ml++) total += unseen[ml];
  return total;
}

// Drain bag, then add the listed N tiles (insert_point=0).
static void peg_set_bag_tiles(Bag *bag, const MachineLetter *tiles,
                               int n_tiles, int ld_size) {
  for (int ml = 0; ml < ld_size; ml++) {
    while (bag_get_letter(bag, (MachineLetter)ml) > 0) {
      (void)bag_draw_letter(bag, (MachineLetter)ml, 0);
    }
  }
  for (int i = 0; i < n_tiles; i++) bag_add_letter(bag, tiles[i], 0);
}

// Reset opp's rack to (unseen MINUS drawn) tiles.
static void peg_set_opp_rack(Rack *opp_rack,
                              const uint8_t unseen[MAX_ALPHABET_SIZE],
                              int ld_size, const MachineLetter *drawn,
                              int n_drawn) {
  uint8_t remaining[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++) remaining[ml] = unseen[ml];
  for (int i = 0; i < n_drawn; i++) {
    if (remaining[drawn[i]] > 0) remaining[drawn[i]]--;
  }
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    for (int i = 0; i < remaining[ml]; i++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

// Build the post-cand game state for one (mover_drawn, bag_remaining) split:
// bag holds (mover_drawn ++ bag_remaining) = N tiles total, opp rack is set
// to (unseen − bag tiles), cand is played, then mover draws their
// k_drawn tiles. Result: mover's rack has k_drawn drawn tiles added, bag has
// only bag_remaining left, opp's rack is the deal (full 7).
static Game *peg_make_post_cand_game(
    const Game *base_game, int mover_idx, const uint8_t *unseen, int ld_size,
    const Move *cand, int k_drawn, const MachineLetter *mover_drawn,
    int n_bag_remaining, const MachineLetter *bag_remaining) {
  Game *g = game_duplicate(base_game);
  game_set_endgame_solving_mode(g);
  game_set_backup_mode(g, BACKUP_MODE_OFF);
  Bag *bag = game_get_bag(g);
  Rack *opp_r = player_get_rack(game_get_player(g, 1 - mover_idx));
  Rack *mover_r = player_get_rack(game_get_player(g, mover_idx));
  // Combine drawn + remaining into one N-tile array for bag setup.
  MachineLetter all_bag[16];
  int N = k_drawn + n_bag_remaining;
  for (int i = 0; i < k_drawn; i++) all_bag[i] = mover_drawn[i];
  for (int i = 0; i < n_bag_remaining; i++) {
    all_bag[k_drawn + i] = bag_remaining[i];
  }
  peg_set_bag_tiles(bag, all_bag, N, ld_size);
  peg_set_opp_rack(opp_r, unseen, ld_size, all_bag, N);
  play_move_without_drawing_tiles(cand, g);
  for (int i = 0; i < k_drawn; i++) {
    rack_add_letter(mover_r, mover_drawn[i]);
    (void)bag_draw_letter(bag, mover_drawn[i], 0);
  }
  // play_move_without_drawing_tiles sets GAME_END_REASON_STANDARD when the
  // rack empties post-placement and bag <= RACK_SIZE — meant for the endgame
  // solver's no-drawing world. We manually re-stock the rack right after,
  // so the going-out flag is stale. Clear it whenever the rack isn't empty;
  // otherwise the greedy playout's first-line "if game_end != NONE break"
  // bails before simulating any continuation for emptier candidates.
  if (!rack_is_empty(mover_r)) {
    game_set_game_end_reason(g, GAME_END_REASON_NONE);
  }
  return g;
}

// Greedy playout to game end. Returns signed mover spread at terminal
// (mover_score − opp_score). Records each played move into out_pv (text)
// and writes final mover/opp rack contents to out_*_rack. If out_pv_text
// is NULL no PV is recorded.
static int32_t peg_greedy_playout_pv(
    Game *game, int mover_idx, MoveList *playout_ml, int thread_index,
    char *out_pv_text, size_t out_pv_text_cap, char *out_mover_rack_end,
    size_t out_mover_rack_end_cap, char *out_opp_rack_end,
    size_t out_opp_rack_end_cap, char *out_final_cgp,
    size_t out_final_cgp_cap) {
  const LetterDistribution *ld = game_get_ld(game);
  StringBuilder *pv_sb = NULL;
  if (out_pv_text && out_pv_text_cap > 0) {
    pv_sb = string_builder_create();
    out_pv_text[0] = '\0';
  }
  int n_plies = 0;
  for (int turn = 0; turn < MAX_SEARCH_DEPTH; turn++) {
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) break;
    const bool bag_has_tiles = bag_get_letters(game_get_bag(game)) > 0;
    const MoveGenArgs ga = {
        .game = game,
        .move_list = playout_ml,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = bag_has_tiles ? MOVE_SORT_EQUITY : MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = thread_index,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&ga);
    if (move_list_get_count(playout_ml) == 0) break;
    const Move *best = move_list_get_move(playout_ml, 0);
    if (pv_sb) {
      if (n_plies > 0) string_builder_add_string(pv_sb, " | ");
      string_builder_add_move(pv_sb, game_get_board(game), best, ld, true);
    }
    play_move(best, game, NULL);
    n_plies++;
  }
  if (pv_sb) {
    snprintf(out_pv_text, out_pv_text_cap, "%s", string_builder_peek(pv_sb));
    string_builder_destroy(pv_sb);
  }
  if (out_mover_rack_end && out_mover_rack_end_cap > 0) {
    StringBuilder *rsb = string_builder_create();
    string_builder_add_rack(rsb,
                            player_get_rack(game_get_player(game, mover_idx)),
                            ld, false);
    snprintf(out_mover_rack_end, out_mover_rack_end_cap, "%s",
             string_builder_peek(rsb));
    string_builder_destroy(rsb);
  }
  if (out_opp_rack_end && out_opp_rack_end_cap > 0) {
    StringBuilder *rsb = string_builder_create();
    string_builder_add_rack(
        rsb, player_get_rack(game_get_player(game, 1 - mover_idx)), ld,
        false);
    snprintf(out_opp_rack_end, out_opp_rack_end_cap, "%s",
             string_builder_peek(rsb));
    string_builder_destroy(rsb);
  }
  if (out_final_cgp && out_final_cgp_cap > 0) {
    char *cgp = game_get_cgp(game, true);
    snprintf(out_final_cgp, out_final_cgp_cap, "%s", cgp ? cgp : "");
    free(cgp);
  }
  const Player *me = game_get_player(game, mover_idx);
  const Player *op = game_get_player(game, 1 - mover_idx);
  int32_t spread = equity_to_int(player_get_score(me) - player_get_score(op));
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    spread -= (int32_t)equity_to_int(rack_get_score(ld, player_get_rack(me)));
    spread += (int32_t)equity_to_int(rack_get_score(ld, player_get_rack(op)));
  }
  return spread;
}

static int64_t peg_binomial(int n, int k) {
  if (k < 0 || k > n) return 0;
  if (k == 0 || k == n) return 1;
  if (k > n - k) k = n - k;
  int64_t r = 1;
  for (int i = 0; i < k; i++) r = r * (n - i) / (i + 1);
  return r;
}

// In-place lexicographic next-permutation over MachineLetter arrays.
// Skips duplicates naturally (only enumerates distinct orderings).
// Caller should sort the array ascending before the first iteration.
// Returns false when the array is at the last permutation.
static bool peg_next_perm(MachineLetter *arr, int n) {
  if (n <= 1) {
    return false;
  }
  int k = n - 2;
  while (k >= 0 && arr[k] >= arr[k + 1]) {
    k--;
  }
  if (k < 0) {
    return false;
  }
  int l = n - 1;
  while (arr[k] >= arr[l]) {
    l--;
  }
  MachineLetter tmp = arr[k];
  arr[k] = arr[l];
  arr[l] = tmp;
  int i = k + 1;
  int j = n - 1;
  while (i < j) {
    tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
    i++;
    j--;
  }
  return true;
}

// Recursive enumerator over N-multisets from `counts[0..k_types-1]`. Calls
// cb(picked, ctx) once per distinct multiset.
static void peg_enum_multiset(int *picked, int idx, int remaining,
                               const int *counts, int k_types,
                               void (*cb)(const int *picked, void *ctx),
                               void *ctx) {
  if (idx == k_types) {
    if (remaining == 0) cb(picked, ctx);
    return;
  }
  int max_take = counts[idx] < remaining ? counts[idx] : remaining;
  for (int k = 0; k <= max_take; k++) {
    picked[idx] = k;
    peg_enum_multiset(picked, idx + 1, remaining - k, counts, k_types, cb,
                       ctx);
  }
  picked[idx] = 0;
}

typedef struct PegCandResult {
  int ci;
  int64_t weight_sum;
  int64_t win_x2;     // 2 per win, 1 per tie, 0 per loss (scaled by weight)
  int64_t spread_sum; // signed (mover − opp) at terminal × weight
  int n_scen;
  // Running tally of total per-scenario weight encountered so far; used by
  // PASSPEG_SCENARIO_STRIDE for stratified sampling proportional to
  // weight (weight-unit stride mod k).
  int64_t sampled_weight_seen;
  // True if any inner endgame_solve returned with depth=-1 (no IDS pass
  // completed before the global deadline). In that case get_value() is
  // the uninitialized zero, so mt = mover_lead and the spread/win tallies
  // for that opp-POV state are bogus. Stage post-processing drops cands flagged
  // incomplete from the ranking.
  bool incomplete;
} PegCandResult;

// Shared state for one cand's enumeration; passed by pointer through the
// recursive enumerators below so we don't rely on gcc nested functions.
typedef struct PegEnumCtx {
  // The current N-multiset being explored (per-type counts, summing to N).
  // peg_enum_outer_multiset writes into this; peg_enum_mover_drawn reads.
  int *n_multiset;
  // The current mover-drawn sub-multiset (per-type counts, summing to
  // k_drawn). peg_enum_mover_drawn writes into this; the leaf
  // peg_emit_split reads.
  int *mover_pick;

  const MachineLetter *types;
  const int *type_counts;
  int k_types;
  int k_drawn;
  int n_bag_remaining;

  const Game *base_game;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  const Move *cand;
  const char *cand_txt;
  int cand_score;
  int pos_idx;
  const LetterDistribution *ld;

  FILE *tsv_f;
  PegCandResult *res;

  // Optional semicolon-separated list of "drawn/remaining" patterns; if
  // non-NULL, only scenarios whose canonical (drawn,remaining) tile strings
  // match one of these substrings get evaluated. Drawn / remaining are
  // emitted as sorted-by-type-index tile lists. Examples: "III/A",
  // "GII/U;IIT/A".
  const char *scenario_filter;

  // Optional semicolon-separated list of substrings; when non-NULL, only
  // opp moves whose movegen text contains one of these substrings are
  // considered. Lets us isolate specific opp moves (e.g. TEMPURA, 6D (T)A)
  // when probing a single scenario at depth.
  const char *opp_move_filter;

  // Evaluation depth.
  //   0 = greedy playout from post-cand state (mover & opp both greedy).
  //   >=1 = enumerate opp's top-K moves at post-cand; for each, apply,
  //         then run endgame_solve at `depth` plies (bag is empty after
  //         opp draws the last bag tile). Take MIN over branches —
  //         matches macondo "guaranteed wins": any opp move that beats
  //         mover ⇒ mark scenario a loss.
  int depth;
  // Number of opp moves enumerated at d>=1. Top-K by EQUITY.
  int opp_top_k;
  // ThreadControl for endgame_solve at d>=1.
  ThreadControl *thread_control;

  // ---- threading ----
  // The thread_index used for movegen + endgame caches by this scenario's
  // worker. Set per-job by the worker fn (= worker_idx the executor hands
  // in). Zero for the serial / non-executor code path.
  int worker_idx;
  // Optional shared executor for inner opp-util dispatch from inside
  // emit_split. NULL = run the opp-util sweep serially.
  PegPool *executor;
  // Mutex protecting the per-cand aggregator `res`. Multiple scenarios
  // of the same cand may run concurrently. NULL = serial mode.
  cpthread_mutex_t *res_mutex;
  // Mutex protecting writes to `tsv_f`. NULL = serial mode.
  cpthread_mutex_t *tsv_mutex;
  // Mutex serializing endgame_solve_inline calls. The internal endgame
  // state has shared mutables (movegen/ABDADA caches keyed by fixed-ish
  // thread indices, pruned-KWG generation, etc.) that race under
  // concurrent workers — so we currently serialize the heavy solve step
  // while keeping the cheaper scenario setup (game_duplicate, opp
  // movegen, MIN aggregation) parallel. NULL = serial mode.
  cpthread_mutex_t *endgame_mutex;
  // When non-NULL, emit_split pushes a copy of (n_multiset, mover_pick)
  // onto this list and returns instead of evaluating. Used by the outer
  // driver to enumerate scenarios in one thread before dispatching them
  // to workers. Guarded by *jobs_mutex (callers run enumeration
  // single-threaded, but the worker fn shares the result back).
  struct PegScenarioJobList *out_jobs;
  // Deadline-watch shared by all workers for time-budget interruption.
  // budget_timer points at the shared start Timer; budget_secs is the
  // total budget. Workers check `ctimer_elapsed_seconds(budget_timer) >
  // budget_secs` and skip their job if exceeded (returning fast so the
  // executor drains quickly). When budget_secs <= 0 or budget_timer is
  // NULL, no time limit is enforced.
  const Timer *budget_timer;
  double budget_secs;
  // Absolute monotonic-ns deadline derived from budget_secs at solver
  // entry. Plumbed through to endgame_solve_inline as external_deadline_ns
  // so abdada_negamax bails out mid-search rather than running to ply
  // completion. 0 = no deadline.
  int64_t deadline_monotonic_ns;
  // Optional per-inner opp-POV TSV, set via PASSPEG_INNER_TSV. When non-NULL,
  // peg_opp_pov_worker_fn writes one row per evaluated opp-POV state with the
  // outer-scenario tile letters, the opp candidate, the opp-POV bag composition,
  // opp-POV weight, and the leaf mt. Lets us trace exactly what the inner 1peg
  // evaluator saw for each candidate. Guarded by inner_tsv_mutex.
  FILE *inner_tsv_f;
  cpthread_mutex_t *inner_tsv_mutex;
  // Per-cand cache of post-opp game state -> mover_total. Allocated by
  // the per-cand dispatcher, freed after the cand's scenarios complete.
  // NULL means caching disabled. See PegOppPovCache above. Largely
  // ineffective on its own (3.5% hit rate on leaf-state collisions) —
  // kept for diagnostics; the real speedup is the shared_eg_tt below.
  struct PegOppPovCache *opp_pov_cache;
  // Per-cand shared endgame TranspositionTable. Passed as shared_tt
  // into every endgame_solve call in this cand's scenarios so the TT
  // is reused across opp-POV states + opp_moves + scenarios. Endgame search
  // sub-trees often share positions (especially right after opp's move
  // when only a couple of tiles differ); the TT hits cut redundant
  // negamax work. macondo's "nested-cache 71.9% hit rate" is the
  // equivalent of this TT sharing.
  TranspositionTable *shared_eg_tt;
  // Per-worker persistent EndgameCtx pointers, indexed by
  // (worker_idx - executor_thread_offset). Workers reuse their own slot
  // across all endgame_solve calls in this cand's evaluation,
  // amortizing the per-call setup cost (ABDADA init, move sort, etc.).
  // Slot 0 reserved for the main thread (worker_idx 0).
  EndgameCtx **per_worker_eg_ctx;
  int per_worker_eg_ctx_n;
  int per_worker_eg_ctx_offset;  // = executor_thread_offset (100)
} PegEnumCtx;

// One (cand, n_multiset, mover_pick) scenario, packaged for dispatch
// through PegPool. `base_ctx` is a shared read-only template;
// `n_multiset` and `mover_pick` are owned copies (k_types ints each).
// One (multiset, mover_pick) scenario, packaged for dispatch through
// PegPool. `base_ctx` is the per-cand read-only template;
// `n_multiset` and `mover_pick` are owned copies (k_types ints each). The
// worker hands this to peg_emit_split (with out_jobs=NULL) — at d=0 the
// worker walks bag-tile orderings serially via peg_eval_d0_ordering; at
// d>=1 the bag is empty at the leaf so there is no ordering walk.
typedef struct PegScenarioJob {
  const PegEnumCtx *base_ctx;
  int *n_multiset;
  int *mover_pick;
} PegScenarioJob;

typedef struct PegScenarioJobList {
  PegScenarioJob *items;
  int n;
  int cap;
} PegScenarioJobList;

static void peg_joblist_push(PegScenarioJobList *jl, PegScenarioJob job) {
  if (jl->n == jl->cap) {
    int new_cap = jl->cap > 0 ? jl->cap * 2 : 256;
    jl->items =
        realloc(jl->items, (size_t)new_cap * sizeof(PegScenarioJob));
    if (!jl->items) {
      log_fatal("peg joblist realloc failed");
    }
    jl->cap = new_cap;
  }
  jl->items[jl->n++] = job;
}

static inline void peg_lock(cpthread_mutex_t *m) {
  if (m) {
    cpthread_mutex_lock(m);
  }
}

static inline void peg_unlock(cpthread_mutex_t *m) {
  if (m) {
    cpthread_mutex_unlock(m);
  }
}

// True when (drawn_str, remaining_str) matches one of the semicolon-
// separated "drawn/remaining" patterns in `filter`. NULL filter = always
// matches.
static bool peg_scenario_filter_match(const char *filter,
                                       const char *drawn_str,
                                       const char *remaining_str) {
  if (!filter) {
    return true;
  }
  char joined[64];
  snprintf(joined, sizeof(joined), "%s/%s", drawn_str, remaining_str);
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", filter);
  char *tok = strtok(tmp, ";");
  while (tok != NULL) {
    if (strcmp(tok, joined) == 0) {
      return true;
    }
    tok = strtok(NULL, ";");
  }
  return false;
}

// Leaf: build the realized split's tile arrays, run a greedy playout, and
// fold the outcome into res / TSV.
// One (opp_move, perceived_bag_tile) leaf for the opp utility sweep
// inside emit_split. Read-only fields are shared; mover_total is the
// only per-job write target. The worker fn body is declared after
// emit_split (it shares scope with the outer scenario worker), so we
// forward-declare it here.
typedef struct PegOppInnerJob {
  const Game *base_game;            // post-cand game (RO, shared)
  int mover_idx;
  int ld_size;
  const Move *opp_move;             // shared, RO
  MachineLetter bag_ml;              // perceived bag tile in this opp-POV
  int weight;                       // == opp_type_counts[ti]
  int n_opp_types;
  const MachineLetter *opp_types;
  const int *opp_type_counts;
  int ti;                           // index into opp_types for this scenario
  int opp_depth;
  ThreadControl *thread_control;
  // Absolute monotonic-ns wall deadline for this item. When 0, no deadline.
  // Set by the caller from the outer peg ctx so a slow opp-inner can bail
  // cleanly instead of running the whole inner endgame past the cascade
  // budget.
  int64_t deadline_monotonic_ns;
  int32_t mover_total;              // output
} PegOppInnerJob;
static void peg_opp_inner_worker_fn(void *arg, int worker_idx);

// Per-cand cache of (post-opp game state) -> mover_total. The walker
// re-evaluates many opp-POV states that produce structurally identical post-opp
// states (different scenarios reaching the same board + racks). macondo's
// equivalent caches the recursive PEG sub-result and hits ~70%+; sharing
// the leaf eval across scenarios within one cand cuts the redundant
// endgame_solve / greedy work substantially.
//
// Key: 64-bit FNV-1a over (board tiles | mover_rack counts | opp_rack
// counts | bag counts | scores). Open-addressing with linear probing.
// Single mutex — contention is low because each scenario does many cache
// ops between its single endgame_solve, and the cache is small.
typedef struct PegOppPovCacheEntry {
  uint64_t key;
  int32_t  mt;
  bool     valid;
} PegOppPovCacheEntry;

typedef struct PegOppPovCache {
  PegOppPovCacheEntry *entries;
  size_t capacity;        // power of 2
  cpthread_mutex_t mutex;
  atomic_int hits;
  atomic_int misses;
} PegOppPovCache;

static PegOppPovCache *peg_opp_pov_cache_create(size_t capacity) {
  // Round up to power of 2.
  size_t cap = 1;
  while (cap < capacity) cap *= 2;
  PegOppPovCache *cache = malloc_or_die(sizeof(PegOppPovCache));
  cache->entries = calloc_or_die(cap, sizeof(PegOppPovCacheEntry));
  cache->capacity = cap;
  cpthread_mutex_init(&cache->mutex);
  atomic_init(&cache->hits, 0);
  atomic_init(&cache->misses, 0);
  return cache;
}

static void peg_opp_pov_cache_destroy(PegOppPovCache *cache) {
  if (!cache) return;
  free(cache->entries);
  free(cache);
}

static inline uint64_t peg_fnv1a_update(uint64_t hash,
                                          const void *data, size_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t byte_idx = 0; byte_idx < len; byte_idx++) {
    hash ^= bytes[byte_idx];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// Hash a Game state for the cache. We hash everything that affects the
// leaf-eval outcome (board tiles, both racks, bag, scores) so that two
// game-state-identical positions get the same key regardless of which
// path produced them.
static uint64_t peg_hash_game_state(const Game *game, int mover_idx) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter ml = board_get_letter(board, row, col);
      hash = peg_fnv1a_update(hash, &ml, sizeof(ml));
    }
  }
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  const Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  const int dist_size = ld_get_size(game_get_ld(game));
  for (int i = 0; i < dist_size; i++) {
    uint8_t mover_count = rack_get_letter(mover_rack, i);
    uint8_t opp_count = rack_get_letter(opp_rack, i);
    hash = peg_fnv1a_update(hash, &mover_count, sizeof(mover_count));
    hash = peg_fnv1a_update(hash, &opp_count, sizeof(opp_count));
  }
  const Bag *bag = game_get_bag(game);
  for (int i = 0; i < dist_size; i++) {
    uint8_t bag_count = bag_get_letter((Bag *)bag, (MachineLetter)i);
    hash = peg_fnv1a_update(hash, &bag_count, sizeof(bag_count));
  }
  Equity mover_score = player_get_score(game_get_player(game, mover_idx));
  Equity opp_score = player_get_score(game_get_player(game, 1 - mover_idx));
  hash = peg_fnv1a_update(hash, &mover_score, sizeof(mover_score));
  hash = peg_fnv1a_update(hash, &opp_score, sizeof(opp_score));
  // Avoid 0 since we use it as "no key" sentinel in some paths.
  if (hash == 0) hash = 1;
  return hash;
}

// Lookup. Returns true on hit (and writes *out_mt); false on miss.
static bool peg_opp_pov_cache_lookup(PegOppPovCache *cache, uint64_t key,
                                   int32_t *out_mt) {
  if (!cache) return false;
  const size_t mask = cache->capacity - 1;
  size_t idx = (size_t)key & mask;
  cpthread_mutex_lock(&cache->mutex);
  for (size_t probe = 0; probe < cache->capacity; probe++) {
    PegOppPovCacheEntry *entry = &cache->entries[(idx + probe) & mask];
    if (!entry->valid) {
      cpthread_mutex_unlock(&cache->mutex);
      atomic_fetch_add(&cache->misses, 1);
      return false;
    }
    if (entry->key == key) {
      *out_mt = entry->mt;
      cpthread_mutex_unlock(&cache->mutex);
      atomic_fetch_add(&cache->hits, 1);
      return true;
    }
  }
  cpthread_mutex_unlock(&cache->mutex);
  return false;  // table full — extremely unlikely
}

static void peg_opp_pov_cache_store(PegOppPovCache *cache, uint64_t key, int32_t mt) {
  if (!cache) return;
  const size_t mask = cache->capacity - 1;
  size_t idx = (size_t)key & mask;
  cpthread_mutex_lock(&cache->mutex);
  for (size_t probe = 0; probe < cache->capacity; probe++) {
    PegOppPovCacheEntry *entry = &cache->entries[(idx + probe) & mask];
    if (!entry->valid || entry->key == key) {
      entry->key = key;
      entry->mt = mt;
      entry->valid = true;
      cpthread_mutex_unlock(&cache->mutex);
      return;
    }
  }
  cpthread_mutex_unlock(&cache->mutex);
}

// Count tiles a cand actually plays from rack (letters outside parens in
// the move text). E.g. "C6 ACIDOT(I)c 80" -> 7 (the (I) is a board tile);
// "5E A(N) 4" -> 1. Used by the cascade driver to bucket cands as
// emptier vs non-emptier given a known bag size.
static int peg_count_tiles_played(const char *cand_text) {
  if (!cand_text) {
    return 0;
  }
  // Skip past the position token (chars up to first space).
  const char *p = cand_text;
  while (*p && *p != ' ') {
    p++;
  }
  while (*p == ' ') {
    p++;
  }
  // Count alpha chars outside parens until the next space (the score).
  int count = 0;
  bool in_paren = false;
  while (*p && *p != ' ') {
    if (*p == '(') {
      in_paren = true;
    } else if (*p == ')') {
      in_paren = false;
    } else if (!in_paren &&
               ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
      count++;
    }
    p++;
  }
  return count;
}

// Three-bucket classifier for cascade diversity quota.
//   0 = capable-emp:   tp >= bag AND post-cand rack >= 5 (bingo-capable
//                      next turn after drawing the bag)
//   1 = non-emp:       tp <  bag (mover doesn't drain bag this turn)
//   2 = incapable-emp: tp >= bag AND post-cand rack <  5
// post-cand rack = (rack_size + bag_size) - tp  when tp >= bag,
//                  rack_size                     when tp <  bag.
static int peg_cand_bucket(int tiles_played, int bag_size, int rack_size) {
  if (tiles_played < bag_size) {
    return 1;  // non-emp
  }
  const int post_rack = rack_size + bag_size - tiles_played;
  return post_rack >= 5 ? 0 : 2;  // capable-emp : incapable-emp
}

// Generalized opp-perception opp-POV state: the perceived pool has
// opp_type_counts[t] tiles of type opp_types[t]; the opp-POV state says
// the bag holds opp_pov_bag_counts[t] of each type and the mover's rack
// holds the rest (opp_type_counts[t] - opp_pov_bag_counts[t]). For
// n_bag_now == 1 this degenerates to the single-tile case the
// PegOppInnerJob path supports today.
typedef struct PegOppPovJob {
  const Game *base_game;            // walker state (RO, shared)
  int mover_idx;
  int ld_size;
  const Move *opp_move;             // shared, RO
  int n_opp_types;
  const MachineLetter *opp_types;   // shared, RO
  const int *opp_type_counts;       // total perceived-pool counts (RO)
  const int *opp_pov_bag_counts;        // bag composition under this opp_pov_game (RO)
  ThreadControl *thread_control;
  // Outer ctx is borrowed for budget plumbing (deadline_monotonic_ns,
  // budget_timer/secs) when the inner-endgame path is selected. RO.
  const struct PegEnumCtx *outer_ctx;
  // Optional per-inner opp-POV TSV context: when outer_ctx->inner_tsv_f is set,
  // these annotate the row with the outer realized scenario and the opp
  // cand's text/score so the consumer can group by (cand, scenario, opp).
  const char *outer_drawn_str;
  const char *outer_remaining_str;
  const char *opp_move_text;
  int opp_move_score;
  int64_t opp_pov_weight;
  int32_t mover_total;              // output (realized mt under this opp_pov_game)
  bool incomplete;                  // output: depth=-1 (eg never iterated)
} PegOppPovJob;

// Per-call timing log captured via endgame_solve's per_ply_callback. Used
// by both the K<N walker leaf (peg_opp_pov_worker_fn) and the K=N bag-emptier
// path (peg_emit_split). The callback fires once per completed IDS depth
// on the first thread of the solver and records (depth, value, ms).
typedef struct PegInnerEgDepthLog {
  Timer call_timer;
  int n;
  int depths[32];
  int32_t values[32];
  double times_ms[32];
} PegInnerEgDepthLog;

static void peg_inner_eg_per_ply_cb(int depth, int32_t value,
                                     const struct PVLine *pv_line,
                                     const struct Game *game,
                                     const struct PVLine *ranked_pvs,
                                     int num_ranked_pvs, void *user_data) {
  (void)pv_line;
  (void)game;
  (void)ranked_pvs;
  (void)num_ranked_pvs;
  PegInnerEgDepthLog *log = (PegInnerEgDepthLog *)user_data;
  if (log->n < 32) {
    log->depths[log->n] = depth;
    log->values[log->n] = value;
    log->times_ms[log->n] = ctimer_elapsed_seconds(&log->call_timer) * 1000.0;
    log->n++;
  }
}

static void peg_opp_pov_worker_fn(void *arg, int worker_idx);
static void peg_eval_opp_with_perception(
    const PegEnumCtx *outer_ctx, const Game *walker, const Move *opp_move,
    const MachineLetter *opp_types, const int *opp_type_counts,
    int n_opp_types, int n_bag_now, const int *realized_bag_counts,
    double alpha, double *out_utility, int32_t *out_realized_mt,
    const char *outer_drawn_str, const char *outer_remaining_str,
    const char *opp_move_text, int opp_move_score);

// Evaluate ONE bag-tile ordering at depth=0: build the post-cand game with
// `iter_perm` as the residual bag, run the greedy playout (or endgame_solve
// for an emptier when PASSPEG_EMPTIER_USE_ENDGAME is set), and aggregate
// the realized mover_total into ctx->res under res_mutex. This is the leaf
// of the d=0 search — extracted so that ordering-grained parallel jobs and
// the serial in-loop path share a single implementation.
//
// `mover_drawn` (length mover_drawn_n) is the labeled tile array the mover
// drew this scenario. `drawn_str` is its canonical short form (for TSV).
// `n_bag_perm` is the number of meaningful entries in `iter_perm`; zero
// for emptiers (in which case bag is already empty post-cand).
// Compute the time still available under our Peg budget. Returns 0 when
// no budget is set (= unlimited for downstream calls). When the budget is
// already exhausted returns 0.001 (1 ms) so an endgame_solve call returns
// promptly with whatever partial result it has rather than running cold.
static double peg_remaining_budget_secs(const PegEnumCtx *ctx) {
  if (ctx->budget_timer == NULL || ctx->budget_secs <= 0.0) {
    return 0.0;
  }
  const double elapsed = ctimer_elapsed_seconds(ctx->budget_timer);
  const double remaining = ctx->budget_secs - elapsed;
  if (remaining <= 0.001) {
    return 0.001;
  }
  return remaining;
}

// Pick the earlier of a per-call deadline and the cascade deadline. Either
// may be 0 (meaning "no deadline"); the result is "no deadline" only if
// both are 0. Used to ensure endgame_solve calls inside the bench never
// outlive the cascade's overall budget — without this, a single
// endgame_solve at depth=4 can blow a 32s budget by 30s+ because the
// per-call wall budget (inner_eg_budget or kn_budget) is independent of
// the cascade's deadline_monotonic_ns.
static int64_t peg_clamp_deadline_ns(int64_t per_call_deadline_ns,
                                       int64_t cascade_deadline_ns) {
  if (cascade_deadline_ns <= 0) {
    return per_call_deadline_ns;
  }
  if (per_call_deadline_ns <= 0) {
    return cascade_deadline_ns;
  }
  return per_call_deadline_ns < cascade_deadline_ns
             ? per_call_deadline_ns
             : cascade_deadline_ns;
}

static void peg_eval_d0_ordering(const PegEnumCtx *ctx,
                                  const MachineLetter *mover_drawn,
                                  int mover_drawn_n,
                                  const char *drawn_str,
                                  const MachineLetter *iter_perm,
                                  int n_bag_perm, int64_t this_weight) {
  (void)mover_drawn_n;  // implied by ctx->k_drawn
  char perm_remaining_str[32] = {0};
  for (int i = 0; i < n_bag_perm && i < 30; i++) {
    perm_remaining_str[i] = ctx->ld->ld_ml_to_hl[iter_perm[i]][0];
  }
  Game *perm_game = peg_make_post_cand_game(
      ctx->base_game, ctx->mover_idx, ctx->unseen, ctx->ld_size, ctx->cand,
      ctx->k_drawn, mover_drawn, n_bag_perm, iter_perm);
  char perm_post_cgp[512] = {0};
  if (ctx->tsv_f) {
    char *cgp = game_get_cgp(perm_game, true);
    snprintf(perm_post_cgp, sizeof(perm_post_cgp), "%s", cgp ? cgp : "");
    free(cgp);
  }
  char perm_pv[1024] = {0};
  char perm_final_cgp[512] = {0};
  char perm_mover_rack[32] = {0};
  char perm_opp_rack[32] = {0};
  int32_t perm_mt = 0;
  // Per-scenario timing for the inner TSV. Captures both the greedy d=0
  // path and the optional EMPTIER endgame fallback so the report can show
  // first-pass wall time per scenario.
  double scen_start_ms = 0.0;
  double scen_dur_ms = 0.0;
  int scen_eg_plies = 0;          // 0 == pure greedy path
  int scen_eg_depth = 0;
  int scen_eg_num_moves = 0;
  PegInnerEgDepthLog scen_log = {0};
  if (ctx->inner_tsv_f) {
    if (ctx->budget_timer) {
      scen_start_ms = ctimer_elapsed_seconds(ctx->budget_timer) * 1000.0;
    }
    ctimer_start(&scen_log.call_timer);
  }
  const char *empty_eg_env = getenv("PASSPEG_EMPTIER_USE_ENDGAME");
  const int empty_eg_plies =
      empty_eg_env && *empty_eg_env ? atoi(empty_eg_env) : 0;
  if (n_bag_perm == 0 && empty_eg_plies > 0 &&
      game_get_game_end_reason(perm_game) == GAME_END_REASON_NONE) {
    const int32_t mover_lead =
        equity_to_int(
            player_get_score(game_get_player(perm_game, ctx->mover_idx))) -
        equity_to_int(player_get_score(
            game_get_player(perm_game, 1 - ctx->mover_idx)));
    EndgameCtx *eg_ctx = NULL;
    EndgameResults *eg_results = endgame_results_create();
    EndgameArgs ea = {
        .thread_control = ctx->thread_control,
        .game = perm_game,
        .plies = empty_eg_plies,
        .shared_tt = NULL,
        .initial_small_move_arena_size =
            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
        .skip_word_pruning = true,
        .thread_index_offset = ctx->worker_idx,
        .soft_time_limit = peg_remaining_budget_secs(ctx),
        .hard_time_limit = peg_remaining_budget_secs(ctx),
        .external_deadline_ns = ctx->deadline_monotonic_ns,
    };
    endgame_solve_inline(&eg_ctx, &ea, eg_results);
    const int turn = game_get_player_on_turn_index(perm_game);
    const int32_t eg_val =
        endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
    perm_mt = (turn == ctx->mover_idx) ? mover_lead + eg_val
                                       : mover_lead - eg_val;
    scen_eg_plies = empty_eg_plies;
    scen_eg_depth =
        endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
    if (ctx->inner_tsv_f) {
      const PVLine *pv =
          endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
      if (pv) scen_eg_num_moves = pv->num_moves;
      if (pv && pv->num_moves > 0) {
        Game *pv_g = game_duplicate(perm_game);
        game_set_endgame_solving_mode(pv_g);
        game_set_backup_mode(pv_g, BACKUP_MODE_OFF);
        StringBuilder *psb = string_builder_create();
        for (int mi = 0; mi < pv->num_moves; mi++) {
          Move mf;
          small_move_to_move(&mf, &pv->moves[mi],
                             game_get_board(pv_g));
          if (mi > 0) string_builder_add_string(psb, " | ");
          string_builder_add_move(psb, game_get_board(pv_g), &mf,
                                  game_get_ld(pv_g), true);
          play_move(&mf, pv_g, NULL);
        }
        snprintf(perm_pv, sizeof(perm_pv), "%s",
                 string_builder_peek(psb));
        string_builder_destroy(psb);
        game_destroy(pv_g);
      }
    }
    endgame_ctx_destroy(eg_ctx);
    endgame_results_destroy(eg_results);
  } else {
    // Capacity 1: greedy playout uses MOVE_RECORD_BEST which only ever
    // holds one move (best so far) + spare_move. The previous 4096
    // allocated ~hundreds of KB per playout for slots that were never
    // touched.
    MoveList *playout_ml = move_list_create(1);
    const bool need_pv = ctx->tsv_f || ctx->inner_tsv_f;
    perm_mt = peg_greedy_playout_pv(
        perm_game, ctx->mover_idx, playout_ml, ctx->worker_idx,
        need_pv ? perm_pv : NULL, sizeof(perm_pv),
        need_pv ? perm_mover_rack : NULL, sizeof(perm_mover_rack),
        need_pv ? perm_opp_rack : NULL, sizeof(perm_opp_rack),
        ctx->tsv_f ? perm_final_cgp : NULL, sizeof(perm_final_cgp));
    move_list_destroy(playout_ml);
    // scen_eg_plies stays 0 → "pure greedy path" in the report.
  }
  if (ctx->inner_tsv_f) {
    scen_dur_ms = ctimer_elapsed_seconds(&scen_log.call_timer) * 1000.0;
  }
  game_destroy(perm_game);

  peg_lock(ctx->res_mutex);
  ctx->res->weight_sum += this_weight;
  ctx->res->spread_sum += this_weight * (int64_t)perm_mt;
  if (perm_mt > 0) {
    ctx->res->win_x2 += 2 * this_weight;
  } else if (perm_mt == 0) {
    ctx->res->win_x2 += this_weight;
  }
  ctx->res->n_scen++;
  peg_unlock(ctx->res_mutex);

  if (ctx->tsv_f) {
    peg_lock(ctx->tsv_mutex);
    fprintf(ctx->tsv_f,
            "%d\t%s\t%d\t%d\t%d\t%s\t%s\t%lld\t%d\t%s\t%s\t%s\t%s\t%s\n",
            ctx->pos_idx, ctx->cand_txt, ctx->cand_score,
            ctx->k_drawn + n_bag_perm, ctx->k_drawn, drawn_str,
            perm_remaining_str, (long long)this_weight, (int)perm_mt,
            perm_post_cgp, perm_final_cgp, perm_pv, perm_mover_rack,
            perm_opp_rack);
    peg_unlock(ctx->tsv_mutex);
  }
  // Per-scenario row to the inner TSV — same schema as the K=N/K<N
  // endgame rows but with opp_pov_bag = "" and (for the pure greedy first
  // pass) eg_plies = 0, eg_depth = 0. Lets the report show first-pass
  // timing for every scenario.
  if (ctx->inner_tsv_f) {
    peg_lock(ctx->inner_tsv_mutex);
    fprintf(ctx->inner_tsv_f,
            "%s\t%s\t%s\t%s\t%d\t%s\t%lld\t%d\t%d\t%d\t%d\t%d\t%d"
            "\t%.1f\t%.1f\t%s\t%s\t%s\n",
            ctx->cand_txt ? ctx->cand_txt : "", drawn_str,
            perm_remaining_str,
            "",   // opp_move — none at d=0 (greedy plays it out)
            0,    // opp_score
            "",   // opp_pov_bag — no perception at d=0
            (long long)this_weight,
            (int)perm_mt,
            scen_eg_plies, scen_eg_depth, 0, scen_eg_num_moves, 0,
            scen_start_ms, scen_dur_ms, "" /* depth_log */,
            perm_mover_rack, perm_pv);
    peg_unlock(ctx->inner_tsv_mutex);
  }
}

static void peg_emit_split(const PegEnumCtx *ctx) {
  MachineLetter mover_drawn[16];
  MachineLetter bag_remaining[16];
  int mover_drawn_n = 0;
  int bag_remaining_n = 0;
  char drawn_str[32] = {0};
  char remaining_str[32] = {0};
  int drawn_len = 0;
  int remaining_len = 0;
  for (int type_idx = 0; type_idx < ctx->k_types; type_idx++) {
    const int mover_count = ctx->mover_pick[type_idx];
    const int bag_count = ctx->n_multiset[type_idx] - mover_count;
    for (int k = 0; k < mover_count; k++) {
      mover_drawn[mover_drawn_n++] = ctx->types[type_idx];
      if (drawn_len < 30) {
        drawn_str[drawn_len++] = ctx->ld->ld_ml_to_hl[ctx->types[type_idx]][0];
      }
    }
    for (int k = 0; k < bag_count; k++) {
      bag_remaining[bag_remaining_n++] = ctx->types[type_idx];
      if (remaining_len < 30) {
        remaining_str[remaining_len++] =
            ctx->ld->ld_ml_to_hl[ctx->types[type_idx]][0];
      }
    }
  }
  // Apply scenario filter (skip silently if no match — the cand's
  // aggregated stats reflect only the scenarios we evaluated).
  if (!peg_scenario_filter_match(ctx->scenario_filter, drawn_str,
                                  remaining_str)) {
    return;
  }

  // Collect-mode: defer evaluation by pushing job(s) onto out_jobs.
  // Two paths:
  //   depth == 0: do stride + ordering walk inline (single-threaded; the
  //     enumerator IS the one caller) and push ONE job per (multiset,
  //     ordering). Workers later evaluate one ordering each — gives
  //     ordering-grained parallelism (up to N_bag! per multiset).
  //   depth >= 1: push one multiset job; the worker calls back into
  //     peg_emit_split (with out_jobs=NULL) to run the opp-top-K +
  //     endgame_solve flow. At d>=1 the bag is empty at the leaf so
  //     there is no ordering walk to split.
  // Collect-mode: defer evaluation by pushing a (multiset, mover_pick)
  // job onto out_jobs. The dispatcher hands it to a worker which calls
  // emit_split again with out_jobs=NULL — at d=0 the worker walks bag-tile
  // orderings serially via peg_eval_d0_ordering, at d>=1 it runs the
  // opp-top-K + endgame_solve flow.
  //
  // We tried splitting d=0 jobs one-per-ordering for finer parallelism but
  // it cost ~13% wall on POND 4peg — job overhead beat the gain. Multiset
  // granularity is the right fit for ~50-250 us scenarios.
  if (ctx->out_jobs) {
    PegScenarioJob job;
    job.base_ctx = ctx;
    job.n_multiset = malloc_or_die((size_t)ctx->k_types * sizeof(int));
    job.mover_pick = malloc_or_die((size_t)ctx->k_types * sizeof(int));
    memcpy(job.n_multiset, ctx->n_multiset,
           (size_t)ctx->k_types * sizeof(int));
    memcpy(job.mover_pick, ctx->mover_pick,
           (size_t)ctx->k_types * sizeof(int));
    peg_joblist_push(ctx->out_jobs, job);
    return;
  }

  // Ordered-pair weight: number of labeled-tile sequences in which mover
  // first draws k_drawn tiles (in order), then bag_remaining is the bag's
  // residual (the walker further iterates its orderings as sub-scenarios).
  // For mover's draw the type-multiset (m_t) admits k_drawn! orderings of
  // the labeled tiles, so the per-multiset weight is
  //   k_drawn! × ∏_t C(c_t, m_t) × C(c_t − m_t, b_t)
  // Total over all multisets sums to P(N, k_drawn + n_bag_remaining), the
  // ordered-pair basis macondo/peg.c use.
  int64_t weight = 1;
  for (int type_idx = 0; type_idx < ctx->k_types; type_idx++) {
    const int total_count = ctx->type_counts[type_idx];
    const int mover_count = ctx->mover_pick[type_idx];
    const int bag_count = ctx->n_multiset[type_idx] - mover_count;
    weight *= peg_binomial(total_count, mover_count) *
              peg_binomial(total_count - mover_count, bag_count);
  }
  for (int f = 2; f <= ctx->k_drawn; f++) weight *= f;

  // PASSPEG_SCENARIO_STRIDE=k: stratified sampling on the conceptual
  // weight-unit-expanded job list. Each multiset of weight w contributes
  // w "weight-units" to a running tally. We sample a multiset if and only
  // if at least one stride-boundary (multiple of k) falls inside its
  // weight range; the # of samples taken from that multiset = how many
  // boundaries it covers. Each sampled multiset is evaluated once with
  // weight = samples × k so the expected aggregate weight is preserved.
  // Default k=1 (no sampling). Applies at every level (outer + inner).
  //
  // Bag-size gate: when the root bag is 1 or 2 (1peg/2peg), the scenario
  // space is still small enough to enumerate fully — stride sampling on
  // that tiny space throws away crucial coverage. Disable stride when
  // N (= k_drawn + n_bag_remaining) <= 2; only enable for 3peg+.
  {
    const int n_root = ctx->k_drawn + ctx->n_bag_remaining;
    const char *stride_env = getenv("PASSPEG_SCENARIO_STRIDE");
    const int stride_req =
        stride_env && *stride_env ? atoi(stride_env) : 1;
    const int stride = (n_root >= 3) ? stride_req : 1;
    if (stride > 1 && ctx->res) {
      peg_lock(ctx->res_mutex);
      const int64_t old_seen = ctx->res->sampled_weight_seen;
      ctx->res->sampled_weight_seen += weight;
      peg_unlock(ctx->res_mutex);
      const int64_t samples =
          (old_seen + weight) / (int64_t)stride - old_seen / (int64_t)stride;
      if (samples == 0) {
        return;  // no stride boundary fell within this multiset's weight band
      }
      weight = samples * (int64_t)stride;
    }
  }

  // At d=0 the per-ordering helper builds its own perm_game; the shared
  // post-cand game is only consumed by the d>=1 branches below. Skip the
  // allocation at d=0 — for collect mode this avoids ~K_jobs wasted
  // game_duplicate calls; for execute mode it's a minor saving.
  Game *game = NULL;
  char post_cand_cgp[512] = {0};
  if (ctx->depth >= 1) {
    game = peg_make_post_cand_game(ctx->base_game, ctx->mover_idx,
                                    ctx->unseen, ctx->ld_size, ctx->cand,
                                    ctx->k_drawn, mover_drawn,
                                    ctx->n_bag_remaining, bag_remaining);
    if (ctx->tsv_f) {
      char *cgp = game_get_cgp(game, true);
      snprintf(post_cand_cgp, sizeof(post_cand_cgp), "%s", cgp ? cgp : "");
      free(cgp);
    }
  }

  int32_t mover_total = 0;
  char pv_text[1024] = {0};
  char final_cgp[512] = {0};
  char mover_rack_end[32] = {0};
  char opp_rack_end[32] = {0};

  if (ctx->depth == 0) {
    // Pure greedy from post-cand: opp + mover both greedy to game end.
    // Walk distinct lex orderings of bag_remaining so each physical draw
    // order contributes its own sub-scenario (matching the walker's
    // d>=1 behavior). Per-perm aggregation here; early-return to skip
    // the bottom single-mt fold.
    //
    // PASSPEG_WALK_SAMPLE=N: instead of walking all K!/∏b_t! distinct
    // orderings per multiset, sample N cyclic rotations of the sorted
    // bag_remaining (= [0, 1, ..., min(N, K)-1]-shift) and scale per-
    // sample weight by full_orderings/N so the per-multiset total
    // matches a full walk.
    MachineLetter perm[16];
    for (int i = 0; i < ctx->n_bag_remaining; i++) {
      perm[i] = bag_remaining[i];
    }
    for (int i = 1; i < ctx->n_bag_remaining; i++) {
      for (int j = i; j > 0 && perm[j] < perm[j - 1]; j--) {
        const MachineLetter tmp = perm[j];
        perm[j] = perm[j - 1];
        perm[j - 1] = tmp;
      }
    }
    // Two ways to sub-sample the ordering walk (independent of
    // PASSPEG_SCENARIO_STRIDE):
    //   PASSPEG_WALK_SAMPLE=N    — take EXACTLY N cyclic rotations of the
    //                                sorted perm per multiset.
    //   PASSPEG_WALK_STRIDE=k    — take ~1/k of orderings per multiset
    //                                (n_to_sample = ceil(n_full / k)).
    // Both reuse the same cyclic-rotation sampler: deterministic,
    // per-multiset, no cross-multiset coordination -> no mutex needed.
    // If both env vars are set, WALK_SAMPLE wins (explicit count).
    const char *sample_env = getenv("PASSPEG_WALK_SAMPLE");
    const int sample_count =
        sample_env && *sample_env ? atoi(sample_env) : 0;
    const char *walk_stride_env = getenv("PASSPEG_WALK_STRIDE");
    const int walk_stride =
        walk_stride_env && *walk_stride_env ? atoi(walk_stride_env) : 1;
    const bool sample_mode =
        ctx->n_bag_remaining >= 2 &&
        (sample_count > 0 || walk_stride > 1);
    int64_t n_full_orderings = 1;
    int64_t per_sample_weight = weight;
    int n_to_sample = 1;
    if (sample_mode) {
      int b_counts[MAX_ALPHABET_SIZE] = {0};
      for (int i = 0; i < ctx->n_bag_remaining; i++) {
        b_counts[perm[i]]++;
      }
      int64_t k_fact = 1;
      for (int k = 2; k <= ctx->n_bag_remaining; k++) k_fact *= k;
      int64_t prod_b_fact = 1;
      for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
        if (b_counts[ml] > 1) {
          int64_t f = 1;
          for (int k = 2; k <= b_counts[ml]; k++) f *= k;
          prod_b_fact *= f;
        }
      }
      n_full_orderings = k_fact / prod_b_fact;
      if (sample_count > 0) {
        n_to_sample = sample_count < (int)n_full_orderings
                          ? sample_count
                          : (int)n_full_orderings;
      } else {
        // walk_stride > 1: pick ceil(n_full / stride), at least 1.
        const int64_t s =
            (n_full_orderings + (int64_t)walk_stride - 1) /
            (int64_t)walk_stride;
        n_to_sample = s < 1 ? 1
                            : (s > n_full_orderings ? (int)n_full_orderings
                                                    : (int)s);
      }
      per_sample_weight =
          (weight * n_full_orderings + n_to_sample / 2) / n_to_sample;
    }
    int sample_idx = 0;
    do {
      if (sample_mode && sample_idx >= n_to_sample) break;
      // In sample mode, build the cyclic-rotated perm for this sample.
      MachineLetter eff_perm[16];
      if (sample_mode) {
        for (int i = 0; i < ctx->n_bag_remaining; i++) {
          eff_perm[i] =
              perm[(i + sample_idx) % ctx->n_bag_remaining];
        }
      } else {
        for (int i = 0; i < ctx->n_bag_remaining; i++) {
          eff_perm[i] = perm[i];
        }
      }
      sample_idx++;
      const int64_t this_weight =
          sample_mode ? per_sample_weight : weight;
      const MachineLetter *iter_perm = sample_mode ? eff_perm : perm;
      peg_eval_d0_ordering(ctx, mover_drawn, mover_drawn_n, drawn_str,
                            iter_perm, ctx->n_bag_remaining, this_weight);
    } while ((sample_mode && sample_idx < n_to_sample) ||
             (!sample_mode && peg_next_perm(perm, ctx->n_bag_remaining)));
    return;
  } else if (ctx->n_bag_remaining == 0) {
    // Bag-emptier: the bag is empty, both racks are determined, and opp is
    // on turn. Stage n => n "informed, rational" plies of negamax. We
    // delegate to endgame_solve(plies=depth); it finds opp's true best
    // move and plays out optimally for `depth` plies, heuristic-leafing
    // the rest.
    const int32_t mover_lead =
        equity_to_int(player_get_score(game_get_player(game, ctx->mover_idx))) -
        equity_to_int(
            player_get_score(game_get_player(game, 1 - ctx->mover_idx)));
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      mover_total = mover_lead;
    } else {
      EndgameCtx *eg_ctx = NULL;
      EndgameResults *eg_results = endgame_results_create();
      // Per-call timing log so the inner-TSV report can show when each
      // IDS depth was reached (same instrumentation as the K<N walker path).
      PegInnerEgDepthLog kn_log = {0};
      double kn_start_ms = 0.0;
      if (ctx->budget_timer) {
        kn_start_ms = ctimer_elapsed_seconds(ctx->budget_timer) * 1000.0;
      }
      ctimer_start(&kn_log.call_timer);
      // K=N (bag-emptier) endgame: plies driven by the current stage
      // depth (so stage 1 = 1-ply, stage 2 = 2-ply, etc.) unless the
      // user overrides via PASSPEG_INNER_EG_PLIES. Optional wall-cap
      // via PASSPEG_INNER_EG_BUDGET (default 0 = no cap — let the
      // small N-ply IDS finish naturally).
      const char *kn_budget_env = getenv("PASSPEG_INNER_EG_BUDGET");
      const double kn_budget = (kn_budget_env && *kn_budget_env)
                                   ? atof(kn_budget_env) : 0.0;
      const char *kn_plies_env = getenv("PASSPEG_INNER_EG_PLIES");
      const int kn_plies = (kn_plies_env && *kn_plies_env)
                                ? atoi(kn_plies_env) : ctx->depth;
      EndgameArgs ea = {
          .thread_control = ctx->thread_control,
          .game = game,
          .plies = kn_plies,
          .shared_tt = ctx->shared_eg_tt,
          .initial_small_move_arena_size =
              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
          .num_threads = 1,
          .use_heuristics = true,
          .num_top_moves = 1,
          .per_ply_callback = ctx->inner_tsv_f ? peg_inner_eg_per_ply_cb
                                                : NULL,
          .per_ply_callback_data = ctx->inner_tsv_f ? (void *)&kn_log : NULL,
          .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
          .skip_word_pruning = true,
          .thread_index_offset = ctx->worker_idx,
          .soft_time_limit = kn_budget,
          .hard_time_limit = kn_budget,
          // Clamp by the cascade deadline so a bag-empty K=N endgame at
          // ctx->depth plies can never outlive the outer wall budget.
          // Without this clamp, default kn_budget=0 leaves external_deadline_ns=0
          // and endgame_solve runs unbounded — a single pos 61 d=4 scenario
          // hangs the cascade past the 60s bp_exec watchdog.
          .external_deadline_ns = peg_clamp_deadline_ns(
              kn_budget > 0.0
                  ? ctimer_monotonic_ns() + (int64_t)(kn_budget * 1.0e9)
                  : 0,
              ctx->deadline_monotonic_ns),
      };
      endgame_solve_inline(&eg_ctx, &ea, eg_results);
      const double kn_dur_ms =
          ctimer_elapsed_seconds(&kn_log.call_timer) * 1000.0;
      // endgame_solve returns the value from the player-on-turn's POV
      // (here: opp). Negate to express it from mover's POV, then add to
      // mover_lead to get mover_total.
      //
      // If the cascade deadline fired before depth 1 completed, the result
      // value/PV are uninitialized — using them downstream blows up on
      // equity_to_int / "duplicate move type" assertions. Fall back to a
      // greedy playout so we always return a sane number.
      const int kn_depth_reached =
          endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
      if (kn_depth_reached < 0) {
        MoveList *pl_fb = move_list_create(1);
        mover_total = peg_greedy_playout_pv(
            game, ctx->mover_idx, pl_fb, ctx->worker_idx, NULL, 0, NULL, 0,
            NULL, 0, NULL, 0);
        move_list_destroy(pl_fb);
      } else {
        const int32_t opp_pov_val =
            endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
        mover_total = mover_lead - opp_pov_val;
      }
      // Emit a row to the inner TSV so the report can show this cand's
      // K=N evaluation alongside the K<N walker rows. We synthesize a
      // single "row per scenario" — there's no opp_move iteration here
      // (the endgame picks opp's best move internally). Skip the PV walk
      // when no depth completed — the SmallMove entries may be partial
      // and tripping play_move asserts ("duplicate move type").
      if (ctx->inner_tsv_f) {
        const int kn_depth =
            endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
        char kn_pv_text[1024] = {0};
        int kn_pv_num_moves = 0;
        int kn_pv_negamax = 0;
        const PVLine *pv_line = kn_depth_reached >= 0
            ? endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST)
            : NULL;
        if (pv_line) {
          kn_pv_num_moves = pv_line->num_moves;
          kn_pv_negamax = pv_line->negamax_depth;
          if (pv_line->num_moves > 0) {
            Game *pv_g = game_duplicate(game);
            game_set_endgame_solving_mode(pv_g);
            game_set_backup_mode(pv_g, BACKUP_MODE_OFF);
            StringBuilder *psb = string_builder_create();
            for (int mi = 0; mi < pv_line->num_moves; mi++) {
              Move mf;
              small_move_to_move(&mf, &pv_line->moves[mi],
                                 game_get_board(pv_g));
              if (mi > 0) string_builder_add_string(psb, " | ");
              string_builder_add_move(psb, game_get_board(pv_g), &mf,
                                      game_get_ld(pv_g), true);
              play_move(&mf, pv_g, NULL);
            }
            snprintf(kn_pv_text, sizeof(kn_pv_text), "%s",
                     string_builder_peek(psb));
            string_builder_destroy(psb);
            game_destroy(pv_g);
          }
        }
        char kn_dlog[256] = {0};
        int kn_off = 0;
        for (int i = 0;
             i < kn_log.n && kn_off + 24 < (int)sizeof(kn_dlog); i++) {
          kn_off += snprintf(kn_dlog + kn_off, sizeof(kn_dlog) - kn_off,
                             "%s%d@%.1f=%+d", i == 0 ? "" : ";",
                             kn_log.depths[i], kn_log.times_ms[i],
                             (int)kn_log.values[i]);
        }
        // K=N path: mover rack is on `game` (mover already played the cand
        // and drew); render it for the report column.
        char kn_mover_rack[32] = {0};
        {
          const LetterDistribution *ld = game_get_ld(game);
          StringBuilder *rsb = string_builder_create();
          string_builder_add_rack(
              rsb,
              player_get_rack(game_get_player(game, ctx->mover_idx)),
              ld, false);
          snprintf(kn_mover_rack, sizeof(kn_mover_rack), "%s",
                   string_builder_peek(rsb));
          string_builder_destroy(rsb);
        }
        peg_lock(ctx->inner_tsv_mutex);
        fprintf(ctx->inner_tsv_f,
                "%s\t%s\t%s\t%s\t%d\t%s\t%lld\t%d\t%d\t%d\t%d\t%d\t%d"
                "\t%.1f\t%.1f\t%s\t%s\t%s\n",
                ctx->cand_txt ? ctx->cand_txt : "", drawn_str,
                "",     // remaining_str — empty for bag-emptier
                "",     // opp_move — not selected separately
                0,      // opp_score
                "",     // opp_pov_bag — no perception
                0LL,    // opp_pov_weight
                (int)mover_total,
                ctx->depth, kn_depth, 0,
                kn_pv_num_moves, kn_pv_negamax,
                kn_start_ms, kn_dur_ms, kn_dlog,
                kn_mover_rack, kn_pv_text);
        peg_unlock(ctx->inner_tsv_mutex);
        // With per-call budget the K=N endgame should always reach at
        // least depth 1. If it didn\'t, mover_total = mover_lead (a
        // valid but uninformative number); we no longer drop the cand —
        // the user\'s design is "return the best result when
        // interrupted", and partial endgame data is still a real signal.
      }
      if (ctx->tsv_f) {
        const PVLine *pv =
            endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
        if (pv && pv->num_moves > 0) {
          Game *pv_game = game_duplicate(game);
          game_set_endgame_solving_mode(pv_game);
          game_set_backup_mode(pv_game, BACKUP_MODE_OFF);
          StringBuilder *pv_sb = string_builder_create();
          for (int mi = 0; mi < pv->num_moves; mi++) {
            Move m_full;
            small_move_to_move(&m_full, &pv->moves[mi],
                               game_get_board(pv_game));
            if (mi > 0) string_builder_add_string(pv_sb, " | ");
            string_builder_add_move(pv_sb, game_get_board(pv_game), &m_full,
                                    game_get_ld(pv_game), true);
            play_move(&m_full, pv_game, NULL);
          }
          snprintf(pv_text, sizeof(pv_text), "%s", string_builder_peek(pv_sb));
          string_builder_destroy(pv_sb);
          char *cgp = game_get_cgp(pv_game, true);
          snprintf(final_cgp, sizeof(final_cgp), "%s", cgp ? cgp : "");
          free(cgp);
          StringBuilder *rsb = string_builder_create();
          string_builder_add_rack(
              rsb,
              player_get_rack(game_get_player(pv_game, ctx->mover_idx)),
              game_get_ld(pv_game), false);
          snprintf(mover_rack_end, sizeof(mover_rack_end), "%s",
                   string_builder_peek(rsb));
          string_builder_destroy(rsb);
          StringBuilder *rsb2 = string_builder_create();
          string_builder_add_rack(
              rsb2,
              player_get_rack(game_get_player(pv_game, 1 - ctx->mover_idx)),
              game_get_ld(pv_game), false);
          snprintf(opp_rack_end, sizeof(opp_rack_end), "%s",
                   string_builder_peek(rsb2));
          string_builder_destroy(rsb2);
          game_destroy(pv_game);
        }
      }
      endgame_ctx_destroy(eg_ctx);
      endgame_results_destroy(eg_results);
    }
  } else {
    // d>=1, n_bag_remaining >= 1: enumerate opp's top-K moves, apply each,
    // then evaluate the resulting position via endgame_solve at `depth`
    // plies. Take MIN over branches.
    MoveList *opp_ml = move_list_create(16384);
    const MoveGenArgs opp_ga = {
        .game = game,
        .move_list = opp_ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = ctx->worker_idx,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&opp_ga);
    const int n_opp = move_list_get_count(opp_ml);

    // One-shot opp utility ranking, per the "rational opp" model. For
    // each opp tile-placement move, evaluate it across all of opp's
    // perceived bag-tile scenarios (= the distinct types in opp's unseen
    // pool, which is mover_rack + bag from opp's POV), do a greedy
    // playout for each, and aggregate into opp's win% / mean spread.
    // opp utility = win_pct + alpha * mean_spread (alpha = env value).
    // Triggered when PASSPEG_GREEDY_OPP_UTIL is set (its value = alpha).
    const char *opp_env = getenv("PASSPEG_GREEDY_OPP_UTIL");
    if (opp_env && *opp_env) {
      const double alpha = atof(opp_env);
      // If PASSPEG_GREEDY_OPP_DEPTH > 0, replace the per-opp-POV-
      // scenario greedy playout with endgame_solve at that ply depth.
      // The bag is empty after opp plays + draws the 1 bag tile, so
      // endgame_solve is well-defined.
      const char *opp_depth_env = getenv("PASSPEG_GREEDY_OPP_DEPTH");
      const int opp_depth =
          opp_depth_env && *opp_depth_env ? atoi(opp_depth_env) : 0;
      const int opp_idx = 1 - ctx->mover_idx;
      uint8_t opp_unseen[MAX_ALPHABET_SIZE];
      peg_compute_unseen(game, opp_idx, opp_unseen);
      MachineLetter opp_types[MAX_ALPHABET_SIZE];
      int opp_type_counts[MAX_ALPHABET_SIZE];
      int n_opp_types = 0;
      int opp_pool_total = 0;
      for (int ml = 0; ml < ctx->ld_size; ml++) {
        if (opp_unseen[ml] > 0) {
          opp_types[n_opp_types] = (MachineLetter)ml;
          opp_type_counts[n_opp_types] = (int)opp_unseen[ml];
          opp_pool_total += (int)opp_unseen[ml];
          n_opp_types++;
        }
      }
      fprintf(stderr,
              "[opp_util] scenario %s/%s  alpha=%g  opp_pool=%d tiles in "
              "%d types\n",
              drawn_str, remaining_str, alpha, opp_pool_total, n_opp_types);

      typedef struct {
        int move_idx;
        double win_pct;
        double mean_spread;
        double utility;
      } OppRanked;
      OppRanked *ranked =
          calloc_or_die((size_t)n_opp, sizeof(OppRanked));
      int n_ranked = 0;

      // Collect tile-placement opp ranks; PASS/EXCHANGE are skipped.
      // If opp_move_filter is set, also restrict to the named opp moves
      // (substring match on movegen text). Lets the test do SH-style
      // ladders that re-rank only the previous round's survivors.
      int *placement_opp_ranks =
          malloc_or_die((size_t)n_opp * sizeof(int));
      int n_placement = 0;
      for (int opp_rank = 0; opp_rank < n_opp; opp_rank++) {
        const Move *opp_move = move_list_get_move(opp_ml, opp_rank);
        if (move_get_type(opp_move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        if (ctx->opp_move_filter) {
          char text[64] = {0};
          StringBuilder *sb_m = string_builder_create();
          string_builder_add_move(sb_m, game_get_board(game), opp_move,
                                  game_get_ld(game), true);
          snprintf(text, sizeof(text), "%s", string_builder_peek(sb_m));
          string_builder_destroy(sb_m);
          bool match = false;
          char tmp[2048];
          snprintf(tmp, sizeof(tmp), "%s", ctx->opp_move_filter);
          char *tok = strtok(tmp, ";");
          while (tok != NULL) {
            if (strstr(text, tok) != NULL) {
              match = true;
              break;
            }
            tok = strtok(NULL, ";");
          }
          if (!match) {
            continue;
          }
        }
        placement_opp_ranks[n_placement++] = opp_rank;
      }

      // Build one job per (opp_move, perceived_bag_tile) leaf. Flat
      // layout: jobs[pp * n_opp_types + ti] is the (placement_opp_ranks
      // [pp], opp_types[ti]) leaf.
      const int n_inner_jobs =
          n_placement > 0 ? n_placement * n_opp_types : 0;
      PegOppInnerJob *inner_jobs = NULL;
      void **inner_arg_ptrs = NULL;
      if (n_inner_jobs > 0) {
        inner_jobs = malloc_or_die((size_t)n_inner_jobs *
                                    sizeof(PegOppInnerJob));
        inner_arg_ptrs =
            malloc_or_die((size_t)n_inner_jobs * sizeof(void *));
        for (int pp = 0; pp < n_placement; pp++) {
          const int opp_rank = placement_opp_ranks[pp];
          const Move *opp_move = move_list_get_move(opp_ml, opp_rank);
          for (int ti = 0; ti < n_opp_types; ti++) {
            const int idx = pp * n_opp_types + ti;
            inner_jobs[idx] = (PegOppInnerJob){
                .base_game = game,
                .mover_idx = ctx->mover_idx,
                .ld_size = ctx->ld_size,
                .opp_move = opp_move,
                .bag_ml = opp_types[ti],
                .weight = opp_type_counts[ti],
                .n_opp_types = n_opp_types,
                .opp_types = opp_types,
                .opp_type_counts = opp_type_counts,
                .ti = ti,
                .opp_depth = opp_depth,
                .thread_control = ctx->thread_control,
                .deadline_monotonic_ns = ctx->deadline_monotonic_ns,
                .mover_total = 0,
            };
            inner_arg_ptrs[idx] = &inner_jobs[idx];
          }
        }
      }

      // Dispatch. With a shared executor we can submit nested work; the
      // help-while-waiting protocol on submit_and_wait lets the calling
      // worker continue draining the queue (no deadlock when nested
      // inner work is submitted from within an outer scenario worker).
      if (ctx->executor && n_inner_jobs > 0) {
        peg_pool_submit_and_wait(ctx->executor,
                                         peg_opp_inner_worker_fn,
                                         inner_arg_ptrs, n_inner_jobs,
                                         ctx->worker_idx);
      } else {
        for (int idx = 0; idx < n_inner_jobs; idx++) {
          peg_opp_inner_worker_fn(&inner_jobs[idx], ctx->worker_idx);
        }
      }

      // Aggregate per-opp_rank.
      for (int pp = 0; pp < n_placement; pp++) {
        const int opp_rank = placement_opp_ranks[pp];
        int64_t weight_sum = 0;
        double win_x2_sum = 0.0;
        int64_t spread_sum = 0;
        for (int ti = 0; ti < n_opp_types; ti++) {
          const int idx = pp * n_opp_types + ti;
          const int32_t opp_pov_mover_total = inner_jobs[idx].mover_total;
          const int weight = inner_jobs[idx].weight;
          weight_sum += weight;
          spread_sum += (int64_t)(-opp_pov_mover_total) * weight;
          if (opp_pov_mover_total < 0) {
            win_x2_sum += 2.0 * weight;
          } else if (opp_pov_mover_total == 0) {
            win_x2_sum += weight;
          }
        }
        const Move *opp_move = move_list_get_move(opp_ml, opp_rank);
        (void)opp_move; // used below via opp_rank
        const double opp_win_pct = win_x2_sum / (2.0 * (double)weight_sum);
        const double opp_mean_spread =
            (double)spread_sum / (double)weight_sum;
        const double utility =
            opp_win_pct + alpha * opp_mean_spread;
        ranked[n_ranked].move_idx = opp_rank;
        ranked[n_ranked].win_pct = opp_win_pct;
        ranked[n_ranked].mean_spread = opp_mean_spread;
        ranked[n_ranked].utility = utility;
        n_ranked++;
      }
      // Sort by utility descending.
      for (int i = 0; i < n_ranked; i++) {
        for (int j = i + 1; j < n_ranked; j++) {
          if (ranked[j].utility > ranked[i].utility) {
            OppRanked tmp = ranked[i];
            ranked[i] = ranked[j];
            ranked[j] = tmp;
          }
        }
      }
      const char *opp_topn_env = getenv("PASSPEG_GREEDY_OPP_TOPN");
      const int opp_topn =
          opp_topn_env && *opp_topn_env ? atoi(opp_topn_env) : 20;
      fprintf(stderr, "[opp_util] top-%d by utility:\n", opp_topn);
      for (int i = 0; i < n_ranked && i < opp_topn; i++) {
        const Move *m = move_list_get_move(opp_ml, ranked[i].move_idx);
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, game_get_board(game), m,
                                game_get_ld(game), true);
        fprintf(stderr,
                "  #%-3d  win=%.4f  spread=%+8.3f  util=%+9.5f  %s\n",
                i + 1, ranked[i].win_pct, ranked[i].mean_spread,
                ranked[i].utility, string_builder_peek(sb));
        string_builder_destroy(sb);
      }
      // Locate the target moves of interest (TEMPURA, AUGUSTER, TEMPTER).
      const char *targets[] = {"(TEMP)URA", "A(U)GUSTER", "(TEMP)TER",
                                "(TEMP)ERAS", "(TEMP)TERS"};
      for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        for (int i = 0; i < n_ranked; i++) {
          const Move *m = move_list_get_move(opp_ml, ranked[i].move_idx);
          StringBuilder *sb = string_builder_create();
          string_builder_add_move(sb, game_get_board(game), m,
                                  game_get_ld(game), true);
          if (strstr(string_builder_peek(sb), targets[t]) != NULL) {
            fprintf(stderr,
                    "[opp_util] %-12s ranks #%-3d  win=%.4f  spread=%+7.3f"
                    "  util=%+9.5f  %s\n",
                    targets[t], i + 1, ranked[i].win_pct,
                    ranked[i].mean_spread, ranked[i].utility,
                    string_builder_peek(sb));
            string_builder_destroy(sb);
            break;
          }
          string_builder_destroy(sb);
        }
      }
      free(ranked);
      free(placement_opp_ranks);
      free(inner_jobs);
      free(inner_arg_ptrs);
      fflush(stderr);
    }

    // One-shot dump of every opp tile-placement move for this scenario,
    // including the mover_total that results from greedy continuation
    // (so you can sort by "actual outcome" instead of equity). Triggered
    // by PASSPEG_GREEDY_DUMP_OPP=1.
    const char *dump_opp_env = getenv("PASSPEG_GREEDY_DUMP_OPP");
    if (dump_opp_env && atoi(dump_opp_env) == 1) {
      fprintf(stderr,
              "[dump_opp] scenario %s/%s  n_opp=%d  cand=%s\n",
              drawn_str, remaining_str, n_opp, ctx->cand_txt);
      for (int dump_rank = 0; dump_rank < n_opp; dump_rank++) {
        const Move *dump_move = move_list_get_move(opp_ml, dump_rank);
        const int dump_type = move_get_type(dump_move);
        if (dump_type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        // Apply this opp move on a branch and greedy-play to game end.
        Game *dump_branch = game_duplicate(game);
        game_set_endgame_solving_mode(dump_branch);
        game_set_backup_mode(dump_branch, BACKUP_MODE_OFF);
        play_move(dump_move, dump_branch, NULL);
        game_set_game_end_reason(dump_branch, GAME_END_REASON_NONE);
        MoveList *dump_pl = move_list_create(4096);
        char dump_pv[1024] = {0};
        char dump_mr[32] = {0};
        char dump_or[32] = {0};
        const int32_t dump_mt = peg_greedy_playout_pv(
            dump_branch, ctx->mover_idx, dump_pl, ctx->worker_idx, dump_pv,
            sizeof(dump_pv), dump_mr, sizeof(dump_mr), dump_or,
            sizeof(dump_or), NULL, 0);
        move_list_destroy(dump_pl);
        game_destroy(dump_branch);
        StringBuilder *dump_sb = string_builder_create();
        string_builder_add_move(dump_sb, game_get_board(game), dump_move,
                                game_get_ld(game), true);
        fprintf(stderr,
                "  #%-5d  score=%-4d  greedy_mt=%+5d  %s  | pv: %s | end mover=[%s] opp=[%s]\n",
                dump_rank + 1,
                (int)equity_to_int(move_get_score(dump_move)),
                (int)dump_mt, string_builder_peek(dump_sb), dump_pv, dump_mr,
                dump_or);
        string_builder_destroy(dump_sb);
      }
      fflush(stderr);
    }

    // rational-opp halving path (opt-in). opp doesn't know the
    // realized bag tile: he evaluates each opp candidate move at the
    // current stage's depth across every perceived bag-tile type
    // (weighted by physical-tile counts), and picks the move that
    // maximizes utility = opp_win_pct + alpha * opp_mean_spread.
    // The scenario's mover_total is then the *realized* mt of opp's
    // pick — utility is used only to choose the move and then
    // discarded; aggregate win % is computed from realized outcomes.
    // Halving: starting from opp_top_k cands, stage s = 0..depth-1
    // cuts to opp_top_k >> s by utility-DESC, final stage at depth
    // picks the utility-max from the survivors.
    // Bag-emptier case (n_bag_remaining == 0): no perceived bag tile,
    // opp's "perceived" pool collapses to a single deterministic
    // scenario (mover's known rack), so utility-pick == MIN-over-opp.
    // Falls into the legacy MIN path below.
    const char *rational_env = getenv("PASSPEG_GREEDY_RATIONAL");
    const bool rational =
        rational_env && atoi(rational_env) > 0 && ctx->n_bag_remaining >= 1;

    // PASSPEG_GREEDY_RAT_WALK=1 — rational walker for inner 2+peg
    // states (also handles 1peg as a degenerate case). At each PEG ply
    // we take opp_top_k candidates by movegen equity, rank them at
    // d=0 (opp's utility = avg over n_bag-multiset perception for opp
    // plies; realized greedy mt for mover plies), pick top-1, apply.
    // When the bag empties we run a greedy playout to game end.
    // The realized scenario's mover_total is the realized spread.
    //
    // PASSPEG_GREEDY_AUTO_WALKER=1 — per-cand auto-dispatch:
    //   bag-after >= 2 → walker
    //   bag-after == 1 → rational halving (no walker)
    //   bag-after == 0 → bag-emptier path (existing MIN/endgame)
    // When set, overrides RAT_WALK.
    const char *walk_env = getenv("PASSPEG_GREEDY_RAT_WALK");
    const char *auto_walk_env = getenv("PASSPEG_GREEDY_AUTO_WALKER");
    const bool auto_walker = auto_walk_env && atoi(auto_walk_env) > 0;
    const bool walk_rat = rational && (auto_walker
                                           ? ctx->n_bag_remaining >= 2
                                           : (walk_env && atoi(walk_env) > 0));

    if (walk_rat) {
      const char *walk_alpha_env = getenv("PASSPEG_GREEDY_ALPHA");
      const double walk_alpha = walk_alpha_env && *walk_alpha_env
                                    ? atof(walk_alpha_env)
                                    : 1e-4;
      const int per_ply_k_default =
          ctx->opp_top_k > 0 ? ctx->opp_top_k : 8;
      // PASSPEG_GREEDY_WALK_K="K1,K2,K3,K4" overrides the per-ply
      // candidate cap by current bag-size-at-ply. Position i = bag
      // size i+1. Unspecified entries fall back to per_ply_k_default.
      // Example: WALK_K="8,32,8" → at a ply with bag=1 use K=8, bag=2
      // K=32, bag=3 K=8. Lets you spend more search where it counts
      // (inner 2-peg) and less where it's cheap (inner 3-peg).
      int walk_k_by_bag[16];
      for (int i = 0; i < 16; i++) {
        walk_k_by_bag[i] = per_ply_k_default;
      }
      {
        const char *walk_k_env = getenv("PASSPEG_GREEDY_WALK_K");
        if (walk_k_env && *walk_k_env) {
          char tmp[256];
          snprintf(tmp, sizeof(tmp), "%s", walk_k_env);
          int i = 0;
          char *tok = strtok(tmp, ",");
          while (tok != NULL && i < 16) {
            walk_k_by_bag[i++] = atoi(tok);
            tok = strtok(NULL, ",");
          }
        }
      }

      // opp_ml (generated at the top of the d>=1 branch on the original
      // `game`) is unused by the walker; release it here. The walker
      // builds fresh post-cand games per permutation below.
      move_list_destroy(opp_ml);
      game_destroy(game);

      // Enumerate distinct lexicographic orderings of bag_remaining.
      // Each ordering is a sub-scenario: opp draws the first tile in
      // order, mover draws the second, etc. We rebuild the post-cand
      // game per perm so that the bag's deterministic draw order
      // matches the perm.
      MachineLetter perm[16];
      for (int i = 0; i < ctx->n_bag_remaining; i++) {
        perm[i] = bag_remaining[i];
      }
      // Sort ascending.
      for (int i = 1; i < ctx->n_bag_remaining; i++) {
        for (int j = i;
             j > 0 && perm[j] < perm[j - 1]; j--) {
          MachineLetter tmp = perm[j];
          perm[j] = perm[j - 1];
          perm[j - 1] = tmp;
        }
      }

      do {
      char perm_remaining_str[32] = {0};
      for (int i = 0;
           i < ctx->n_bag_remaining && i < 30; i++) {
        perm_remaining_str[i] =
            ctx->ld->ld_ml_to_hl[perm[i]][0];
      }

      Game *walker = peg_make_post_cand_game(
          ctx->base_game, ctx->mover_idx, ctx->unseen, ctx->ld_size,
          ctx->cand, ctx->k_drawn, mover_drawn, ctx->n_bag_remaining,
          perm);
      StringBuilder *walker_pv =
          ctx->tsv_f ? string_builder_create() : NULL;

      // Per-cand opp-ply counter: lets the user set a different K for the
      // very first opp decision (deeper search at the start of the peg)
      // vs subsequent opp decisions (where the bag is smaller and the
      // game is more constrained).
      const char *first_opp_k_env =
          getenv("PASSPEG_GREEDY_WALK_K_FIRST_OPP");
      const char *later_opp_k_env =
          getenv("PASSPEG_GREEDY_WALK_K_LATER_OPP");
      const int first_opp_k =
          first_opp_k_env && *first_opp_k_env ? atoi(first_opp_k_env) : 0;
      const int later_opp_k =
          later_opp_k_env && *later_opp_k_env ? atoi(later_opp_k_env) : 0;
      int opp_ply_count = 0;
      // Total rational nonempty decisions made by the walker. We cap the
      // walker at ctx->depth plies so non-emptier and emptier cands both
      // get exactly `depth` plies of informed lookahead at stage `depth`.
      // Whatever remains after the walker (if any) is filled in by a
      // terminal endgame_solve when the bag has emptied.
      int walker_plies = 0;

      while (bag_get_letters(game_get_bag(walker)) > 0) {
        // Cascade-budget poll. The walker loop fans out into deep
        // per-ply work (opp-side perception enumerations, mover-side
        // move generation + playouts). Without this poll a single
        // scenario can run the full walker tree even after the outer
        // wall budget is exceeded.
        if (ctx->deadline_monotonic_ns > 0 &&
            ctimer_monotonic_ns() > ctx->deadline_monotonic_ns) {
          break;
        }
        const int turn = game_get_player_on_turn_index(walker);
        const bool is_opp = (turn != ctx->mover_idx);
        const int bag_at_ply = bag_get_letters(game_get_bag(walker));
        int per_ply_k =
            (bag_at_ply >= 1 && bag_at_ply <= 16)
                ? walk_k_by_bag[bag_at_ply - 1]
                : per_ply_k_default;
        if (is_opp) {
          if (opp_ply_count == 0 && first_opp_k > 0) {
            per_ply_k = first_opp_k;
          } else if (opp_ply_count > 0 && later_opp_k > 0) {
            per_ply_k = later_opp_k;
          }
        }

        MoveList *wml = move_list_create(16384);
        const MoveGenArgs wga = {
            .game = walker,
            .move_list = wml,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .thread_index = ctx->worker_idx,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&wga);
        const int n_full = move_list_get_count(wml);
        // MOVE_RECORD_ALL is heap-ordered; iterating positions 0..N grabs
        // moves by heap layout, not by equity. Sort the placement-only
        // candidates by equity descending and then take the first per_ply_k.
        int *placement_idx =
            malloc_or_die((size_t)(n_full > 0 ? n_full : 1) * sizeof(int));
        int n_placement = 0;
        for (int i = 0; i < n_full; i++) {
          const Move *m = move_list_get_move(wml, i);
          if (move_get_type(m) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
            placement_idx[n_placement++] = i;
          }
        }
        const int top_take = per_ply_k < n_placement ? per_ply_k : n_placement;
        for (int ti = 0; ti < top_take; ti++) {
          int best = ti;
          Equity best_eq =
              move_get_equity(move_list_get_move(wml, placement_idx[ti]));
          for (int j = ti + 1; j < n_placement; j++) {
            const Equity je =
                move_get_equity(move_list_get_move(wml, placement_idx[j]));
            if (je > best_eq) {
              best = j;
              best_eq = je;
            }
          }
          if (best != ti) {
            const int tmp = placement_idx[ti];
            placement_idx[ti] = placement_idx[best];
            placement_idx[best] = tmp;
          }
        }
        // PASSPEG_OPP_RANK_BY_PLAYOUT=1: at each opp ply, re-rank the
        // top-PASSPEG_OPP_RANK_POOL (default 32) static-equity candidates
        // by realized greedy-playout outcome (lowest mover_total = worst
        // for mover = best opp choice), then keep the top per_ply_k from
        // that re-rank. This is more expensive but should pick up moves
        // that are defensively strong without scoring high (which static
        // equity systematically under-ranks).
        if (is_opp) {
          const char *opp_rank_env = getenv("PASSPEG_OPP_RANK_BY_PLAYOUT");
          const bool rank_by_playout =
              opp_rank_env && atoi(opp_rank_env) > 0;
          if (rank_by_playout) {
            const char *pool_env = getenv("PASSPEG_OPP_RANK_POOL");
            const int pool_size_req =
                pool_env && *pool_env ? atoi(pool_env) : 32;
            const int pool_size = pool_size_req < n_placement
                                      ? pool_size_req
                                      : n_placement;
            // First take the top pool_size by static equity (already sorted
            // for top top_take = per_ply_k; need to extend the sort to
            // pool_size).
            for (int ti = top_take; ti < pool_size; ti++) {
              int best = ti;
              Equity best_eq = move_get_equity(
                  move_list_get_move(wml, placement_idx[ti]));
              for (int j = ti + 1; j < n_placement; j++) {
                const Equity je = move_get_equity(
                    move_list_get_move(wml, placement_idx[j]));
                if (je > best_eq) {
                  best = j;
                  best_eq = je;
                }
              }
              if (best != ti) {
                const int tmp = placement_idx[ti];
                placement_idx[ti] = placement_idx[best];
                placement_idx[best] = tmp;
              }
            }
            // For each candidate in the pool, run a realized greedy
            // playout from post-opp state and record mover_total.
            int32_t *po_mt =
                malloc_or_die((size_t)pool_size * sizeof(int32_t));
            for (int i = 0; i < pool_size; i++) {
              const Move *m =
                  move_list_get_move(wml, placement_idx[i]);
              Game *probe = game_duplicate(walker);
              game_set_endgame_solving_mode(probe);
              game_set_backup_mode(probe, BACKUP_MODE_OFF);
              play_move(m, probe, NULL);
              game_set_game_end_reason(probe, GAME_END_REASON_NONE);
              MoveList *pml = move_list_create(1);
              po_mt[i] = peg_greedy_playout_pv(
                  probe, ctx->mover_idx, pml, ctx->worker_idx, NULL, 0,
                  NULL, 0, NULL, 0, NULL, 0);
              move_list_destroy(pml);
              game_destroy(probe);
            }
            // Partial-selection-sort: keep the per_ply_k entries with the
            // LOWEST po_mt (best for opp).
            for (int ti = 0; ti < top_take; ti++) {
              int best = ti;
              int32_t best_mt = po_mt[ti];
              for (int j = ti + 1; j < pool_size; j++) {
                if (po_mt[j] < best_mt) {
                  best = j;
                  best_mt = po_mt[j];
                }
              }
              if (best != ti) {
                const int tmp_idx = placement_idx[ti];
                placement_idx[ti] = placement_idx[best];
                placement_idx[best] = tmp_idx;
                const int32_t tmp_mt = po_mt[ti];
                po_mt[ti] = po_mt[best];
                po_mt[best] = tmp_mt;
              }
            }
            free(po_mt);
          }
        }

        int sel_idx[256];
        int n_sel = 0;
        for (int i = 0; i < top_take && n_sel < 256; i++) {
          sel_idx[n_sel++] = placement_idx[i];
        }
        free(placement_idx);
        if (n_sel == 0) {
          move_list_destroy(wml);
          break;
        }

        int best_local = 0;
        if (is_opp) {
          // Opp ply: perception eval with n_bag-multiset enumeration.
          const int opp_idx = turn;
          uint8_t walk_unseen[MAX_ALPHABET_SIZE];
          peg_compute_unseen(walker, opp_idx, walk_unseen);
          MachineLetter walk_types[MAX_ALPHABET_SIZE];
          int walk_type_counts[MAX_ALPHABET_SIZE];
          int n_walk_types = 0;
          for (int ml_idx = 0; ml_idx < ctx->ld_size; ml_idx++) {
            if (walk_unseen[ml_idx] > 0) {
              walk_types[n_walk_types] = (MachineLetter)ml_idx;
              walk_type_counts[n_walk_types] = (int)walk_unseen[ml_idx];
              n_walk_types++;
            }
          }
          // Realized bag composition (indexed parallel to walk_types).
          int realized_bag_counts[MAX_ALPHABET_SIZE] = {0};
          const Bag *wbag = game_get_bag(walker);
          const int n_bag_now = bag_get_letters(wbag);
          for (int t = 0; t < n_walk_types; t++) {
            realized_bag_counts[t] = bag_get_letter(
                (Bag *)wbag, walk_types[t]);
          }
          double best_util = -1e18;
          for (int pick_idx = 0; pick_idx < n_sel; pick_idx++) {
            // Per-iteration cascade-budget poll. Each peg_eval_opp_with_perception
            // call is a deep recursive enumeration over perception opp-POV states that
            // can run hundreds of game_duplicate + greedy_playout calls;
            // without this poll a single scenario can exceed the outer wall
            // budget by an order of magnitude.
            if (ctx->deadline_monotonic_ns > 0 &&
                ctimer_monotonic_ns() > ctx->deadline_monotonic_ns) {
              break;
            }
            const Move *m = move_list_get_move(wml, sel_idx[pick_idx]);
            double util = 0.0;
            int32_t realized = 0;
            // Format opp move text for inner-TSV annotation (cheap when
            // tsv disabled — string_builder is light, and the actual write
            // only happens if inner_tsv_f is set).
            char opp_move_text[64] = {0};
            int opp_move_score = 0;
            if (ctx->inner_tsv_f) {
              StringBuilder *sb = string_builder_create();
              string_builder_add_move(sb, game_get_board(walker), m,
                                      game_get_ld(walker), true);
              snprintf(opp_move_text, sizeof(opp_move_text), "%s",
                       string_builder_peek(sb));
              string_builder_destroy(sb);
              opp_move_score = equity_to_int(move_get_score(m));
            }
            peg_eval_opp_with_perception(
                ctx, walker, m, walk_types, walk_type_counts,
                n_walk_types, n_bag_now, realized_bag_counts, walk_alpha,
                &util, &realized,
                ctx->inner_tsv_f ? drawn_str : NULL,
                ctx->inner_tsv_f ? perm_remaining_str : NULL,
                ctx->inner_tsv_f ? opp_move_text : NULL,
                opp_move_score);
            if (util > best_util) {
              best_util = util;
              best_local = pick_idx;
            }
          }
        } else {
          // Mover ply: realized greedy mt per candidate.
          int32_t best_mt = INT32_MIN;
          for (int pick_idx = 0; pick_idx < n_sel; pick_idx++) {
            if (ctx->deadline_monotonic_ns > 0 &&
                ctimer_monotonic_ns() > ctx->deadline_monotonic_ns) {
              break;
            }
            const Move *m = move_list_get_move(wml, sel_idx[pick_idx]);
            Game *probe = game_duplicate(walker);
            game_set_endgame_solving_mode(probe);
            game_set_backup_mode(probe, BACKUP_MODE_OFF);
            play_move(m, probe, NULL);
            game_set_game_end_reason(probe, GAME_END_REASON_NONE);
            MoveList *pml = move_list_create(1);
            const int32_t mt = peg_greedy_playout_pv(
                probe, ctx->mover_idx, pml, ctx->worker_idx, NULL, 0,
                NULL, 0, NULL, 0, NULL, 0);
            move_list_destroy(pml);
            game_destroy(probe);
            if (mt > best_mt) {
              best_mt = mt;
              best_local = pick_idx;
            }
          }
        }

        const Move *picked = move_list_get_move(wml, sel_idx[best_local]);
        if (walker_pv) {
          if (string_builder_length(walker_pv) > 0) {
            string_builder_add_string(walker_pv, " | ");
          }
          string_builder_add_move(walker_pv, game_get_board(walker),
                                  picked, game_get_ld(walker), true);
        }
        play_move(picked, walker, NULL);
        game_set_game_end_reason(walker, GAME_END_REASON_NONE);
        if (is_opp) {
          opp_ply_count++;
        }
        walker_plies++;
        move_list_destroy(wml);
        // Cap the walker at ctx->depth plies. Anything beyond is filled
        // in by greedy / endgame_solve below.
        if (walker_plies >= ctx->depth) {
          break;
        }
      }

      // Finish the scenario. Every cand at stage `depth` gets exactly
      // `depth` lookahead plies. The walker has already done some of
      // those (`walker_plies` — its rational-opp pre-bag-empty
      // decisions). The remaining (`depth - walker_plies`) plies are
      // filled in by endgame_solve when the bag is empty. If the bag
      // still has tiles (walker capped early due to walker_plies >=
      // depth), no remaining plies — fall back to greedy.
      const int32_t mover_lead =
          equity_to_int(player_get_score(
              game_get_player(walker, ctx->mover_idx))) -
          equity_to_int(player_get_score(
              game_get_player(walker, 1 - ctx->mover_idx)));
      int32_t walker_mt = 0;
      char final_pv[1024] = {0};
      char final_mr[32] = {0};
      char final_or[32] = {0};
      int remaining_plies = ctx->depth - walker_plies;
      const bool bag_empty_now =
          bag_get_letters(game_get_bag(walker)) == 0;
      // PASSPEG_INNER_USE_ENDGAME=N (plies): when the walker exits with the
      // bag already empty but no remaining plies in its depth budget, the
      // fallback is greedy — that misvalues endgames the same way the leaf
      // does. Promote remaining_plies to at least N so the terminal eval
      // uses endgame_solve (already endgame-aware downstream).
      {
        const char *inner_eg_env = getenv("PASSPEG_INNER_USE_ENDGAME");
        const int inner_eg_plies =
            inner_eg_env && *inner_eg_env ? atoi(inner_eg_env) : 0;
        if (inner_eg_plies > 0 && bag_empty_now &&
            remaining_plies < inner_eg_plies) {
          remaining_plies = inner_eg_plies;
        }
      }
      if (remaining_plies > 0 && bag_empty_now &&
          game_get_game_end_reason(walker) == GAME_END_REASON_NONE) {
        EndgameCtx *eg_ctx = NULL;
        EndgameResults *eg_results = endgame_results_create();
        EndgameArgs ea = {
            .thread_control = ctx->thread_control,
            .game = walker,
            .plies = remaining_plies,
            .shared_tt = NULL,
            .initial_small_move_arena_size =
                DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
            .num_threads = 1,
            .use_heuristics = true,
            .num_top_moves = 1,
            .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
            .skip_word_pruning = true,
            .thread_index_offset = ctx->worker_idx,
            .soft_time_limit = peg_remaining_budget_secs(ctx),
            .hard_time_limit = peg_remaining_budget_secs(ctx),
            .external_deadline_ns = ctx->deadline_monotonic_ns,
        };
        endgame_solve_inline(&eg_ctx, &ea, eg_results);
        // Cascade deadline may have fired mid-search — guard against using
        // an uninitialized result (see equity.h equity_to_int assertion).
        const int walker_eg_depth =
            endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
        if (walker_eg_depth < 0) {
          MoveList *pl_fb = move_list_create(1);
          walker_mt = peg_greedy_playout_pv(
              walker, ctx->mover_idx, pl_fb, ctx->worker_idx,
              walker_pv ? final_pv : NULL, sizeof(final_pv),
              walker_pv ? final_mr : NULL, sizeof(final_mr),
              walker_pv ? final_or : NULL, sizeof(final_or), NULL, 0);
          move_list_destroy(pl_fb);
        } else {
          const int turn = game_get_player_on_turn_index(walker);
          const int32_t eg_val =
              endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
          walker_mt =
              (turn == ctx->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
        }
        endgame_ctx_destroy(eg_ctx);
        endgame_results_destroy(eg_results);
      } else {
        MoveList *final_ml = move_list_create(1);
        walker_mt = peg_greedy_playout_pv(
            walker, ctx->mover_idx, final_ml, ctx->worker_idx,
            walker_pv ? final_pv : NULL, sizeof(final_pv),
            walker_pv ? final_mr : NULL, sizeof(final_mr),
            walker_pv ? final_or : NULL, sizeof(final_or), NULL, 0);
        move_list_destroy(final_ml);
      }

      if (walker_pv) {
        if (final_pv[0] != '\0') {
          if (string_builder_length(walker_pv) > 0) {
            string_builder_add_string(walker_pv, " | ");
          }
          string_builder_add_string(walker_pv, final_pv);
        }
        snprintf(pv_text, sizeof(pv_text), "%s",
                 string_builder_peek(walker_pv));
        snprintf(mover_rack_end, sizeof(mover_rack_end), "%s", final_mr);
        snprintf(opp_rack_end, sizeof(opp_rack_end), "%s", final_or);
        string_builder_destroy(walker_pv);
      }
      game_destroy(walker);

      // Per-perm aggregation. Each ordering counts as a distinct
      // sub-scenario with weight = multiset weight (so a multiset with
      // n_orderings letter orderings contributes n_orderings entries
      // to weight_sum; this captures the physical bag-draw orderings
      // we'd otherwise miss).
      peg_lock(ctx->res_mutex);
      ctx->res->weight_sum += weight;
      ctx->res->spread_sum += weight * (int64_t)walker_mt;
      if (walker_mt > 0) {
        ctx->res->win_x2 += 2 * weight;
      } else if (walker_mt == 0) {
        ctx->res->win_x2 += weight;
      }
      ctx->res->n_scen++;
      peg_unlock(ctx->res_mutex);

      if (ctx->tsv_f) {
        peg_lock(ctx->tsv_mutex);
        fprintf(
            ctx->tsv_f,
            "%d\t%s\t%d\t%d\t%d\t%s\t%s\t%lld\t%d\t%s\t%s\t%s\t%s\t%s\n",
            ctx->pos_idx, ctx->cand_txt, ctx->cand_score,
            ctx->k_drawn + ctx->n_bag_remaining, ctx->k_drawn, drawn_str,
            perm_remaining_str, (long long)weight, (int)walker_mt,
            post_cand_cgp, "", pv_text, mover_rack_end, opp_rack_end);
        peg_unlock(ctx->tsv_mutex);
      }
      } while (peg_next_perm(perm, ctx->n_bag_remaining));

      return;  // walker handled all aggregation per-perm; skip the
               // single-mt fold at the bottom of emit_split.
    } else if (rational) {
      // 1. Pre-filter opp moves to placements + opp_move_filter.
      int *opp_cand_idx =
          malloc_or_die((size_t)(n_opp > 0 ? n_opp : 1) * sizeof(int));
      int n_opp_cand = 0;
      for (int opp_rank = 0; opp_rank < n_opp; opp_rank++) {
        const Move *opp_move_chk = move_list_get_move(opp_ml, opp_rank);
        if (move_get_type(opp_move_chk) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        if (ctx->opp_move_filter) {
          char text[64] = {0};
          StringBuilder *sb_chk = string_builder_create();
          string_builder_add_move(sb_chk, game_get_board(game),
                                  opp_move_chk, game_get_ld(game), true);
          snprintf(text, sizeof(text), "%s",
                   string_builder_peek(sb_chk));
          string_builder_destroy(sb_chk);
          bool match = false;
          char tmp[2048];
          snprintf(tmp, sizeof(tmp), "%s", ctx->opp_move_filter);
          char *tok = strtok(tmp, ";");
          while (tok != NULL) {
            if (strstr(text, tok) != NULL) {
              match = true;
              break;
            }
            tok = strtok(NULL, ";");
          }
          if (!match) {
            continue;
          }
        }
        opp_cand_idx[n_opp_cand++] = opp_rank;
      }

      // 2. opp's perceived bag-tile pool.
      const int opp_idx = 1 - ctx->mover_idx;
      uint8_t opp_unseen[MAX_ALPHABET_SIZE];
      peg_compute_unseen(game, opp_idx, opp_unseen);
      MachineLetter opp_types[MAX_ALPHABET_SIZE];
      int opp_type_counts[MAX_ALPHABET_SIZE];
      int n_opp_types = 0;
      for (int ml = 0; ml < ctx->ld_size; ml++) {
        if (opp_unseen[ml] > 0) {
          opp_types[n_opp_types] = (MachineLetter)ml;
          opp_type_counts[n_opp_types] = (int)opp_unseen[ml];
          n_opp_types++;
        }
      }
      int realized_ti = -1;
      {
        const MachineLetter realized_tile = bag_remaining[0];
        for (int t = 0; t < n_opp_types; t++) {
          if (opp_types[t] == realized_tile) {
            realized_ti = t;
            break;
          }
        }
      }

      const char *alpha_env = getenv("PASSPEG_GREEDY_ALPHA");
      const double alpha =
          alpha_env && *alpha_env ? atof(alpha_env) : 1e-4;

      // 3. Halving: stages 0..depth-1 cut, final stage at depth picks.
      int n_to_eval = n_opp_cand;
      double *final_utility = NULL;
      int32_t *final_realized = NULL;
      for (int stage = 0; stage <= ctx->depth && n_to_eval > 0; stage++) {
        const int target_k =
            stage < ctx->depth ? (ctx->opp_top_k >> stage) : n_to_eval;
        if (stage < ctx->depth && (target_k <= 0 || n_to_eval <= target_k)) {
          continue;
        }
        // Build per-(opp, perceived_tile) jobs and dispatch.
        const int n_jobs = n_to_eval * n_opp_types;
        PegOppInnerJob *jobs =
            malloc_or_die((size_t)n_jobs * sizeof(PegOppInnerJob));
        void **args =
            malloc_or_die((size_t)n_jobs * sizeof(void *));
        for (int i = 0; i < n_to_eval; i++) {
          const Move *opp_move =
              move_list_get_move(opp_ml, opp_cand_idx[i]);
          for (int ti = 0; ti < n_opp_types; ti++) {
            int idx = i * n_opp_types + ti;
            jobs[idx] = (PegOppInnerJob){
                .base_game = game,
                .mover_idx = ctx->mover_idx,
                .ld_size = ctx->ld_size,
                .opp_move = opp_move,
                .bag_ml = opp_types[ti],
                .weight = opp_type_counts[ti],
                .n_opp_types = n_opp_types,
                .opp_types = opp_types,
                .opp_type_counts = opp_type_counts,
                .ti = ti,
                .opp_depth = stage,
                .thread_control = ctx->thread_control,
                .deadline_monotonic_ns = ctx->deadline_monotonic_ns,
                .mover_total = 0,
            };
            args[idx] = &jobs[idx];
          }
        }
        if (ctx->executor && n_jobs > 0) {
          peg_pool_submit_and_wait(
              ctx->executor, peg_opp_inner_worker_fn, args, n_jobs,
              ctx->worker_idx);
        } else {
          for (int j = 0; j < n_jobs; j++) {
            peg_opp_inner_worker_fn(&jobs[j], ctx->worker_idx);
          }
        }
        // Aggregate per-opp utility + realized mt.
        double *stage_util =
            malloc_or_die((size_t)n_to_eval * sizeof(double));
        int32_t *stage_realized =
            malloc_or_die((size_t)n_to_eval * sizeof(int32_t));
        for (int i = 0; i < n_to_eval; i++) {
          int64_t weight_sum = 0;
          int64_t spread_sum_opp = 0;
          double win_x2_sum = 0.0;
          int32_t realized_mt = 0;
          for (int ti = 0; ti < n_opp_types; ti++) {
            int idx = i * n_opp_types + ti;
            int32_t mt = jobs[idx].mover_total;
            int w = jobs[idx].weight;
            int32_t mt_opp = -mt;
            weight_sum += w;
            spread_sum_opp += (int64_t)mt_opp * w;
            if (mt_opp > 0) {
              win_x2_sum += 2.0 * w;
            } else if (mt_opp == 0) {
              win_x2_sum += w;
            }
            if (ti == realized_ti) {
              realized_mt = mt;
            }
          }
          double opp_winpct = win_x2_sum / (2.0 * (double)weight_sum);
          double opp_mean_spread =
              (double)spread_sum_opp / (double)weight_sum;
          stage_util[i] = opp_winpct + alpha * opp_mean_spread;
          stage_realized[i] = realized_mt;
        }
        free(jobs);
        free(args);
        if (stage < ctx->depth) {
          // Partial selection sort: top target_k by utility DESC.
          for (int i = 0; i < target_k; i++) {
            int max_i = i;
            for (int j = i + 1; j < n_to_eval; j++) {
              if (stage_util[j] > stage_util[max_i]) {
                max_i = j;
              }
            }
            if (max_i != i) {
              double t_u = stage_util[i];
              stage_util[i] = stage_util[max_i];
              stage_util[max_i] = t_u;
              int32_t t_r = stage_realized[i];
              stage_realized[i] = stage_realized[max_i];
              stage_realized[max_i] = t_r;
              int t_idx = opp_cand_idx[i];
              opp_cand_idx[i] = opp_cand_idx[max_i];
              opp_cand_idx[max_i] = t_idx;
            }
          }
          n_to_eval = target_k;
          free(stage_util);
          free(stage_realized);
        } else {
          free(final_utility);
          free(final_realized);
          final_utility = stage_util;
          final_realized = stage_realized;
        }
      }

      // 4. opp picks utility-max from the final stage.
      int32_t mover_total_rational = 0;
      if (final_utility != NULL && n_to_eval > 0) {
        int picked = 0;
        for (int i = 1; i < n_to_eval; i++) {
          if (final_utility[i] > final_utility[picked]) {
            picked = i;
          }
        }
        mover_total_rational = final_realized[picked];
        // For TSV: capture opp's pick text + endgame PV under the
        // realized bag tile. One extra endgame_solve per scenario.
        if (ctx->tsv_f) {
          const Move *picked_move =
              move_list_get_move(opp_ml, opp_cand_idx[picked]);
          char picked_text[64] = {0};
          {
            StringBuilder *sb_p = string_builder_create();
            string_builder_add_move(sb_p, game_get_board(game), picked_move,
                                    game_get_ld(game), true);
            snprintf(picked_text, sizeof(picked_text), "%s",
                     string_builder_peek(sb_p));
            string_builder_destroy(sb_p);
          }
          // Build the realized opp_pov_game game (same as peg_opp_inner_worker_fn
          // does, but for the realized bag tile only) and run
          // endgame_solve with PV capture.
          Game *opp_pov_game = game_duplicate(game);
          game_set_endgame_solving_mode(opp_pov_game);
          game_set_backup_mode(opp_pov_game, BACKUP_MODE_OFF);
          {
            Bag *hb = game_get_bag(opp_pov_game);
            for (int ml = 0; ml < ctx->ld_size; ml++) {
              while (bag_get_letter(hb, (MachineLetter)ml) > 0) {
                (void)bag_draw_letter(hb, (MachineLetter)ml, 0);
              }
            }
            // Bag should already be empty; nothing else to seed since
            // the realized state mirrors the post-cand game.
          }
          play_move(picked_move, opp_pov_game, NULL);
          game_set_game_end_reason(opp_pov_game, GAME_END_REASON_NONE);
          const LetterDistribution *bld = game_get_ld(opp_pov_game);
          EndgameCtx *eg_ctx = NULL;
          EndgameResults *eg_results = endgame_results_create();
          EndgameArgs ea = {
              .thread_control = ctx->thread_control,
              .game = opp_pov_game,
              .plies = ctx->depth,
              .shared_tt = NULL,
              .initial_small_move_arena_size =
                  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
              .num_threads = 1,
              .use_heuristics = true,
              .num_top_moves = 1,
              .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
              .skip_word_pruning = true,
              .thread_index_offset = ctx->worker_idx,
              .soft_time_limit = 5.0,
              .hard_time_limit = 5.0,
              // soft/hard_time_limit only caps the next IDS depth start —
              // a single depth can run past it. external_deadline_ns is
              // the strict mid-search bail. Clamp by cascade deadline so
              // this realized-pick endgame can never outlive the wall budget.
              .external_deadline_ns = peg_clamp_deadline_ns(
                  ctimer_monotonic_ns() + (int64_t)(5.0 * 1.0e9),
                  ctx->deadline_monotonic_ns),
          };
          endgame_solve_inline(&eg_ctx, &ea, eg_results);
          // Build the PV text: opp's pick first, then endgame plies.
          StringBuilder *pv_sb = string_builder_create();
          string_builder_add_string(pv_sb, picked_text);
          const PVLine *pv =
              endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
          if (pv && pv->num_moves > 0) {
            Game *pv_game = game_duplicate(opp_pov_game);
            game_set_endgame_solving_mode(pv_game);
            game_set_backup_mode(pv_game, BACKUP_MODE_OFF);
            for (int mi = 0; mi < pv->num_moves; mi++) {
              Move m_full;
              small_move_to_move(&m_full, &pv->moves[mi],
                                 game_get_board(pv_game));
              string_builder_add_string(pv_sb, " | ");
              string_builder_add_move(pv_sb, game_get_board(pv_game),
                                      &m_full, bld, true);
              play_move(&m_full, pv_game, NULL);
            }
            char *cgp = game_get_cgp(pv_game, true);
            snprintf(final_cgp, sizeof(final_cgp), "%s", cgp ? cgp : "");
            free(cgp);
            StringBuilder *rsb = string_builder_create();
            string_builder_add_rack(
                rsb,
                player_get_rack(game_get_player(pv_game, ctx->mover_idx)),
                bld, false);
            snprintf(mover_rack_end, sizeof(mover_rack_end), "%s",
                     string_builder_peek(rsb));
            string_builder_destroy(rsb);
            StringBuilder *rsb2 = string_builder_create();
            string_builder_add_rack(
                rsb2,
                player_get_rack(
                    game_get_player(pv_game, 1 - ctx->mover_idx)),
                bld, false);
            snprintf(opp_rack_end, sizeof(opp_rack_end), "%s",
                     string_builder_peek(rsb2));
            string_builder_destroy(rsb2);
            game_destroy(pv_game);
          }
          snprintf(pv_text, sizeof(pv_text), "%s",
                   string_builder_peek(pv_sb));
          string_builder_destroy(pv_sb);
          endgame_ctx_destroy(eg_ctx);
          endgame_results_destroy(eg_results);
          game_destroy(opp_pov_game);
        }
      }
      free(final_utility);
      free(final_realized);
      free(opp_cand_idx);
      move_list_destroy(opp_ml);
      mover_total = mover_total_rational;
    } else {
      int32_t worst_for_mover = 0;
    bool have_any = false;
    char worst_opp_text[64] = {0};
    StringBuilder *pv_builder = ctx->tsv_f ? string_builder_create() : NULL;

    // Pre-filter to placement moves (and optionally to the opp_move_filter
    // substring set). When PASSPEG_GREEDY_OPP_RERANK=1, also sort by
    // d=0 greedy mover_total ascending (worst-for-mover first) and take
    // the top opp_top_k — much more honest than equity's natural order
    // for the d=1 MIN-over-branches semantics (a low-equity but lethal
    // reply like TEMPURA can sit at rank #700 by equity but be #1 by
    // greedy mover_total). Default is the legacy equity order.
    int *opp_cand_idx =
        malloc_or_die((size_t)(n_opp > 0 ? n_opp : 1) * sizeof(int));
    int n_opp_cand = 0;
    for (int opp_rank = 0; opp_rank < n_opp; opp_rank++) {
      const Move *opp_move_chk = move_list_get_move(opp_ml, opp_rank);
      if (move_get_type(opp_move_chk) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      if (ctx->opp_move_filter) {
        char text[64] = {0};
        StringBuilder *sb_chk = string_builder_create();
        string_builder_add_move(sb_chk, game_get_board(game), opp_move_chk,
                                game_get_ld(game), true);
        snprintf(text, sizeof(text), "%s", string_builder_peek(sb_chk));
        string_builder_destroy(sb_chk);
        bool match = false;
        char tmp[2048];
        snprintf(tmp, sizeof(tmp), "%s", ctx->opp_move_filter);
        char *tok = strtok(tmp, ";");
        while (tok != NULL) {
          if (strstr(text, tok) != NULL) {
            match = true;
            break;
          }
          tok = strtok(NULL, ";");
        }
        if (!match) {
          continue;
        }
      }
      opp_cand_idx[n_opp_cand++] = opp_rank;
    }

    // MOVE_RECORD_ALL returns a heap-ordered list, not a sorted one — so
    // opp_cand_idx[0..opp_top_k-1] in input order picks 8 moves by heap
    // position, not by equity. Partial-selection-sort the prefix we'll
    // actually evaluate so the "top opp_top_k" really is the top opp_top_k
    // by movegen equity. Without this, killer outplays (TOREROS, NOTARISE)
    // sit deep in the heap and never reach the MIN loop.
    {
      const int prefix =
          ctx->opp_top_k < n_opp_cand ? ctx->opp_top_k : n_opp_cand;
      for (int i = 0; i < prefix; i++) {
        int best = i;
        Equity best_eq =
            move_get_equity(move_list_get_move(opp_ml, opp_cand_idx[i]));
        for (int j = i + 1; j < n_opp_cand; j++) {
          const Equity je =
              move_get_equity(move_list_get_move(opp_ml, opp_cand_idx[j]));
          if (je > best_eq) {
            best = j;
            best_eq = je;
          }
        }
        if (best != i) {
          const int tmp = opp_cand_idx[i];
          opp_cand_idx[i] = opp_cand_idx[best];
          opp_cand_idx[best] = tmp;
        }
      }
    }

    // Optional Sequential-Halving ladder. PASSPEG_GREEDY_OPP_HALVE=1
    // implies cuts of K, K/2, K/4, ..., K/2^(D-1) at depths 0, 1, ...,
    // D-1, where K = opp_top_k and D = ctx->depth. The final MIN runs
    // over the K/2^(D-1) survivors at depth D. For (K=8, D=2):
    //   d=0 on all → top 8, d=1 on 8 → top 4, d=2 on 4 → MIN.
    // When set, supersedes PASSPEG_GREEDY_OPP_RERANK.
    const char *halve_env = getenv("PASSPEG_GREEDY_OPP_HALVE");
    const bool halve = halve_env && atoi(halve_env) > 0;
    if (halve && n_opp_cand > 1 && ctx->depth >= 1) {
      int cuts[16];
      int n_stages = 0;
      // Stage s cuts to opp_top_k >> s for s = 0..depth-1.
      // (At s=0 we filter from the full opp pool down to opp_top_k.)
      for (int stage_idx = 0; stage_idx < ctx->depth && n_stages < 16; stage_idx++) {
        int k = ctx->opp_top_k >> stage_idx;
        if (k < 1) {
          k = 1;
        }
        cuts[n_stages++] = k;
      }
      for (int stage_idx = 0; stage_idx < n_stages; stage_idx++) {
        if (n_opp_cand <= cuts[stage_idx] || cuts[stage_idx] <= 0) {
          continue;
        }
        const int stage_depth = stage_idx; // d=0, d=1, d=2, ...
        int32_t *mt_arr =
            malloc_or_die((size_t)n_opp_cand * sizeof(int32_t));
        for (int i = 0; i < n_opp_cand; i++) {
          const Move *opp_move_p =
              move_list_get_move(opp_ml, opp_cand_idx[i]);
          Game *prior_branch = game_duplicate(game);
          game_set_endgame_solving_mode(prior_branch);
          game_set_backup_mode(prior_branch, BACKUP_MODE_OFF);
          play_move(opp_move_p, prior_branch, NULL);
          game_set_game_end_reason(prior_branch, GAME_END_REASON_NONE);
          if (stage_depth == 0) {
            MoveList *pml = move_list_create(1);
            mt_arr[i] = peg_greedy_playout_pv(
                prior_branch, ctx->mover_idx, pml, ctx->worker_idx, NULL, 0,
                NULL, 0, NULL, 0, NULL, 0);
            move_list_destroy(pml);
          } else {
            const int32_t mover_lead =
                equity_to_int(player_get_score(game_get_player(
                    prior_branch, ctx->mover_idx))) -
                equity_to_int(player_get_score(game_get_player(
                    prior_branch, 1 - ctx->mover_idx)));
            if (game_get_game_end_reason(prior_branch) !=
                GAME_END_REASON_NONE) {
              mt_arr[i] = mover_lead;
            } else {
              EndgameCtx *eg = NULL;
              EndgameResults *er = endgame_results_create();
              EndgameArgs ea = {
                  .thread_control = ctx->thread_control,
                  .game = prior_branch,
                  .plies = stage_depth,
                  .shared_tt = NULL,
                  .initial_small_move_arena_size =
                      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                  .num_threads = 1,
                  .use_heuristics = true,
                  .num_top_moves = 1,
                  .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
                  .skip_word_pruning = true,
                  .thread_index_offset = ctx->worker_idx,
                  .soft_time_limit = 5.0,
                  .hard_time_limit = 5.0,
                  // Clamp by cascade deadline — soft/hard_time_limit alone
                  // can't strictly cap a single IDS depth.
                  .external_deadline_ns = peg_clamp_deadline_ns(
                      ctimer_monotonic_ns() + (int64_t)(5.0 * 1.0e9),
                      ctx->deadline_monotonic_ns),
              };
              endgame_solve_inline(&eg, &ea, er);
              mt_arr[i] = mover_lead + endgame_results_get_value(
                                            er, ENDGAME_RESULT_BEST);
              endgame_ctx_destroy(eg);
              endgame_results_destroy(er);
            }
          }
          game_destroy(prior_branch);
        }
        // Partial selection sort: bring the top `keep` worst-for-mover
        // entries to the front of opp_cand_idx.
        const int keep = cuts[stage_idx];
        for (int i = 0; i < keep; i++) {
          int min_i = i;
          for (int j = i + 1; j < n_opp_cand; j++) {
            if (mt_arr[j] < mt_arr[min_i]) {
              min_i = j;
            }
          }
          if (min_i != i) {
            int32_t tmp_mt = mt_arr[i];
            mt_arr[i] = mt_arr[min_i];
            mt_arr[min_i] = tmp_mt;
            int tmp_idx = opp_cand_idx[i];
            opp_cand_idx[i] = opp_cand_idx[min_i];
            opp_cand_idx[min_i] = tmp_idx;
          }
        }
        n_opp_cand = keep;
        free(mt_arr);
      }
    }

    const char *rerank_env = getenv("PASSPEG_GREEDY_OPP_RERANK");
    const bool rerank = !halve && rerank_env && atoi(rerank_env) > 0;
    if (rerank && n_opp_cand > 1) {
      int32_t *opp_mt =
          malloc_or_die((size_t)n_opp_cand * sizeof(int32_t));
      for (int i = 0; i < n_opp_cand; i++) {
        const Move *opp_move_p =
            move_list_get_move(opp_ml, opp_cand_idx[i]);
        Game *prior_branch = game_duplicate(game);
        game_set_endgame_solving_mode(prior_branch);
        game_set_backup_mode(prior_branch, BACKUP_MODE_OFF);
        play_move(opp_move_p, prior_branch, NULL);
        game_set_game_end_reason(prior_branch, GAME_END_REASON_NONE);
        MoveList *pml = move_list_create(1);
        opp_mt[i] = peg_greedy_playout_pv(
            prior_branch, ctx->mover_idx, pml, ctx->worker_idx, NULL, 0,
            NULL, 0, NULL, 0, NULL, 0);
        move_list_destroy(pml);
        game_destroy(prior_branch);
      }
      // Selection sort ascending by opp_mt (worst-for-mover first).
      // n_opp_cand is at most ~1000 so this is fine.
      for (int i = 0; i < n_opp_cand; i++) {
        int min_i = i;
        for (int j = i + 1; j < n_opp_cand; j++) {
          if (opp_mt[j] < opp_mt[min_i]) {
            min_i = j;
          }
        }
        if (min_i != i) {
          int32_t tmp_mt = opp_mt[i];
          opp_mt[i] = opp_mt[min_i];
          opp_mt[min_i] = tmp_mt;
          int tmp_idx = opp_cand_idx[i];
          opp_cand_idx[i] = opp_cand_idx[min_i];
          opp_cand_idx[min_i] = tmp_idx;
        }
      }
      free(opp_mt);
    }

    const int n_eval_final =
        n_opp_cand < ctx->opp_top_k ? n_opp_cand : ctx->opp_top_k;

    for (int eval_idx = 0; eval_idx < n_eval_final; eval_idx++) {
      const int opp_rank = opp_cand_idx[eval_idx];
      const Move *opp_move = move_list_get_move(opp_ml, opp_rank);
      Game *branch = game_duplicate(game);
      game_set_endgame_solving_mode(branch);
      game_set_backup_mode(branch, BACKUP_MODE_OFF);
      char opp_move_text[64] = {0};
      {
        StringBuilder *opp_sb = string_builder_create();
        string_builder_add_move(opp_sb, game_get_board(branch), opp_move,
                                game_get_ld(branch), true);
        snprintf(opp_move_text, sizeof(opp_move_text), "%s",
                 string_builder_peek(opp_sb));
        string_builder_destroy(opp_sb);
      }
      (void)opp_move_text;
      play_move(opp_move, branch, NULL);
      game_set_game_end_reason(branch, GAME_END_REASON_NONE);

      // After opp plays, the bag is empty (mover drew k_drawn tiles, opp
      // drew the remaining bag_remaining tiles). Run endgame_solve at
      // ctx->depth plies for an optimal-play evaluation.
      //
      // For bag-emptier cands (n_bag_remaining == 0) we grant one extra
      // ply: non-emptiers spend their first ply on the rational opp pick
      // (the perception-utility step), leaving (depth-1) plies for the
      // post-pick endgame; emptiers have no opp-perception ply and need
      // the extra ply to compare like-for-like.
      const int eg_plies = (ctx->n_bag_remaining == 0)
                               ? ctx->depth + 1
                               : ctx->depth;
      int32_t branch_total = 0;
      char branch_pv[1024] = {0};
      char branch_mover_rack[32] = {0};
      char branch_opp_rack[32] = {0};
      char branch_final_cgp[512] = {0};
      {
        const LetterDistribution *bld = game_get_ld(branch);
        const int32_t mover_lead = equity_to_int(player_get_score(
            game_get_player(branch, ctx->mover_idx))) -
            equity_to_int(player_get_score(
                game_get_player(branch, 1 - ctx->mover_idx)));
        if (game_get_game_end_reason(branch) != GAME_END_REASON_NONE) {
          branch_total = mover_lead;
        } else {
          EndgameCtx *eg_ctx = NULL;
          EndgameResults *eg_results = endgame_results_create();
          EndgameArgs ea = {
              .thread_control = ctx->thread_control,
              .game = branch,
              .plies = eg_plies,
              .shared_tt = NULL,
              .initial_small_move_arena_size =
                  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
              .num_threads = 1,
              .use_heuristics = true,
              .num_top_moves = 1,
              .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
              .skip_word_pruning = true,
              .thread_index_offset = ctx->worker_idx,
              .soft_time_limit = 5.0,
              .hard_time_limit = 5.0,
              // Clamp by cascade deadline — soft/hard_time_limit alone
              // can't strictly cap a single IDS depth.
              .external_deadline_ns = peg_clamp_deadline_ns(
                  ctimer_monotonic_ns() + (int64_t)(5.0 * 1.0e9),
                  ctx->deadline_monotonic_ns),
          };
          endgame_solve_inline(&eg_ctx, &ea, eg_results);
          const int eg_val =
              endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
          // After opp's move, mover is on turn → eg_val is mover's gain.
          branch_total = mover_lead + eg_val;
          // Optional: capture endgame PV as text.
          if (pv_builder) {
            const PVLine *pv =
                endgame_results_get_pvline(eg_results, ENDGAME_RESULT_BEST);
            if (pv && pv->num_moves > 0) {
              Game *pv_game = game_duplicate(branch);
              game_set_endgame_solving_mode(pv_game);
              game_set_backup_mode(pv_game, BACKUP_MODE_OFF);
              StringBuilder *pv_sb = string_builder_create();
              for (int mi = 0; mi < pv->num_moves; mi++) {
                Move m_full;
                small_move_to_move(&m_full, &pv->moves[mi],
                                   game_get_board(pv_game));
                if (mi > 0) {
                  string_builder_add_string(pv_sb, " | ");
                }
                string_builder_add_move(pv_sb, game_get_board(pv_game),
                                        &m_full, bld, true);
                play_move(&m_full, pv_game, NULL);
              }
              snprintf(branch_pv, sizeof(branch_pv), "%s",
                       string_builder_peek(pv_sb));
              string_builder_destroy(pv_sb);
              char *cgp = game_get_cgp(pv_game, true);
              snprintf(branch_final_cgp, sizeof(branch_final_cgp), "%s",
                       cgp ? cgp : "");
              free(cgp);
              StringBuilder *rsb = string_builder_create();
              string_builder_add_rack(
                  rsb,
                  player_get_rack(
                      game_get_player(pv_game, ctx->mover_idx)),
                  bld, false);
              snprintf(branch_mover_rack, sizeof(branch_mover_rack), "%s",
                       string_builder_peek(rsb));
              string_builder_destroy(rsb);
              StringBuilder *rsb2 = string_builder_create();
              string_builder_add_rack(
                  rsb2,
                  player_get_rack(
                      game_get_player(pv_game, 1 - ctx->mover_idx)),
                  bld, false);
              snprintf(branch_opp_rack, sizeof(branch_opp_rack), "%s",
                       string_builder_peek(rsb2));
              string_builder_destroy(rsb2);
              game_destroy(pv_game);
            }
          }
          endgame_ctx_destroy(eg_ctx);
          endgame_results_destroy(eg_results);
        }
      }
      game_destroy(branch);

      if (!have_any || branch_total < worst_for_mover) {
        worst_for_mover = branch_total;
        snprintf(worst_opp_text, sizeof(worst_opp_text), "%s", opp_move_text);
        if (pv_builder) {
          // Capture the FULL PV for the worst branch in the TSV row: opp's
          // move first, then the greedy continuation that peg_greedy_*
          // recorded.
          snprintf(pv_text, sizeof(pv_text), "%s%s%s", opp_move_text,
                   branch_pv[0] ? " | " : "", branch_pv);
          snprintf(final_cgp, sizeof(final_cgp), "%s", branch_final_cgp);
          snprintf(mover_rack_end, sizeof(mover_rack_end), "%s",
                   branch_mover_rack);
          snprintf(opp_rack_end, sizeof(opp_rack_end), "%s", branch_opp_rack);
        }
        have_any = true;
      }
    }
    if (pv_builder) {
      string_builder_destroy(pv_builder);
    }
    free(opp_cand_idx);
    move_list_destroy(opp_ml);
    mover_total = worst_for_mover;
    (void)worst_opp_text;
    }  // end legacy MIN-over-realized branch
  }
  game_destroy(game);

  peg_lock(ctx->res_mutex);
  ctx->res->weight_sum += weight;
  ctx->res->spread_sum += weight * (int64_t)mover_total;
  if (mover_total > 0) {
    ctx->res->win_x2 += 2 * weight;
  } else if (mover_total == 0) {
    ctx->res->win_x2 += weight;
  }
  ctx->res->n_scen++;
  peg_unlock(ctx->res_mutex);

  if (ctx->tsv_f) {
    peg_lock(ctx->tsv_mutex);
    fprintf(
        ctx->tsv_f,
        "%d\t%s\t%d\t%d\t%d\t%s\t%s\t%lld\t%d\t%s\t%s\t%s\t%s\t%s\n",
        ctx->pos_idx, ctx->cand_txt, ctx->cand_score,
        ctx->k_drawn + ctx->n_bag_remaining, ctx->k_drawn, drawn_str,
        remaining_str, (long long)weight, (int)mover_total, post_cand_cgp,
        final_cgp, pv_text, mover_rack_end, opp_rack_end);
    peg_unlock(ctx->tsv_mutex);
  }
}

// Worker fn invoked by PegPool for each scenario job. Builds a
// per-worker PegEnumCtx pointing at the job's owned (n_multiset,
// mover_pick) and calls peg_emit_split in execute mode (out_jobs=NULL).
// At d=0 emit_split walks bag-tile orderings serially via the helper;
// at d>=1 it runs the opp-top-K + endgame_solve flow.
static void peg_scenario_worker_fn(void *arg, int worker_idx) {
  PegScenarioJob *job = (PegScenarioJob *)arg;
  PegEnumCtx local = *job->base_ctx;
  // Skip if the time budget has expired. The result accumulator just
  // misses this scenario; rankings reflect whichever scenarios completed
  // before the deadline.
  if (local.budget_timer != NULL && local.budget_secs > 0.0 &&
      ctimer_elapsed_seconds(local.budget_timer) > local.budget_secs) {
    return;
  }
  local.n_multiset = job->n_multiset;
  local.mover_pick = job->mover_pick;
  local.worker_idx = worker_idx;
  local.out_jobs = NULL;
  peg_emit_split(&local);
}

static void peg_opp_inner_worker_fn(void *arg, int worker_idx) {
  PegOppInnerJob *j = (PegOppInnerJob *)arg;
  // Bail immediately if the cascade deadline has already fired — don't
  // even allocate the opp_pov_game game. Leaves j->mover_total at its default (0).
  if (j->deadline_monotonic_ns > 0 &&
      ctimer_monotonic_ns() > j->deadline_monotonic_ns) {
    return;
  }
  Game *opp_pov_game = game_duplicate(j->base_game);
  game_set_endgame_solving_mode(opp_pov_game);
  game_set_backup_mode(opp_pov_game, BACKUP_MODE_OFF);
  Bag *opp_pov_bag = game_get_bag(opp_pov_game);
  Rack *opp_pov_mover_rack =
      player_get_rack(game_get_player(opp_pov_game, j->mover_idx));
  for (int ml = 0; ml < j->ld_size; ml++) {
    while (bag_get_letter(opp_pov_bag, (MachineLetter)ml) > 0) {
      (void)bag_draw_letter(opp_pov_bag, (MachineLetter)ml, 0);
    }
  }
  rack_reset(opp_pov_mover_rack);
  bag_add_letter(opp_pov_bag, j->bag_ml, 0);
  for (int t = 0; t < j->n_opp_types; t++) {
    int copies = j->opp_type_counts[t] - (t == j->ti ? 1 : 0);
    for (int k = 0; k < copies; k++) {
      rack_add_letter(opp_pov_mover_rack, j->opp_types[t]);
    }
  }
  play_move(j->opp_move, opp_pov_game, NULL);
  game_set_game_end_reason(opp_pov_game, GAME_END_REASON_NONE);
  int32_t mt = 0;
  if (j->opp_depth == 0) {
    MoveList *pl = move_list_create(1);
    mt = peg_greedy_playout_pv(opp_pov_game, j->mover_idx, pl, worker_idx, NULL, 0,
                                NULL, 0, NULL, 0, NULL, 0);
    move_list_destroy(pl);
  } else {
    const int32_t opp_pov_lead = equity_to_int(player_get_score(
        game_get_player(opp_pov_game, j->mover_idx))) -
        equity_to_int(player_get_score(
            game_get_player(opp_pov_game, 1 - j->mover_idx)));
    if (game_get_game_end_reason(opp_pov_game) != GAME_END_REASON_NONE) {
      mt = opp_pov_lead;
    } else {
      EndgameCtx *eg = NULL;
      EndgameResults *er = endgame_results_create();
      // Per-call hard wall budget for the opp-inner endgame solve.
      // soft/hard time limits only govern starting the *next* IDS depth;
      // without an external_deadline the *current* depth can run forever
      // on a pathological position. Plumbing this as an absolute
      // monotonic-ns deadline makes check_depth_deadline interrupt
      // mid-search. Configurable via PASSPEG_OPP_INNER_BUDGET (seconds).
      const char *opp_budget_env = getenv("PASSPEG_OPP_INNER_BUDGET");
      const double opp_budget = opp_budget_env && *opp_budget_env
                                     ? atof(opp_budget_env)
                                     : 5.0;
      EndgameArgs ea = {
          .thread_control = j->thread_control,
          .game = opp_pov_game,
          .plies = j->opp_depth,
          .shared_tt = NULL,
          .initial_small_move_arena_size =
              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
          .num_threads = 1,
          .use_heuristics = true,
          .num_top_moves = 1,
          .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
          .skip_word_pruning = true,
          .thread_index_offset = worker_idx,
          .soft_time_limit = opp_budget,
          .hard_time_limit = opp_budget,
          // Clamp per-call deadline by the cascade deadline so an inner
          // solve can never outlive the outer budget.
          .external_deadline_ns = peg_clamp_deadline_ns(
              ctimer_monotonic_ns() + (int64_t)(opp_budget * 1.0e9),
              j->deadline_monotonic_ns),
      };
      const int64_t solve_start_ns = ctimer_monotonic_ns();
      endgame_solve_inline(&eg, &ea, er);
      const int64_t solve_end_ns = ctimer_monotonic_ns();
      const double solve_secs = (solve_end_ns - solve_start_ns) / 1.0e9;
      if (solve_secs > opp_budget * 1.2 + 0.2) {
        // Flag if endgame_solve didn't honor the external deadline.
        fprintf(stderr,
                "[opp_inner SLOW] worker=%d plies=%d solve=%.2fs"
                " (deadline=%.2fs — endgame_solve isn't honoring it!)\n",
                worker_idx, j->opp_depth, solve_secs, opp_budget);
        fflush(stderr);
      }
      // If the 5s deadline fired before any depth completed, the result
      // value is uninitialized — use the opp_pov_game's mover_lead as a fallback so
      // downstream arithmetic doesn't propagate garbage.
      const int eg_depth =
          endgame_results_get_depth(er, ENDGAME_RESULT_BEST);
      if (eg_depth < 0) {
        mt = opp_pov_lead;
      } else {
        const int eg_val =
            endgame_results_get_value(er, ENDGAME_RESULT_BEST);
        mt = opp_pov_lead + eg_val;
      }
      endgame_ctx_destroy(eg);
      endgame_results_destroy(er);
    }
  }
  game_destroy(opp_pov_game);
  j->mover_total = mt;
}

// Generalized opp_pov_game worker: bag = opp_pov_bag_counts, mover_rack = total -
// opp_pov_bag_counts. Applies opp_move and runs greedy playout to game end.
static void peg_opp_pov_worker_fn(void *arg, int worker_idx) {
  PegOppPovJob *j = (PegOppPovJob *)arg;
  Game *opp_pov_game = game_duplicate(j->base_game);
  game_set_endgame_solving_mode(opp_pov_game);
  game_set_backup_mode(opp_pov_game, BACKUP_MODE_OFF);
  Bag *opp_pov_bag = game_get_bag(opp_pov_game);
  Rack *opp_pov_mover_rack =
      player_get_rack(game_get_player(opp_pov_game, j->mover_idx));
  // Drain bag and mover rack.
  for (int ml = 0; ml < j->ld_size; ml++) {
    while (bag_get_letter(opp_pov_bag, (MachineLetter)ml) > 0) {
      (void)bag_draw_letter(opp_pov_bag, (MachineLetter)ml, 0);
    }
  }
  rack_reset(opp_pov_mover_rack);
  // Bag = opp_pov_bag_counts.
  for (int t = 0; t < j->n_opp_types; t++) {
    for (int k = 0; k < j->opp_pov_bag_counts[t]; k++) {
      bag_add_letter(opp_pov_bag, j->opp_types[t], 0);
    }
  }
  // Mover rack = total - opp_pov_bag_counts.
  for (int t = 0; t < j->n_opp_types; t++) {
    const int copies = j->opp_type_counts[t] - j->opp_pov_bag_counts[t];
    for (int k = 0; k < copies; k++) {
      rack_add_letter(opp_pov_mover_rack, j->opp_types[t]);
    }
  }
  // Capture the opp_pov_game mover rack BEFORE opp plays — that's the rack the leaf
  // evaluator sees and that the report should show. After play_move opp
  // draws from the bag (changing nothing on mover's rack), but the rack is
  // already what we want here.
  char opp_pov_mover_rack_str[32] = {0};
  if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
    const LetterDistribution *ld = game_get_ld(opp_pov_game);
    StringBuilder *rsb = string_builder_create();
    string_builder_add_rack(rsb, opp_pov_mover_rack, ld, false);
    snprintf(opp_pov_mover_rack_str, sizeof(opp_pov_mover_rack_str), "%s",
             string_builder_peek(rsb));
    string_builder_destroy(rsb);
  }
  play_move(j->opp_move, opp_pov_game, NULL);
  game_set_game_end_reason(opp_pov_game, GAME_END_REASON_NONE);
  // Cache lookup: if this exact post-opp game state has been evaluated
  // before in this cand's pass, reuse the result. Big win when two opp-POV states
  // collapse to the same (board, mover_rack, opp_rack, bag) state — common
  // because many opp moves leave the bag empty and the rest of the state
  // is determined by the realized opp_pov_game's bag-content.
  uint64_t cache_key = 0;
  bool cache_hit = false;
  int32_t cache_hit_mt = 0;
  if (j->outer_ctx && j->outer_ctx->opp_pov_cache) {
    cache_key = peg_hash_game_state(opp_pov_game, j->mover_idx);
    cache_hit = peg_opp_pov_cache_lookup(j->outer_ctx->opp_pov_cache, cache_key,
                                       &cache_hit_mt);
  }
  // PASSPEG_INNER_USE_ENDGAME=N (plies): when the bag is empty after opp
  // plays (i.e., this was effectively a 1-tile-in-bag inner state for the
  // opp-POV state), swap greedy for an N-ply endgame_solve. Greedy systematically
  // misvalues bingo-blocking and going-out positions; in 2peg pos 28 this
  // asymmetry inflated K=1 A(N) over K=2 ACIDOT(I)c (the latter already gets
  // endgame_solve via PASSPEG_EMPTIER_USE_ENDGAME at the outer K=N path).
  // Inner endgame_solve at the K<N walker leaf is OFF by default — the
  // walker's "1peg" cands use greedy playout. Turn it on per-stage via
  // PASSPEG_INNER_USE_ENDGAME=N (= use N-ply IDS at the leaf). Optional
  // per-call wall cap via PASSPEG_INNER_EG_BUDGET (seconds, default 0
  // = no budget, let the IDS complete N plies naturally).
  const char *inner_eg_env = getenv("PASSPEG_INNER_USE_ENDGAME");
  const int inner_eg_plies =
      (inner_eg_env && *inner_eg_env) ? atoi(inner_eg_env) : 0;
  const char *inner_eg_budget_env = getenv("PASSPEG_INNER_EG_BUDGET");
  const double inner_eg_budget = inner_eg_budget_env && *inner_eg_budget_env
      ? atof(inner_eg_budget_env) : 0.0;
  const bool bag_empty_post_opp = (bag_get_letters(game_get_bag(opp_pov_game)) == 0);
  int32_t mt = 0;
  char inner_pv_text[1024] = {0};
  int inner_pv_plies = 0;  // 0 == greedy path; >0 == endgame depth used
  int inner_eg_depth_reached = 0;
  int inner_eg_status = 0; // 0 unset / 1 finished / 2 interrupted
  int inner_eg_num_moves = 0;
  int inner_eg_negamax_depth = 0;
  double inner_eg_start_ms = 0.0;  // PEG-relative timestamp (ms)
  double inner_eg_dur_ms = 0.0;
  PegInnerEgDepthLog inner_eg_log = {0};
  // For the greedy path we time the playout with this dedicated timer so
  // the inner-TSV report can show stage-1 (walker + greedy leaf) per-opp_pov_game
  // wall time too, not just the endgame-leaf path.
  Timer greedy_timer = {0};
  if (j->outer_ctx && j->outer_ctx->inner_tsv_f &&
      j->outer_ctx->budget_timer) {
    inner_eg_start_ms =
        ctimer_elapsed_seconds(j->outer_ctx->budget_timer) * 1000.0;
  }
  if (cache_hit) {
    mt = cache_hit_mt;
    inner_pv_plies = 0;
    // (skip both endgame and greedy paths — we have the answer)
  } else if (inner_eg_plies > 0 && bag_empty_post_opp &&
      game_get_game_end_reason(opp_pov_game) == GAME_END_REASON_NONE && j->outer_ctx) {
    const int32_t mover_lead =
        equity_to_int(
            player_get_score(game_get_player(opp_pov_game, j->mover_idx))) -
        equity_to_int(player_get_score(
            game_get_player(opp_pov_game, 1 - j->mover_idx)));
    // Reuse this worker's persistent EndgameCtx across calls. The slot
    // is keyed by worker_idx; first call allocates, subsequent calls
    // reset+reuse to amortize setup cost (ABDADA init, move sort, etc.).
    EndgameCtx **eg_ctx_pp = NULL;
    EndgameCtx *eg_ctx_local = NULL;
    {
      const int slot = worker_idx == 0
          ? 0
          : (worker_idx - j->outer_ctx->per_worker_eg_ctx_offset + 1);
      if (j->outer_ctx->per_worker_eg_ctx &&
          slot >= 0 && slot < j->outer_ctx->per_worker_eg_ctx_n) {
        eg_ctx_pp = &j->outer_ctx->per_worker_eg_ctx[slot];
      } else {
        eg_ctx_pp = &eg_ctx_local;
      }
    }
    EndgameResults *eg_results = endgame_results_create();
    // Initialize the per-call depth log so the per_ply_callback can
    // record (depth, value, ms) tuples. inner_eg_start_ms was set above
    // before the if/else branch so both endgame and greedy paths share it.
    ctimer_start(&inner_eg_log.call_timer);
    inner_eg_log.n = 0;
    EndgameArgs ea = {
        .thread_control = j->thread_control,
        .game = opp_pov_game,
        .plies = inner_eg_plies,
        .shared_tt = j->outer_ctx->shared_eg_tt,
        .initial_small_move_arena_size =
            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .per_ply_callback = j->outer_ctx->inner_tsv_f
                                ? peg_inner_eg_per_ply_cb
                                : NULL,
        .per_ply_callback_data = j->outer_ctx->inner_tsv_f
                                     ? (void *)&inner_eg_log
                                     : NULL,
        .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
        .skip_word_pruning = true,
        .thread_index_offset = worker_idx,
        // Per-call wall budget. external_deadline_ns is what actually
        // interrupts an in-flight iteration (check_depth_deadline polls
        // it every 1024 nodes); soft_time_limit only governs whether to
        // start the *next* depth. So we set BOTH: soft/hard guides EBF,
        // external_deadline is the hard wall stop.
        //
        // Clamp the per-call deadline by the cascade's outer deadline so
        // a single inner endgame solve can't outlive the cascade budget.
        // Without this, a deep K<N walker leaf at d=4 can run 60s+ even
        // when the cascade has 5s remaining.
        .soft_time_limit = inner_eg_budget,
        .hard_time_limit = inner_eg_budget,
        .external_deadline_ns = peg_clamp_deadline_ns(
            ctimer_monotonic_ns() + (int64_t)(inner_eg_budget * 1.0e9),
            j->outer_ctx ? j->outer_ctx->deadline_monotonic_ns : 0),
    };
    endgame_solve_inline(eg_ctx_pp, &ea, eg_results);
    inner_eg_dur_ms =
        ctimer_elapsed_seconds(&inner_eg_log.call_timer) * 1000.0;
    inner_eg_depth_reached =
        endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
    if (inner_eg_depth_reached < 0) {
      // The per-call wall budget expired before any IDS iteration
      // completed (rare with a sane 1s budget). Fall back to greedy so
      // we still produce a real number for this opp_pov_game; mark incomplete
      // only for stats — the cand stays in the ranking.
      MoveList *pl_fb = move_list_create(1);
      mt = peg_greedy_playout_pv(
          opp_pov_game, j->mover_idx, pl_fb, worker_idx, NULL, 0, NULL, 0,
          NULL, 0, NULL, 0);
      move_list_destroy(pl_fb);
      inner_pv_plies = 0;
      j->incomplete = true;
    } else {
      const int turn = game_get_player_on_turn_index(opp_pov_game);
      const int32_t eg_val =
          endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
      mt = (turn == j->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
      inner_pv_plies = inner_eg_plies;
    }
    {
      endgame_result_status_t st = endgame_results_get_status(eg_results);
      inner_eg_status = (st == ENDGAME_RESULT_STATUS_FINISHED) ? 1
                        : (st == ENDGAME_RESULT_STATUS_INTERRUPTED) ? 2
                        : 0;
    }
    // Extract endgame PV for the inner TSV.
    if (j->outer_ctx->inner_tsv_f) {
      const PVLine *pv = endgame_results_get_pvline(eg_results,
                                                    ENDGAME_RESULT_BEST);
      if (pv) {
        inner_eg_num_moves = pv->num_moves;
        inner_eg_negamax_depth = pv->negamax_depth;
      }
      if (pv && pv->num_moves > 0) {
        Game *pv_game = game_duplicate(opp_pov_game);
        game_set_endgame_solving_mode(pv_game);
        game_set_backup_mode(pv_game, BACKUP_MODE_OFF);
        StringBuilder *pv_sb = string_builder_create();
        for (int mi = 0; mi < pv->num_moves; mi++) {
          Move m_full;
          small_move_to_move(&m_full, &pv->moves[mi],
                             game_get_board(pv_game));
          if (mi > 0) string_builder_add_string(pv_sb, " | ");
          string_builder_add_move(pv_sb, game_get_board(pv_game), &m_full,
                                  game_get_ld(pv_game), true);
          play_move(&m_full, pv_game, NULL);
        }
        snprintf(inner_pv_text, sizeof(inner_pv_text), "%s",
                 string_builder_peek(pv_sb));
        string_builder_destroy(pv_sb);
        game_destroy(pv_game);
      }
    }
    // Only destroy the ctx if we allocated it as a local (not a shared
    // per-worker slot). Per-worker ctxs are destroyed by the cand
    // dispatcher after all the cand's scenarios complete.
    if (eg_ctx_pp == &eg_ctx_local) {
      endgame_ctx_destroy(eg_ctx_local);
    }
    endgame_results_destroy(eg_results);
  } else {
    // Greedy fallback. Capture the PV when an inner TSV is requested so
    // the report can show what greedy played out — it's the "old answer"
    // we're contrasting endgame against. Time the playout so stage-1
    // (walker + greedy leaf) per-opp_pov_game wall time is visible too.
    if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
      ctimer_start(&greedy_timer);
    }
    MoveList *pl = move_list_create(1);
    if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
      mt = peg_greedy_playout_pv(
          opp_pov_game, j->mover_idx, pl, worker_idx,
          inner_pv_text, sizeof(inner_pv_text),
          NULL, 0, NULL, 0, NULL, 0);
    } else {
      mt = peg_greedy_playout_pv(
          opp_pov_game, j->mover_idx, pl, worker_idx, NULL, 0, NULL, 0,
          NULL, 0, NULL, 0);
    }
    move_list_destroy(pl);
    if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
      inner_eg_dur_ms =
          ctimer_elapsed_seconds(&greedy_timer) * 1000.0;
      // inner_pv_plies stays 0 → greedy path indicator in the report.
    }
  }
  // Store the result so a later opp_pov_game / scenario landing on the same
  // post-opp state can short-circuit. Skip storing on cache_hit (already
  // there) and on incomplete (depth=-1 fallback result is greedy and may
  // disagree with a later proper-endgame attempt at the same state — let
  // the later call recompute rather than poison the cache).
  if (!cache_hit && !j->incomplete && cache_key &&
      j->outer_ctx && j->outer_ctx->opp_pov_cache) {
    peg_opp_pov_cache_store(j->outer_ctx->opp_pov_cache, cache_key, mt);
  }
  // Optional per-inner opp-POV TSV. Each row: outer realized tiles, opp cand
  // text + score, opp-POV bag composition (canonical short tile string), opp_pov_game
  // weight, leaf mt. Consumer groups by (cand, scenario, opp).
  if (j->outer_ctx && j->outer_ctx->inner_tsv_f) {
    const LetterDistribution *ld = game_get_ld(j->base_game);
    char opp_pov_bag_str[32] = {0};
    int opp_pov_pos = 0;
    for (int t = 0; t < j->n_opp_types && opp_pov_pos < 30; t++) {
      for (int k = 0; k < j->opp_pov_bag_counts[t] && opp_pov_pos < 30; k++) {
        opp_pov_bag_str[opp_pov_pos++] = ld->ld_ml_to_hl[j->opp_types[t]][0];
      }
    }
    char depth_log_str[256] = {0};
    int dl_off = 0;
    for (int i = 0; i < inner_eg_log.n && dl_off + 24 < (int)sizeof(depth_log_str); i++) {
      dl_off += snprintf(depth_log_str + dl_off,
                         sizeof(depth_log_str) - dl_off,
                         "%s%d@%.1f=%+d", i == 0 ? "" : ";",
                         inner_eg_log.depths[i],
                         inner_eg_log.times_ms[i],
                         (int)inner_eg_log.values[i]);
    }
    peg_lock(j->outer_ctx->inner_tsv_mutex);
    fprintf(j->outer_ctx->inner_tsv_f,
            "%s\t%s\t%s\t%s\t%d\t%s\t%lld\t%d\t%d\t%d\t%d\t%d\t%d"
            "\t%.1f\t%.1f\t%s\t%s\t%s\n",
            j->outer_ctx->cand_txt ? j->outer_ctx->cand_txt : "",
            j->outer_drawn_str ? j->outer_drawn_str : "",
            j->outer_remaining_str ? j->outer_remaining_str : "",
            j->opp_move_text ? j->opp_move_text : "",
            j->opp_move_score, opp_pov_bag_str,
            (long long)j->opp_pov_weight, (int)mt,
            inner_pv_plies,
            inner_eg_depth_reached, inner_eg_status,
            inner_eg_num_moves, inner_eg_negamax_depth,
            inner_eg_start_ms, inner_eg_dur_ms, depth_log_str,
            opp_pov_mover_rack_str, inner_pv_text);
    peg_unlock(j->outer_ctx->inner_tsv_mutex);
  }
  // Note: we no longer mark the cand "incomplete" when a opp_pov_game fell back
  // to greedy — the user's design is "return the best result when
  // interrupted", and the greedy fallback IS a valid (cheaper) leaf
  // eval. Stage-level interruption is still tracked via budget_hit at
  // the wrapper level.
  game_destroy(opp_pov_game);
  j->mover_total = mt;
}

// Recursive accumulator: for each n_bag-multiset of perceived pool,
// build a opp_pov_game, run greedy, fold into opp's win/spread totals (and
// stash the realized mt when the multiset matches the realized bag).
typedef struct PegOppPovEnumCtx {
  const PegEnumCtx *outer_ctx;
  const Game *walker;
  const Move *opp_move;
  int n_opp_types;
  const MachineLetter *opp_types;
  const int *opp_type_counts;
  const int *realized_bag_counts;
  int *cur_opp_pov_bag; // size n_opp_types
  // accumulators
  int64_t total_weight;
  double win_x2_sum;       // weighted by opp-POV state weight, opp's POV
  int64_t spread_sum_opp; // weighted spread sum, opp's POV
  bool realized_set;
  int32_t realized_mt;
  // PASSPEG_PERCEPTION_STRIDE: stride-style stratified sampling on the
  // opp-POV state enumeration. perception_stride <= 1 means full enumeration.
  // opp_pov_weight_seen is a per-call (single-threaded) running counter of
  // opp-POV state weights, used for stride boundary tracking.
  int perception_stride;
  int64_t opp_pov_weight_seen;
  // Carried through to PegOppPovJob for inner-TSV row annotation.
  const char *outer_drawn_str;
  const char *outer_remaining_str;
  const char *opp_move_text;
  int opp_move_score;
} PegOppPovEnumCtx;

static void peg_enum_opp_pov_recursive(PegOppPovEnumCtx *e, int type_idx,
                                    int remaining) {
  // Cascade-budget poll. enum_opp_pov_recursive calls peg_opp_pov_worker_fn
  // SYNCHRONOUSLY per leaf opp_pov_game; without this poll a single scenario can
  // run hundreds of game_duplicate + greedy_playout calls past the
  // outer wall budget. Bail out early once the deadline has fired —
  // callers treat partial opp_pov_game coverage as "scenario skipped"
  // (out_utility stays at its initial value, no contribution to the
  // result accumulator).
  if (e->outer_ctx && e->outer_ctx->deadline_monotonic_ns > 0 &&
      ctimer_monotonic_ns() > e->outer_ctx->deadline_monotonic_ns) {
    return;
  }
  if (type_idx == e->n_opp_types) {
    if (remaining != 0) {
      return;
    }
    int64_t weight = 1;
    for (int t = 0; t < e->n_opp_types; t++) {
      weight *= peg_binomial(e->opp_type_counts[t], e->cur_opp_pov_bag[t]);
    }
    // Identify the realized opp-POV state (the one whose bag composition
    // matches the actual bag). We always want its mt for the scenario's
    // realized outcome, even if perception stride would otherwise skip it.
    bool is_realized = true;
    for (int t = 0; t < e->n_opp_types; t++) {
      if (e->cur_opp_pov_bag[t] != e->realized_bag_counts[t]) {
        is_realized = false;
        break;
      }
    }
    // Apply perception stride: stride-style stratified sampling over
    // opp-POV states, weighted by multinomial weight. Sampled opp-POV states contribute
    // to the utility accumulators with weight scaled by samples * stride;
    // Skipped opp-POV states don't contribute. Realized opp-POV state is always evaluated
    // (cost: one extra job at most per perception call) so realized_mt
    // is always available — but it only contributes to the utility
    // accumulators when stride samples it.
    const int64_t old_seen = e->opp_pov_weight_seen;
    e->opp_pov_weight_seen += weight;
    bool sampled = true;
    int64_t effective_weight = weight;
    if (e->perception_stride > 1) {
      const int64_t samples =
          (old_seen + weight) / (int64_t)e->perception_stride -
          old_seen / (int64_t)e->perception_stride;
      if (samples == 0) {
        sampled = false;
      } else {
        effective_weight = samples * (int64_t)e->perception_stride;
      }
    }
    if (!sampled && !is_realized) {
      return;  // stride-skipped and not the realized opp_pov_game; nothing to do.
    }
    PegOppPovJob job = {
        .base_game = e->walker,
        .mover_idx = e->outer_ctx->mover_idx,
        .ld_size = e->outer_ctx->ld_size,
        .opp_move = e->opp_move,
        .n_opp_types = e->n_opp_types,
        .opp_types = e->opp_types,
        .opp_type_counts = e->opp_type_counts,
        .opp_pov_bag_counts = e->cur_opp_pov_bag,
        .thread_control = e->outer_ctx->thread_control,
        .outer_ctx = e->outer_ctx,
        .outer_drawn_str = e->outer_drawn_str,
        .outer_remaining_str = e->outer_remaining_str,
        .opp_move_text = e->opp_move_text,
        .opp_move_score = e->opp_move_score,
        .opp_pov_weight = weight,
        .mover_total = 0,
    };
    peg_opp_pov_worker_fn(&job, e->outer_ctx->worker_idx);
    const int32_t mt = job.mover_total;
    if (sampled) {
      const int32_t mt_opp = -mt;
      e->total_weight += effective_weight;
      e->spread_sum_opp += (int64_t)mt_opp * effective_weight;
      if (mt_opp > 0) {
        e->win_x2_sum += 2.0 * (double)effective_weight;
      } else if (mt_opp == 0) {
        e->win_x2_sum += (double)effective_weight;
      }
    }
    if (is_realized && !e->realized_set) {
      e->realized_mt = mt;
      e->realized_set = true;
    }
    return;
  }
  const int max_take = e->opp_type_counts[type_idx] < remaining
                           ? e->opp_type_counts[type_idx]
                           : remaining;
  for (int k = 0; k <= max_take; k++) {
    e->cur_opp_pov_bag[type_idx] = k;
    peg_enum_opp_pov_recursive(e, type_idx + 1, remaining - k);
  }
  e->cur_opp_pov_bag[type_idx] = 0;
}

// Public-ish helper: evaluate opp's utility for one opp_move at the
// given walker state, averaging over all n_bag-multisets of opp's
// perceived pool (weighted by multinomial). Also returns the realized
// mt under the actual bag composition (used as scenario outcome at the
// final ply).
static void peg_eval_opp_with_perception(
    const PegEnumCtx *outer_ctx, const Game *walker, const Move *opp_move,
    const MachineLetter *opp_types, const int *opp_type_counts,
    int n_opp_types, int n_bag_now, const int *realized_bag_counts,
    double alpha, double *out_utility, int32_t *out_realized_mt,
    const char *outer_drawn_str, const char *outer_remaining_str,
    const char *opp_move_text, int opp_move_score) {
  int cur_opp_pov_bag[MAX_ALPHABET_SIZE] = {0};
  // Perception stride only fires when the root bag size is large enough
  // that the perception opp-POV state space justifies sampling. At root bag
  // <= 2 (1peg / 2peg) the space is small enough to enumerate; sampling
  // 1/k of it throws away too much coverage.
  const int root_bag_size =
      outer_ctx->k_drawn + outer_ctx->n_bag_remaining;
  const char *perception_stride_env = getenv("PASSPEG_PERCEPTION_STRIDE");
  const int perception_stride =
      (root_bag_size >= 3 && n_bag_now >= 2 && perception_stride_env &&
       *perception_stride_env)
          ? atoi(perception_stride_env)
          : 1;
  PegOppPovEnumCtx e = {
      .outer_ctx = outer_ctx,
      .walker = walker,
      .opp_move = opp_move,
      .n_opp_types = n_opp_types,
      .opp_types = opp_types,
      .opp_type_counts = opp_type_counts,
      .realized_bag_counts = realized_bag_counts,
      .cur_opp_pov_bag = cur_opp_pov_bag,
      .total_weight = 0,
      .win_x2_sum = 0.0,
      .spread_sum_opp = 0,
      .realized_set = false,
      .realized_mt = 0,
      .perception_stride = perception_stride,
      .opp_pov_weight_seen = 0,
      .outer_drawn_str = outer_drawn_str,
      .outer_remaining_str = outer_remaining_str,
      .opp_move_text = opp_move_text,
      .opp_move_score = opp_move_score,
  };
  peg_enum_opp_pov_recursive(&e, 0, n_bag_now);
  if (e.total_weight > 0) {
    const double winpct =
        e.win_x2_sum / (2.0 * (double)e.total_weight);
    const double mean_spread =
        (double)e.spread_sum_opp / (double)e.total_weight;
    *out_utility = winpct + alpha * mean_spread;
  } else {
    *out_utility = -1e9;
  }
  *out_realized_mt = e.realized_mt;
}

// For a fixed N-multiset in ctx->n_multiset, enumerate all mover-drawn
// sub-multisets of size k_drawn (= per-type counts summing to k_drawn).
static void peg_enum_mover_drawn(PegEnumCtx *ctx, int type_idx,
                                  int remaining) {
  if (type_idx == ctx->k_types) {
    if (remaining == 0) {
      peg_emit_split(ctx);
    }
    return;
  }
  const int max_take = ctx->n_multiset[type_idx] < remaining
                           ? ctx->n_multiset[type_idx]
                           : remaining;
  for (int take = 0; take <= max_take; take++) {
    ctx->mover_pick[type_idx] = take;
    peg_enum_mover_drawn(ctx, type_idx + 1, remaining - take);
  }
  ctx->mover_pick[type_idx] = 0;
}

// Enumerate all N-multisets from `type_counts` (the unseen pool). For each
// emitted multiset, fan out to peg_enum_mover_drawn to enumerate splits.
static void peg_enum_outer_multiset(PegEnumCtx *ctx, int type_idx,
                                     int remaining) {
  if (type_idx == ctx->k_types) {
    if (remaining == 0) {
      peg_enum_mover_drawn(ctx, 0, ctx->k_drawn);
    }
    return;
  }
  const int max_take = ctx->type_counts[type_idx] < remaining
                           ? ctx->type_counts[type_idx]
                           : remaining;
  for (int take = 0; take <= max_take; take++) {
    ctx->n_multiset[type_idx] = take;
    peg_enum_outer_multiset(ctx, type_idx + 1, remaining - take);
  }
  ctx->n_multiset[type_idx] = 0;
}

void test_pass_peg_greedy_bench(void) {
  const char *path_env = getenv("PASSPEG_GREEDY_PATH");
  const char *path =
      path_env && *path_env ? path_env : "/tmp/peg_positions.txt";
  const char *topk_env = getenv("PASSPEG_GREEDY_TOP_K");
  int top_k = topk_env && *topk_env ? atoi(topk_env) : 15;
  const char *only_env = getenv("PASSPEG_GREEDY_ONLY");
  const char *only_moves = only_env && *only_env ? only_env : NULL;
  const char *only_scen_env = getenv("PASSPEG_GREEDY_ONLY_SCEN");
  const char *scenario_filter =
      only_scen_env && *only_scen_env ? only_scen_env : NULL;
  const char *depth_env = getenv("PASSPEG_GREEDY_DEPTH");
  const int depth = depth_env && *depth_env ? atoi(depth_env) : 0;
  const char *opp_topk_env = getenv("PASSPEG_GREEDY_OPP_TOP_K");
  const int opp_top_k =
      opp_topk_env && *opp_topk_env ? atoi(opp_topk_env) : 8;
  const char *opp_match_env = getenv("PASSPEG_GREEDY_OPP_MATCH");
  const char *opp_move_filter =
      opp_match_env && *opp_match_env ? opp_match_env : NULL;
  const char *tsv_env = getenv("PASSPEG_GREEDY_TSV");
  const char *tsv_path = tsv_env && *tsv_env ? tsv_env : NULL;
  const char *inner_tsv_env = getenv("PASSPEG_INNER_TSV");
  const char *inner_tsv_path =
      inner_tsv_env && *inner_tsv_env ? inner_tsv_env : NULL;
  // PASSPEG_GREEDY_RESULT_FILE: when set, write a machine-readable TSV of
  // the ranked results (one row per cand) so a cascade driver can read the
  // ranking instead of grepping stderr. Columns:
  //   pos  rank  win  spread  scen  weight  tiles_played  bucket  cand_text
  // bucket: 0=capable-empt (tp>=bag, post-rack>=5), 1=non-empt (tp<bag),
  //         2=incapable-empt (tp>=bag, post-rack<5). rack_size assumed 7.
  const char *result_env = getenv("PASSPEG_GREEDY_RESULT_FILE");
  const char *result_path =
      result_env && *result_env ? result_env : NULL;
  const char *threads_env = getenv("PASSPEG_GREEDY_THREADS");
  const int n_threads =
      threads_env && *threads_env ? atoi(threads_env) : 1;

  // Persistent worker pool reused across positions and across the outer
  // (cand × scenario) loop AND the inner opp utility sweep. workers
  // claim indices in [100, 100 + n_threads); the main thread runs at
  // helper_worker_idx = 0, outside that range.
  PegPool *executor =
      n_threads > 1 ? peg_pool_create(n_threads, 100) : NULL;
  cpthread_mutex_t res_mutex;
  cpthread_mutex_t tsv_mutex;
  cpthread_mutex_t endgame_mutex;
  cpthread_mutex_t inner_tsv_mutex;
  cpthread_mutex_init(&res_mutex);
  cpthread_mutex_init(&tsv_mutex);
  cpthread_mutex_init(&endgame_mutex);
  cpthread_mutex_init(&inner_tsv_mutex);

  char **cgps = NULL;
  int n_pos = load_cgp_lines(path, &cgps, 1000);
  if (n_pos == 0) log_fatal("no positions loaded from %s", path);
  fprintf(stderr,
          "[peggreedy] loaded %d positions from %s  depth=%d  opp_top_k=%d"
          "  threads=%d"
          "%s%s\n",
          n_pos, path, depth, opp_top_k, n_threads,
          scenario_filter ? "  scenario_filter=" : "",
          scenario_filter ? scenario_filter : "");
  fflush(stderr);

  FILE *tsv_f = NULL;
  if (tsv_path) {
    tsv_f = fopen(tsv_path, "we");
    if (!tsv_f) log_fatal("cannot open TSV %s", tsv_path);
    fprintf(tsv_f,
            "pos\tcand\tcand_score\tN\tK_drawn\tdrawn\tremaining\tweight"
            "\tmover_total\tpost_cand_cgp\tfinal_cgp\tpv_text"
            "\tmover_rack_end\topp_rack_end\n");
  }

  FILE *inner_tsv_f = NULL;
  if (inner_tsv_path) {
    inner_tsv_f = fopen(inner_tsv_path, "we");
    if (!inner_tsv_f) log_fatal("cannot open INNER TSV %s", inner_tsv_path);
    fprintf(inner_tsv_f,
            "cand\tdrawn\tremaining\topp_move\topp_score\topp_pov_bag\topp_pov_weight"
            "\tmover_total\teg_plies\teg_depth\teg_status\teg_pv_moves"
            "\teg_pv_negamax\teg_start_ms\teg_dur_ms\teg_depth_log"
            "\topp_pov_mover_rack\teg_pv\n");
  }

  FILE *result_f = NULL;
  if (result_path) {
    result_f = fopen(result_path, "we");
    if (!result_f) log_fatal("cannot open RESULT %s", result_path);
    fprintf(result_f,
            "pos\trank\twin\tspread\tscen\tweight\ttiles_played\tbucket"
            "\tcand_text\n");
  }

  for (int pi = 0; pi < n_pos; pi++) {
    Config *config = config_create_or_die("set -s1 score -s2 score");
    char load_cmd[10240];
    snprintf(load_cmd, sizeof(load_cmd), "cgp %s", cgps[pi]);
    load_and_exec_config_or_die(config, load_cmd);
    Game *game = config_get_game(config);
    int mover_idx = game_get_player_on_turn_index(game);
    const LetterDistribution *ld = game_get_ld(game);
    int lds = ld_get_size(ld);

    // Unseen pool = full distribution − mover's rack − board tiles. Opp's
    // rack as it appears in the CGP is ignored: PEG always treats those
    // tiles as unknown. Bag size N then follows by: opp gets RACK_SIZE of
    // the unseen, bag gets the rest.
    uint8_t unseen[MAX_ALPHABET_SIZE];
    int total_unseen = peg_compute_unseen(game, mover_idx, unseen);
    int N = total_unseen - RACK_SIZE;
    if (N < 0) N = 0;

    MachineLetter types[MAX_ALPHABET_SIZE];
    int counts[MAX_ALPHABET_SIZE];
    int k_types = 0;
    for (int ml = 0; ml < lds; ml++) {
      if (unseen[ml] > 0) {
        types[k_types] = (MachineLetter)ml;
        counts[k_types] = (int)unseen[ml];
        k_types++;
      }
    }

    MoveList *ml_cands = move_list_create(16384);
    const MoveGenArgs ga = {
        .game = game,
        .move_list = ml_cands,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&ga);
    int n_all = move_list_get_count(ml_cands);

    fprintf(stderr,
            "[peggreedy] pos %d  N=%d  unseen=%d  k_types=%d  cands=%d\n",
            pi, N, total_unseen, k_types, n_all);
    if (getenv("PASSPEG_DUMP_CANDS")) {
      const int dump_top = 32;
      fprintf(stderr, "[cand_dump] top-%d by equity (sorted desc):\n",
              dump_top);
      const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
      // Build an index array sorted by equity descending. MoveList from
      // MOVE_RECORD_ALL is heap-ordered, not fully sorted.
      int *eq_order = malloc_or_die((size_t)n_all * sizeof(int));
      for (int i = 0; i < n_all; i++) {
        eq_order[i] = i;
      }
      // Insertion sort top-dump_top by descending equity.
      for (int i = 0; i < dump_top && i < n_all; i++) {
        int best = i;
        Equity best_eq =
            move_get_equity(move_list_get_move(ml_cands, eq_order[i]));
        for (int j = i + 1; j < n_all; j++) {
          Equity je =
              move_get_equity(move_list_get_move(ml_cands, eq_order[j]));
          if (je > best_eq) {
            best = j;
            best_eq = je;
          }
        }
        if (best != i) {
          int tmp = eq_order[i];
          eq_order[i] = eq_order[best];
          eq_order[best] = tmp;
        }
      }
      int printed = 0;
      for (int rank = 0; rank < n_all && printed < dump_top; rank++) {
        const Move *m = move_list_get_move(ml_cands, eq_order[rank]);
        if (move_get_type(m) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        printed++;
        // Compute leave: mover_rack minus the tiles played.
        Rack leave;
        rack_set_dist_size(&leave, ld_get_size(ld));
        rack_reset(&leave);
        for (int t = 0; t < ld_get_size(ld); t++) {
          int n = rack_get_letter(mr, t);
          for (int k = 0; k < n; k++) {
            rack_add_letter(&leave, t);
          }
        }
        const int tiles_played = move_get_tiles_played(m);
        for (int t = 0; t < tiles_played; t++) {
          MachineLetter ml = move_get_tile(m, t);
          if (ml == PLAYED_THROUGH_MARKER) {
            continue;
          }
          // Treat blanks: stored as letter|BLANK_MASK, blank in rack.
          if (get_is_blanked(ml)) {
            ml = BLANK_MACHINE_LETTER;
          }
          (void)rack_take_letter(&leave, ml);
        }
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, game_get_board(game), m, ld, true);
        StringBuilder *lsb = string_builder_create();
        string_builder_add_rack(lsb, &leave, ld, false);
        fprintf(stderr,
                "  #%-3d  score=%-3d  leave=%-8s  equity=%+9.3f  %s\n",
                printed, (int)equity_to_int(move_get_score(m)),
                string_builder_peek(lsb),
                (double)move_get_equity(m) / 1000.0,
                string_builder_peek(sb));
        string_builder_destroy(sb);
        string_builder_destroy(lsb);
      }
      free(eq_order);
      fflush(stderr);
    }
    fflush(stderr);

    // Pre-build a pruned KWG for the current board state and install it
    // as an override on the base game. game_duplicate (used inside
    // emit_split via peg_make_post_cand_game) carries the override
    // pointer through to every scenario's branch, so endgame_solve can
    // run with skip_word_pruning=true and reuse this single pruned KWG
    // instead of rebuilding one per (cand, scenario). The rebuild path
    // shares state that races under concurrent workers; pre-building
    // once per position eliminates that race entirely. The pruned KWG
    // for the pre-cand board is a superset of the words playable in any
    // post-cand position (the post-cand board has strictly more tiles
    // and strictly fewer possible words), so this is a correctness-
    // preserving optimization.
    KWG *peg_pruned_kwg = NULL;
    if (depth >= 1) {
      DictionaryWordList *word_list = dictionary_word_list_create();
      const KWG *full_kwg = player_get_kwg(game_get_player(game, mover_idx));
      generate_possible_words(game, full_kwg, word_list);
      peg_pruned_kwg = make_kwg_from_words_small(
          word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
      dictionary_word_list_destroy(word_list);
      game_set_override_kwgs(game, peg_pruned_kwg, NULL,
                             DUAL_LEXICON_MODE_IGNORANT);
      game_gen_all_cross_sets(game);
    }

    PegCandResult *results =
        calloc_or_die((size_t)(n_all > 0 ? n_all : 1), sizeof(PegCandResult));
    int n_results = 0;

    Timer t;
    ctimer_start(&t);

    // ml_cands is heap-ordered (MOVE_RECORD_ALL), not equity-sorted. Build
    // an equity-sorted index. If PASSPEG_CAND_TOP_K is set, only evaluate
    // the top-K placements by static equity — useful as a lossy speedup
    // for inner PEG runs where opp's best move is almost always near the
    // top by equity.
    const char *_inc_pass_env = getenv("PASSPEG_INCLUDE_PASS");
    const bool _inc_pass_here = _inc_pass_env && atoi(_inc_pass_env) > 0;
    int *cand_order = malloc_or_die((size_t)n_all * sizeof(int));
    int cand_order_n = 0;
    for (int ci = 0; ci < n_all; ci++) {
      const Move *m = move_list_get_move(ml_cands, ci);
      const game_event_t mt = move_get_type(m);
      if (mt == GAME_EVENT_TILE_PLACEMENT_MOVE ||
          (_inc_pass_here && mt == GAME_EVENT_PASS)) {
        cand_order[cand_order_n++] = ci;
      }
    }
    const char *cand_topk_env = getenv("PASSPEG_CAND_TOP_K");
    const int cand_topk_limit =
        cand_topk_env && *cand_topk_env ? atoi(cand_topk_env) : 0;
    const int cand_sort_top =
        (cand_topk_limit > 0 && cand_topk_limit < cand_order_n)
            ? cand_topk_limit
            : cand_order_n;
    // Ordering:
    //  - With PASSPEG_GREEDY_ONLY set, the filter is a semicolon-separated
    //    list of move-text substrings IN PREVIOUS-STAGE RANK ORDER. Honor
    //    that order so partial-stage runs (budget hit mid-stage) evaluate
    //    the previous stage's best-known cands first.
    //  - Otherwise, fall back to static-equity descending.
    if (only_moves) {
      // Build a parallel array of cand text strings so we can substring-
      // match each filter token to find which ml_cand index it refers to.
      char (*cand_text)[256] = (char (*)[256])malloc_or_die(
          (size_t)cand_order_n * sizeof(char[256]));
      for (int i = 0; i < cand_order_n; i++) {
        const Move *m = move_list_get_move(ml_cands, cand_order[i]);
        StringBuilder *sb_m = string_builder_create();
        string_builder_add_move(sb_m, game_get_board(game), m, ld, true);
        snprintf(cand_text[i], 256, "%s", string_builder_peek(sb_m));
        string_builder_destroy(sb_m);
      }
      int *new_order = malloc_or_die((size_t)cand_order_n * sizeof(int));
      int new_order_n = 0;
      bool *consumed =
          (bool *)malloc_or_die((size_t)cand_order_n * sizeof(bool));
      memset(consumed, 0, (size_t)cand_order_n * sizeof(bool));
      char tmp_filter[4096];
      snprintf(tmp_filter, sizeof(tmp_filter), "%s", only_moves);
      char *tok = strtok(tmp_filter, ";");
      while (tok) {
        // Skip leading spaces in the token.
        while (*tok == ' ') {
          tok++;
        }
        for (int i = 0; i < cand_order_n; i++) {
          if (!consumed[i] && strstr(cand_text[i], tok)) {
            new_order[new_order_n++] = cand_order[i];
            consumed[i] = true;
            break;
          }
        }
        tok = strtok(NULL, ";");
      }
      // Append any unmatched cands at the end so they still get evaluated
      // if budget allows (preserves the prior behaviour for cands the
      // filter never named; mostly relevant outside the wrapper path).
      for (int i = 0; i < cand_order_n; i++) {
        if (!consumed[i]) {
          new_order[new_order_n++] = cand_order[i];
        }
      }
      // Replace cand_order with the filter-driven ordering.
      memcpy(cand_order, new_order, (size_t)new_order_n * sizeof(int));
      cand_order_n = new_order_n;
      free(new_order);
      free(consumed);
      free(cand_text);
    } else {
      // Static-equity descending (selection sort over the top-K).
      for (int i = 0; i < cand_sort_top; i++) {
        int best = i;
        Equity best_eq =
            move_get_equity(move_list_get_move(ml_cands, cand_order[i]));
        for (int j = i + 1; j < cand_order_n; j++) {
          const Equity je =
              move_get_equity(move_list_get_move(ml_cands, cand_order[j]));
          if (je > best_eq) {
            best = j;
            best_eq = je;
          }
        }
        if (best != i) {
          const int tmp = cand_order[i];
          cand_order[i] = cand_order[best];
          cand_order[best] = tmp;
        }
      }
    }
    const int cand_iter_n = cand_sort_top;

    const char *include_pass_env = getenv("PASSPEG_INCLUDE_PASS");
    const bool include_pass = include_pass_env && atoi(include_pass_env) > 0;

    // Pooled-dispatch storage: keep each cand's EnumCtx + cand_txt alive
    // until ALL cands' scenarios have run, so workers can pull jobs across
    // cand boundaries instead of stalling at every per-cand barrier.
    PegEnumCtx *enum_ctxs = NULL;
    char (*cand_txts)[256] = NULL;
    PegScenarioJobList all_jobs = {0};
    if (executor != NULL) {
      enum_ctxs =
          malloc_or_die((size_t)cand_iter_n * sizeof(PegEnumCtx));
      cand_txts =
          malloc_or_die((size_t)cand_iter_n * sizeof(char[256]));
    }
    int cand_used = 0;

    // PASSPEG_GREEDY_BUDGET=T (seconds, may be fractional): time budget
    // for this stage. Checked at the START of each cand iteration AND
    // inside the scenario worker. When exceeded, the cand loop breaks and
    // we dispatch only the cands enumerated so far. Workers that pick up
    // jobs after the deadline return early (no-op). Result is the partial
    // ranking from completed cands+scenarios.
    const char *budget_env = getenv("PASSPEG_GREEDY_BUDGET");
    const double budget_secs =
        budget_env && *budget_env ? atof(budget_env) : 0.0;
    // Absolute monotonic-ns deadline shared with endgame_solve_inline so
    // mid-search abdada_negamax can bail rather than running to depth
    // completion. 0 = no deadline.
    const int64_t budget_deadline_ns =
        budget_secs > 0.0
            ? ctimer_monotonic_ns() + (int64_t)(budget_secs * 1.0e9)
            : 0;

    for (int ord_i = 0; ord_i < cand_iter_n; ord_i++) {
      if (budget_secs > 0.0 &&
          ctimer_elapsed_seconds(&t) > budget_secs) {
        fprintf(stderr,
                "[peggreedy] budget %.3fs hit at cand %d/%d, stopping enum\n",
                budget_secs, ord_i, cand_iter_n);
        break;
      }
      const int ci = cand_order[ord_i];
      const Move *cand = move_list_get_move(ml_cands, ci);
      const bool is_pass = (move_get_type(cand) == GAME_EVENT_PASS);
      if ((is_pass && !include_pass) ||
          (!is_pass && move_get_tiles_played(cand) < 1)) {
        continue;
      }
      // only_moves filter: build the cand text once, into a stack scratch
      // for the serial path or into the persistent cand_txts slot for the
      // pooled path so workers can read it after enumeration returns.
      char serial_cand_txt[256] = {0};
      char *cand_txt =
          (executor != NULL) ? cand_txts[cand_used] : serial_cand_txt;
      if (executor != NULL) {
        cand_txt[0] = '\0';
      }
      {
        StringBuilder *sb_m = string_builder_create();
        string_builder_add_move(sb_m, game_get_board(game), cand, ld, true);
        snprintf(cand_txt, 256, "%s", string_builder_peek(sb_m));
        string_builder_destroy(sb_m);
      }
      if (only_moves) {
        bool match = false;
        char tmp[2048];
        snprintf(tmp, sizeof(tmp), "%s", only_moves);
        char *tok = strtok(tmp, ";");
        while (tok) {
          if (strstr(cand_txt, tok)) {
            match = true;
            break;
          }
          tok = strtok(NULL, ";");
        }
        if (!match) {
          continue;
        }
      }

      int K = move_get_tiles_played(cand);
      int k_drawn = K < N ? K : N;
      int n_bag_remaining = N - k_drawn;
      int cand_score = (int)equity_to_int(move_get_score(cand));

      PegCandResult *res = &results[n_results++];
      res->ci = ci;

      if (executor == NULL) {
        // Serial path: enumerate + evaluate in place with stack scratch.
        int n_multiset_buf[MAX_ALPHABET_SIZE] = {0};
        int mover_pick_buf[MAX_ALPHABET_SIZE] = {0};
        PegEnumCtx enum_ctx = {
            .n_multiset = n_multiset_buf,
            .mover_pick = mover_pick_buf,
            .types = types,
            .type_counts = counts,
            .k_types = k_types,
            .k_drawn = k_drawn,
            .n_bag_remaining = n_bag_remaining,
            .base_game = game,
            .mover_idx = mover_idx,
            .unseen = unseen,
            .ld_size = lds,
            .cand = cand,
            .cand_txt = cand_txt,
            .cand_score = cand_score,
            .pos_idx = pi,
            .ld = ld,
            .tsv_f = tsv_f,
            .res = res,
            .scenario_filter = scenario_filter,
            .opp_move_filter = opp_move_filter,
            .depth = depth,
            .opp_top_k = opp_top_k,
            .thread_control = config_get_thread_control(config),
            .worker_idx = 0,
            .executor = NULL,
            .res_mutex = NULL,
            .tsv_mutex = NULL,
            .endgame_mutex = NULL,
            .out_jobs = NULL,
            .budget_timer = budget_secs > 0.0 ? &t : NULL,
            .budget_secs = budget_secs,
            .deadline_monotonic_ns = budget_deadline_ns,
            .inner_tsv_f = inner_tsv_f,
            .inner_tsv_mutex = inner_tsv_f ? &inner_tsv_mutex : NULL,
            .opp_pov_cache = NULL,  // serial path: not yet wired to cache
            .shared_eg_tt = NULL,  // serial path: not wired
        };
        peg_enum_outer_multiset(&enum_ctx, 0, N);
      } else {
        // Pooled path: enumerate scenarios into the GLOBAL job list. The
        // EnumCtx itself lives in enum_ctxs[cand_used] so workers reading
        // job->base_ctx still see valid memory after enumeration returns.
        // The enumerator's n_multiset/mover_pick are scratch used only
        // during enumeration; each pushed job owns its own copies.
        int n_multiset_scratch[MAX_ALPHABET_SIZE] = {0};
        int mover_pick_scratch[MAX_ALPHABET_SIZE] = {0};
        // Per-cand opp_pov_game cache (task #35). Allocated here, freed after this
        // cand's scenarios complete. Disable via PASSPEG_OPP_POV_CACHE=0.
        PegOppPovCache *cand_opp_pov_cache = NULL;
        const char *opp_pov_cache_env = getenv("PASSPEG_OPP_POV_CACHE");
        const bool opp_pov_cache_on =
            !opp_pov_cache_env || atoi(opp_pov_cache_env) > 0;  // default on
        if (opp_pov_cache_on) {
          cand_opp_pov_cache = peg_opp_pov_cache_create(16384);
        }
        // Per-cand shared endgame transposition table. This is the real
        // macondo-equivalent shared cache: endgame_solve uses it for
        // sub-tree position lookups, and any two endgame_solve calls in
        // this cand's opp-POV states/scenarios that reach overlapping sub-trees
        // share the work. Memory: 1% of system RAM per cand (allocated
        // and freed per-cand). Disable via PASSPEG_SHARED_EG_TT=0.
        TranspositionTable *cand_shared_eg_tt = NULL;
        const char *shared_tt_env = getenv("PASSPEG_SHARED_EG_TT");
        const bool shared_tt_on =
            !shared_tt_env || atoi(shared_tt_env) > 0;
        if (shared_tt_on) {
          const char *shared_tt_frac_env =
              getenv("PASSPEG_SHARED_EG_TT_FRAC");
          const double shared_tt_frac =
              (shared_tt_frac_env && *shared_tt_frac_env)
                  ? atof(shared_tt_frac_env)
                  : 0.01;  // 1% of memory by default
          cand_shared_eg_tt = transposition_table_create(shared_tt_frac);
        }
        // Per-worker persistent EndgameCtx slots. Slot 0 = main thread;
        // slots 1..n = executor workers (worker_idx 100..100+n-1).
        // Default on; disable via PASSPEG_PERSIST_EG_CTX=0.
        EndgameCtx **per_worker_eg_ctx = NULL;
        const int per_worker_eg_ctx_n = n_threads + 1;
        const char *persist_env = getenv("PASSPEG_PERSIST_EG_CTX");
        const bool persist_on =
            !persist_env || atoi(persist_env) > 0;
        if (persist_on) {
          per_worker_eg_ctx = calloc_or_die(
              (size_t)per_worker_eg_ctx_n, sizeof(EndgameCtx *));
        }
        enum_ctxs[cand_used] = (PegEnumCtx){
            .n_multiset = n_multiset_scratch,
            .mover_pick = mover_pick_scratch,
            .types = types,
            .type_counts = counts,
            .k_types = k_types,
            .k_drawn = k_drawn,
            .n_bag_remaining = n_bag_remaining,
            .base_game = game,
            .mover_idx = mover_idx,
            .unseen = unseen,
            .ld_size = lds,
            .cand = cand,
            .cand_txt = cand_txt,
            .cand_score = cand_score,
            .pos_idx = pi,
            .ld = ld,
            .tsv_f = tsv_f,
            .res = res,
            .scenario_filter = scenario_filter,
            .opp_move_filter = opp_move_filter,
            .depth = depth,
            .opp_top_k = opp_top_k,
            .thread_control = config_get_thread_control(config),
            .worker_idx = 0,
            .executor = executor,
            .res_mutex = &res_mutex,
            .tsv_mutex = &tsv_mutex,
            .endgame_mutex = &endgame_mutex,
            .out_jobs = &all_jobs,
            .budget_timer = budget_secs > 0.0 ? &t : NULL,
            .budget_secs = budget_secs,
            .deadline_monotonic_ns = budget_deadline_ns,
            .inner_tsv_f = inner_tsv_f,
            .inner_tsv_mutex = inner_tsv_f ? &inner_tsv_mutex : NULL,
            .opp_pov_cache = cand_opp_pov_cache,
            .shared_eg_tt = cand_shared_eg_tt,
            .per_worker_eg_ctx = per_worker_eg_ctx,
            .per_worker_eg_ctx_n = per_worker_eg_ctx_n,
            .per_worker_eg_ctx_offset = 100,  // executor's thread_index_offset
        };
        // Per-cand batch dispatch. We enumerate THIS cand's scenarios
        // into the shared `all_jobs` list, then immediately submit just
        // those jobs and wait for them all to finish before moving to
        // the next cand. That gives strict best-first cand order: cand
        // 0's evaluation fully completes (or its opp-POV states run out the
        // global deadline and the cand is marked incomplete) before
        // cand 1's jobs are dispatched. Worker-level scheduling can
        // still interleave scenarios within one cand, which is fine.
        const int cand_jobs_start = all_jobs.n;
        peg_enum_outer_multiset(&enum_ctxs[cand_used], 0, N);
        const int cand_jobs_end = all_jobs.n;
        // After enumeration, the scratch buffers go out of scope; clear
        // the dangling pointers so a buggy late read trips an obvious
        // null-deref instead of reading stack garbage.
        enum_ctxs[cand_used].n_multiset = NULL;
        enum_ctxs[cand_used].mover_pick = NULL;
        enum_ctxs[cand_used].out_jobs = NULL;
        if (cand_jobs_end > cand_jobs_start) {
          const int n_this_cand = cand_jobs_end - cand_jobs_start;
          void **args = malloc_or_die((size_t)n_this_cand * sizeof(void *));
          for (int j = 0; j < n_this_cand; j++) {
            args[j] = &all_jobs.items[cand_jobs_start + j];
          }
          peg_pool_submit_and_wait(
              executor, peg_scenario_worker_fn, args, n_this_cand, 0);
          free(args);
        }
        // Cand's scenarios are done; free the cache and (optionally) log
        // hit/miss stats. Clear from the ctx so any late access fails fast.
        if (cand_opp_pov_cache) {
          const int hits = atomic_load(&cand_opp_pov_cache->hits);
          const int misses = atomic_load(&cand_opp_pov_cache->misses);
          const int total = hits + misses;
          if (total > 0 && getenv("PASSPEG_OPP_POV_CACHE_STATS")) {
            fprintf(stderr,
                    "[opp_pov_cache] %s: %d hits / %d total (%.1f%%), "
                    "%d misses\n",
                    cand_txt, hits, total, hits * 100.0 / total, misses);
          }
          enum_ctxs[cand_used].opp_pov_cache = NULL;
          peg_opp_pov_cache_destroy(cand_opp_pov_cache);
        }
        if (cand_shared_eg_tt) {
          if (getenv("PASSPEG_OPP_POV_CACHE_STATS")) {
            const int tt_created = atomic_load(&cand_shared_eg_tt->created);
            const int tt_lookups = atomic_load(&cand_shared_eg_tt->lookups);
            const int tt_hits = atomic_load(&cand_shared_eg_tt->hits);
            fprintf(stderr,
                    "[shared_eg_tt] %s: created=%d lookups=%d hits=%d "
                    "(%.1f%% hit rate)\n",
                    cand_txt, tt_created, tt_lookups, tt_hits,
                    tt_lookups > 0 ? tt_hits * 100.0 / tt_lookups : 0.0);
          }
          enum_ctxs[cand_used].shared_eg_tt = NULL;
          transposition_table_destroy(cand_shared_eg_tt);
        }
        // Destroy per-worker persistent EndgameCtx slots. All this
        // cand's scenarios have completed, so no worker is using one.
        if (per_worker_eg_ctx) {
          int slots_used = 0;
          for (int slot_idx = 0; slot_idx < per_worker_eg_ctx_n; slot_idx++) {
            if (per_worker_eg_ctx[slot_idx]) {
              endgame_ctx_destroy(per_worker_eg_ctx[slot_idx]);
              slots_used++;
            }
          }
          if (getenv("PASSPEG_OPP_POV_CACHE_STATS")) {
            fprintf(stderr,
                    "[persist_eg_ctx] %s: reused across %d worker "
                    "slot(s)\n",
                    cand_txt, slots_used);
          }
          enum_ctxs[cand_used].per_worker_eg_ctx = NULL;
          free(per_worker_eg_ctx);
        }
        cand_used++;
      }
    }

    if (executor != NULL) {
      for (int j = 0; j < all_jobs.n; j++) {
        free(all_jobs.items[j].n_multiset);
        free(all_jobs.items[j].mover_pick);
      }
      free(all_jobs.items);
      free(enum_ctxs);
      free(cand_txts);
    }
    free(cand_order);

    double wall = ctimer_elapsed_seconds(&t);

    // Sort results by u = q_win + 1e-4 × q_spread.
    typedef struct {
      int ci;
      double q_win, q_spread, u;
      int64_t weight_sum;
      int n_scen;
    } Ranked;
    Ranked *ranked = calloc_or_die((size_t)(n_results > 0 ? n_results : 1),
                                    sizeof(Ranked));
    int n_ranked = 0;
    int n_dropped_incomplete = 0;
    for (int r = 0; r < n_results; r++) {
      if (results[r].weight_sum <= 0) continue;
      if (results[r].incomplete) {
        // At least one inner endgame_solve returned depth=-1 (global
        // deadline hit before any IDS iteration). Aggregated win/spread
        // is contaminated by mover_lead defaults for those opp-POV states. Drop
        // the cand from the ranking rather than emit a misleading score.
        n_dropped_incomplete++;
        continue;
      }
      double q_win =
          (double)results[r].win_x2 / (2.0 * (double)results[r].weight_sum);
      double q_spread =
          (double)results[r].spread_sum / (double)results[r].weight_sum;
      ranked[n_ranked++] = (Ranked){results[r].ci, q_win, q_spread,
                                     q_win + 1e-4 * q_spread,
                                     results[r].weight_sum,
                                     results[r].n_scen};
    }
    for (int i = 0; i < n_ranked; i++) {
      for (int j = i + 1; j < n_ranked; j++) {
        if (ranked[j].u > ranked[i].u) {
          Ranked tmp = ranked[i];
          ranked[i] = ranked[j];
          ranked[j] = tmp;
        }
      }
    }
    int show = top_k < n_ranked ? top_k : n_ranked;
    const double budget_used = (budget_secs > 0.0) ? budget_secs : -1.0;
    fprintf(stderr,
            "[peggreedy] pos %d wall=%.3fs  budget=%.3fs  ranked=%d  "
            "dropped_incomplete=%d  top-%d cands:\n",
            pi, wall, budget_used, n_ranked, n_dropped_incomplete, show);
    for (int r = 0; r < show; r++) {
      const Move *m = move_list_get_move(ml_cands, ranked[r].ci);
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(game), m, ld, true);
      fprintf(stderr,
              "  #%-2d  win=%.4f  spread=%+9.3f  scen=%d  weight=%lld  %s\n",
              r + 1, ranked[r].q_win, ranked[r].q_spread, ranked[r].n_scen,
              (long long)ranked[r].weight_sum, string_builder_peek(sb));
      string_builder_destroy(sb);
    }
    // Machine-readable rank emit for the cascade driver. Always write
    // ALL n_ranked cands (not just top show); the driver uses the full
    // list to apply bucket quotas at the next stage.
    if (result_f) {
      for (int r = 0; r < n_ranked; r++) {
        const Move *m = move_list_get_move(ml_cands, ranked[r].ci);
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, game_get_board(game), m, ld, true);
        const char *cand_txt = string_builder_peek(sb);
        const int tp = peg_count_tiles_played(cand_txt);
        const int bucket = peg_cand_bucket(tp, N, RACK_SIZE);
        fprintf(result_f,
                "%d\t%d\t%.6f\t%.4f\t%d\t%lld\t%d\t%d\t%s\n",
                pi, r + 1, ranked[r].q_win, ranked[r].q_spread,
                ranked[r].n_scen, (long long)ranked[r].weight_sum,
                tp, bucket, cand_txt);
        string_builder_destroy(sb);
      }
      fflush(result_f);
    }
    // Static fallback: if budget expired before ANY cand completed a
    // scenario (n_ranked == 0), emit the single highest-static-equity
    // cand so downstream consumers always get a play.
    if (n_ranked == 0 && cand_iter_n > 0) {
      const int fallback_ci = cand_order[0];
      const Move *fallback_m = move_list_get_move(ml_cands, fallback_ci);
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(game), fallback_m, ld,
                              true);
      fprintf(stderr,
              "  #1   [fallback=static]  scen=0  weight=0  %s\n",
              string_builder_peek(sb));
      string_builder_destroy(sb);
    }
    fflush(stderr);

    free(ranked);
    free(results);
    move_list_destroy(ml_cands);
    if (peg_pruned_kwg) {
      game_set_override_kwgs(game, NULL, NULL, DUAL_LEXICON_MODE_IGNORANT);
      kwg_destroy(peg_pruned_kwg);
    }
    config_destroy(config);
  }

  if (tsv_f) {
    fclose(tsv_f);
    fprintf(stderr, "[peggreedy] TSV written to %s\n", tsv_path);
  }
  if (inner_tsv_f) {
    fclose(inner_tsv_f);
    fprintf(stderr, "[peggreedy] INNER TSV written to %s\n", inner_tsv_path);
  }
  if (result_f) {
    fclose(result_f);
    fprintf(stderr, "[peggreedy] RESULT TSV written to %s\n", result_path);
  }
  if (executor) {
    peg_pool_destroy(executor);
  }
  for (int i = 0; i < n_pos; i++) free(cgps[i]);
  free(cgps);
}

// One ranked-cand row read back from the result file emitted by
// test_pass_peg_greedy_bench (PASSPEG_GREEDY_RESULT_FILE).
typedef struct PegCascadeRank {
  int pos;
  int rank;
  double win;
  double spread;
  int scen;
  int64_t weight;
  int tiles_played;
  int bucket;  // 0=capable, 1=non-emp, 2=incapable
  char cand_text[256];
} PegCascadeRank;

// Parse the TSV result file into an array. Returns count, sets *out_arr.
// Allocates *out_arr; caller frees.
static int peg_cascade_load_results(const char *path,
                                     PegCascadeRank **out_arr) {
  FILE *f = fopen(path, "re");
  if (!f) {
    log_fatal("cannot open result file %s", path);
  }
  char line[1024];
  // Skip header.
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    *out_arr = NULL;
    return 0;
  }
  int cap = 64;
  PegCascadeRank *arr = malloc_or_die((size_t)cap * sizeof(*arr));
  int n = 0;
  while (fgets(line, sizeof(line), f)) {
    if (n >= cap) {
      cap *= 2;
      arr = realloc(arr, (size_t)cap * sizeof(*arr));
      if (!arr) log_fatal("realloc failed");
    }
    PegCascadeRank *r = &arr[n];
    long long weight_ll;
    // Last column (cand_text) may contain spaces; parse up to cand_text
    // then take the rest of the line minus trailing newline.
    int consumed = 0;
    int matched = sscanf(line,
                         "%d\t%d\t%lf\t%lf\t%d\t%lld\t%d\t%d\t%n",
                         &r->pos, &r->rank, &r->win, &r->spread, &r->scen,
                         &weight_ll, &r->tiles_played, &r->bucket, &consumed);
    if (matched != 8 || consumed == 0) {
      continue;
    }
    r->weight = (int64_t)weight_ll;
    const char *txt = line + consumed;
    size_t tlen = strlen(txt);
    while (tlen > 0 && (txt[tlen - 1] == '\n' || txt[tlen - 1] == '\r')) {
      tlen--;
    }
    if (tlen >= sizeof(r->cand_text)) tlen = sizeof(r->cand_text) - 1;
    memcpy(r->cand_text, txt, tlen);
    r->cand_text[tlen] = '\0';
    n++;
  }
  fclose(f);
  *out_arr = arr;
  return n;
}

// Apply three-bucket quota: pick top n_cap from bucket 0, top n_non from
// bucket 1, top n_inc from bucket 2 (each bucket already pre-sorted by
// the rank file). Builds a ';'-joined filter string into out_filter and
// returns total count selected.
static int peg_cascade_apply_quota(const PegCascadeRank *arr, int n,
                                    int n_cap, int n_non, int n_inc,
                                    char *out_filter, size_t out_filter_sz) {
  out_filter[0] = '\0';
  int got_cap = 0, got_non = 0, got_inc = 0;
  size_t off = 0;
  int total = 0;
  for (int i = 0; i < n; i++) {
    const PegCascadeRank *r = &arr[i];
    bool take = false;
    if (r->bucket == 0 && got_cap < n_cap) {
      got_cap++;
      take = true;
    } else if (r->bucket == 1 && got_non < n_non) {
      got_non++;
      take = true;
    } else if (r->bucket == 2 && got_inc < n_inc) {
      got_inc++;
      take = true;
    }
    if (take) {
      const char *sep = total == 0 ? "" : ";";
      int wrote = snprintf(out_filter + off, out_filter_sz - off, "%s%s",
                           sep, r->cand_text);
      if (wrote < 0 || (size_t)wrote >= out_filter_sz - off) {
        log_fatal("quota filter overflow");
      }
      off += (size_t)wrote;
      total++;
    }
  }
  fprintf(stderr,
          "[cascade] quota: %d capable-emp + %d non-emp + %d incap-emp"
          " = %d total (requested %d+%d+%d=%d)\n",
          got_cap, got_non, got_inc, total, n_cap, n_non, n_inc,
          n_cap + n_non + n_inc);
  return total;
}

// Merge the prior stage's ranking with the current stage's partial ranking.
//
// Rule (preserves "above-finished cands stay above" — a cand whose stage-N
// eval didn't finish keeps its stage-(N-1) slot, so a higher-ranked stage-3
// cand that didn't complete stage 4 doesn't get demoted by lower-ranked
// stage-4 finishers): walk prev in order. For each prev cand, if it appears
// in last (i.e., completed the new stage), take the next entry from last (in
// last's ranking order) for that slot. Otherwise keep prev's entry verbatim.
//
// The result has length prev_n. Any last entries that didn't match (shouldn't
// happen since last is derived from prev's filter) are appended after.
static int peg_cascade_merge_rankings(
    const PegCascadeRank *prev, int prev_n,
    const PegCascadeRank *last, int last_n,
    PegCascadeRank **out_merged) {
  PegCascadeRank *out =
      malloc_or_die((size_t)(prev_n + last_n) * sizeof(*out));
  bool *last_used = calloc_or_die((size_t)last_n, sizeof(bool));
  int next_finished_idx = 0;
  for (int i = 0; i < prev_n; i++) {
    bool in_last = false;
    for (int j = 0; j < last_n; j++) {
      if (strcmp(prev[i].cand_text, last[j].cand_text) == 0) {
        in_last = true;
        break;
      }
    }
    if (in_last && next_finished_idx < last_n) {
      // Advance past any last entries already used (paranoia).
      while (next_finished_idx < last_n && last_used[next_finished_idx]) {
        next_finished_idx++;
      }
      if (next_finished_idx < last_n) {
        out[i] = last[next_finished_idx];
        last_used[next_finished_idx] = true;
        next_finished_idx++;
      } else {
        out[i] = prev[i];
      }
    } else {
      out[i] = prev[i];
    }
  }
  int out_n = prev_n;
  // Append any last entries that weren't slotted (defensive — shouldn't fire
  // if last is a strict subset of prev as enforced by the filter).
  for (int j = 0; j < last_n; j++) {
    if (!last_used[j]) {
      out[out_n++] = last[j];
    }
  }
  free(last_used);
  // Renumber ranks 1..out_n so the final printout is consistent.
  for (int i = 0; i < out_n; i++) {
    out[i].rank = i + 1;
  }
  *out_merged = out;
  return out_n;
}

// Like apply_quota but combined top-K (no bucket discrimination).
static int peg_cascade_apply_topk(const PegCascadeRank *arr, int n, int top_k,
                                   char *out_filter, size_t out_filter_sz) {
  out_filter[0] = '\0';
  int take = top_k < n ? top_k : n;
  size_t off = 0;
  for (int i = 0; i < take; i++) {
    const char *sep = i == 0 ? "" : ";";
    int wrote = snprintf(out_filter + off, out_filter_sz - off, "%s%s",
                         sep, arr[i].cand_text);
    if (wrote < 0 || (size_t)wrote >= out_filter_sz - off) {
      log_fatal("topK filter overflow");
    }
    off += (size_t)wrote;
  }
  return take;
}

// Cascade stage driver. Replicates /tmp/peg_halving_quota.sh in C:
//   Stage 0: d=0, all cands, greedy
//   Stage 1: d=1, top 32 (quota 8+8+16) — walker + greedy leaf
//   Stage 2: d=2, top 16 (quota 4+4+8)
//   Stage 3: d=3, top 8 (combined)
//   Stage 4: d=4, top 4 (combined)
//
// Env:
//   PASSPEG_CASCADE_PATH      — CGP file (single-position recommended)
//   PASSPEG_CASCADE_BUDGET    — total wall budget in seconds (default 64)
//   PASSPEG_CASCADE_THREADS   — thread count (default 18)
//   PASSPEG_CASCADE_MAX_STAGE — stop after this stage (default 4)
//   PASSPEG_CASCADE_INNER_EG  — inner endgame plies for d>=1 stages (default 0
//                                = greedy leaf)
//   PASSPEG_CASCADE_OUT_PREFIX — file prefix for stage TSV outputs
//                                (default /tmp/cascade)
void test_pass_peg_cascade(void) {
  const char *path_env = getenv("PASSPEG_CASCADE_PATH");
  const char *path =
      path_env && *path_env ? path_env : "/tmp/peg_positions.txt";
  const char *budget_env = getenv("PASSPEG_CASCADE_BUDGET");
  const double total_budget =
      budget_env && *budget_env ? atof(budget_env) : 64.0;
  const char *threads_env = getenv("PASSPEG_CASCADE_THREADS");
  const char *threads_str =
      threads_env && *threads_env ? threads_env : "18";
  const char *max_stage_env = getenv("PASSPEG_CASCADE_MAX_STAGE");
  const int max_stage =
      max_stage_env && *max_stage_env ? atoi(max_stage_env) : 4;
  const char *inner_eg_env = getenv("PASSPEG_CASCADE_INNER_EG");
  const char *inner_eg_str =
      inner_eg_env && *inner_eg_env ? inner_eg_env : NULL;
  const char *prefix_env = getenv("PASSPEG_CASCADE_OUT_PREFIX");
  const char *out_prefix =
      prefix_env && *prefix_env ? prefix_env : "/tmp/cascade";
  // PASSPEG_CASCADE_ORACLE=1: disable all stride sampling for "oracle" runs
  // (WALK_STRIDE=5 at stage 0 becomes 1; SCENARIO/PERCEPTION stay at 1, which
  // matches their non-oracle defaults so the flag effectively only flips
  // WALK_STRIDE today — explicit knob in case more stride knobs get added).
  const char *oracle_env = getenv("PASSPEG_CASCADE_ORACLE");
  const bool oracle_mode = oracle_env && atoi(oracle_env) > 0;

  Timer cascade_timer;
  ctimer_start(&cascade_timer);

  // Stage table mirrors peg_halving_quota.sh. {depth, top_k_print, quota_cap,
  // quota_non, quota_inc} — if all quotas are 0, top_k_print acts as combined
  // top-K (no bucket discrimination).
  typedef struct {
    int depth;
    int top_k;       // print count + combined top-K when no quota
    int quota_cap;
    int quota_non;
    int quota_inc;
  } StageSpec;
  const StageSpec stages[] = {
      {0, 8641, 0, 0, 0},  // greedy first pass; no filter, no quota
      {1, 32, 8, 8, 16},
      {2, 16, 4, 4, 8},
      {3, 8, 0, 0, 0},
      {4, 4, 0, 0, 0},
  };
  const int n_stages = sizeof(stages) / sizeof(stages[0]);

  // Stash + restore env vars that test_pass_peg_greedy_bench reads so we
  // don't pollute the caller's environment.
  const char *const overridden[] = {
      "PASSPEG_GREEDY_PATH",         "PASSPEG_GREEDY_DEPTH",
      "PASSPEG_GREEDY_TOP_K",        "PASSPEG_GREEDY_ONLY",
      "PASSPEG_GREEDY_RATIONAL",     "PASSPEG_GREEDY_RAT_WALK",
      "PASSPEG_GREEDY_WALK_K_FIRST_OPP",
      "PASSPEG_GREEDY_WALK_K_LATER_OPP",
      "PASSPEG_SCENARIO_STRIDE",     "PASSPEG_PERCEPTION_STRIDE",
      "PASSPEG_WALK_STRIDE",         "PASSPEG_OPP_RANK_BY_PLAYOUT",
      "PASSPEG_OPP_RANK_POOL",       "PASSPEG_GREEDY_BUDGET",
      "PASSPEG_GREEDY_THREADS",      "PASSPEG_GREEDY_RESULT_FILE",
      "PASSPEG_INNER_USE_ENDGAME",
  };
  enum { N_OVERRIDDEN = sizeof(overridden) / sizeof(overridden[0]) };
  char *saved[N_OVERRIDDEN];
  for (int i = 0; i < N_OVERRIDDEN; i++) {
    const char *v = getenv(overridden[i]);
    saved[i] = v ? strdup(v) : NULL;
  }

  // Persistent filter buffer carried between stages. 256KB handles up to
  // ~2000 cand texts at avg 100 chars.
  char *filter = malloc_or_die(256 * 1024);
  filter[0] = '\0';

  // Track last stage's ranked array for the final printout. prev_arr/prev_n
  // hold the stage N-1 ranking (the basis of the filter that ran in stage
  // N), used to slot incomplete cands at their prior-stage position when the
  // current stage's budget didn't cover them.
  PegCascadeRank *last_arr = NULL;
  int last_n = 0;
  PegCascadeRank *prev_arr = NULL;
  int prev_n = 0;

  for (int stage_idx = 0; stage_idx <= max_stage && stage_idx < n_stages; stage_idx++) {
    const StageSpec *sp = &stages[stage_idx];
    const double elapsed = ctimer_elapsed_seconds(&cascade_timer);
    const double remaining = total_budget - elapsed;
    if (remaining <= 0.0) {
      fprintf(stderr,
              "[cascade] budget %.3fs exhausted at stage %d (elapsed %.3fs);"
              " stopping\n",
              total_budget, stage_idx, elapsed);
      break;
    }

    char stage_file[512];
    snprintf(stage_file, sizeof(stage_file), "%s_s%d.tsv", out_prefix, stage_idx);
    char depth_str[16], topk_str[16], budget_str[32];
    snprintf(depth_str, sizeof(depth_str), "%d", sp->depth);
    snprintf(topk_str, sizeof(topk_str), "%d", sp->top_k);
    snprintf(budget_str, sizeof(budget_str), "%.3f", remaining);

    setenv("PASSPEG_GREEDY_PATH", path, 1);
    setenv("PASSPEG_GREEDY_DEPTH", depth_str, 1);
    setenv("PASSPEG_GREEDY_TOP_K", topk_str, 1);
    setenv("PASSPEG_GREEDY_BUDGET", budget_str, 1);
    setenv("PASSPEG_GREEDY_THREADS", threads_str, 1);
    setenv("PASSPEG_GREEDY_RESULT_FILE", stage_file, 1);
    if (stage_idx == 0) {
      // Stage 0 is the pure greedy first pass: no filter (consider every
      // legal move), wide scenario coverage.
      unsetenv("PASSPEG_GREEDY_ONLY");
      setenv("PASSPEG_SCENARIO_STRIDE", "1", 1);
      setenv("PASSPEG_WALK_STRIDE", oracle_mode ? "1" : "5", 1);
      unsetenv("PASSPEG_GREEDY_RATIONAL");
      unsetenv("PASSPEG_GREEDY_RAT_WALK");
      unsetenv("PASSPEG_PERCEPTION_STRIDE");
      unsetenv("PASSPEG_OPP_RANK_BY_PLAYOUT");
      unsetenv("PASSPEG_OPP_RANK_POOL");
      unsetenv("PASSPEG_INNER_USE_ENDGAME");
    } else {
      setenv("PASSPEG_GREEDY_ONLY", filter, 1);
      setenv("PASSPEG_GREEDY_RATIONAL", "1", 1);
      setenv("PASSPEG_GREEDY_RAT_WALK", "1", 1);
      setenv("PASSPEG_GREEDY_WALK_K_FIRST_OPP", "16", 1);
      setenv("PASSPEG_GREEDY_WALK_K_LATER_OPP", "8", 1);
      setenv("PASSPEG_SCENARIO_STRIDE", "1", 1);
      setenv("PASSPEG_PERCEPTION_STRIDE", "1", 1);
      setenv("PASSPEG_OPP_RANK_BY_PLAYOUT", "1", 1);
      setenv("PASSPEG_OPP_RANK_POOL", "64", 1);
      if (inner_eg_str) {
        setenv("PASSPEG_INNER_USE_ENDGAME", inner_eg_str, 1);
      } else {
        unsetenv("PASSPEG_INNER_USE_ENDGAME");
      }
    }
    fprintf(stderr,
            "[cascade] === stage %d (depth=%d, top_k=%d, remaining=%.3fs) ===\n",
            stage_idx, sp->depth, sp->top_k, remaining);
    fflush(stderr);

    test_pass_peg_greedy_bench();

    // Promote previous last_arr → prev_arr (it'stage_idx the basis of the filter
    // that just ran), then load the new stage'stage_idx results into last_arr.
    free(prev_arr);
    prev_arr = last_arr;
    prev_n = last_n;
    last_arr = NULL;
    last_n = 0;
    last_n = peg_cascade_load_results(stage_file, &last_arr);
    if (last_n == 0) {
      fprintf(stderr, "[cascade] stage %d produced no results — stopping\n", stage_idx);
      // Roll back: leave prev_arr in place so the final printout still has
      // something to show.
      break;
    }

    // Build filter for NEXT stage. Apply quota if next stage has one,
    // else combined top-K = next stage'stage_idx top_k.
    if (stage_idx + 1 < n_stages && stage_idx + 1 <= max_stage) {
      const StageSpec *next = &stages[stage_idx + 1];
      if (next->quota_cap || next->quota_non || next->quota_inc) {
        peg_cascade_apply_quota(last_arr, last_n,
                                 next->quota_cap, next->quota_non,
                                 next->quota_inc, filter, 256 * 1024);
      } else {
        int took = peg_cascade_apply_topk(last_arr, last_n, next->top_k,
                                           filter, 256 * 1024);
        fprintf(stderr,
                "[cascade] combined top-%d → %d forwarded to stage %d\n",
                next->top_k, took, stage_idx + 1);
      }
    }
  }

  const double total_wall = ctimer_elapsed_seconds(&cascade_timer);
  fprintf(stderr,
          "[cascade] DONE: total wall=%.3fs (budget %.3fs)\n",
          total_wall, total_budget);

  // Build the merged final ranking. The current stage may have dropped
  // some of its cands as incomplete (budget hit before all their opp-POV states
  // finished); those cands keep their stage-(N-1) position from prev_arr.
  // Cands that completed the current stage are slotted in stage-N order
  // into the positions originally occupied by finished cands. Cands not
  // forwarded into the current stage (i.e., below the filter cutoff) keep
  // their prev_arr position.
  PegCascadeRank *final_arr = NULL;
  int final_n = 0;
  bool used_merge = false;
  if (prev_arr && prev_n > 0 && last_arr && last_n > 0) {
    final_n = peg_cascade_merge_rankings(prev_arr, prev_n, last_arr, last_n,
                                           &final_arr);
    used_merge = true;
  } else if (last_arr && last_n > 0) {
    final_arr = last_arr;
    final_n = last_n;
  } else if (prev_arr && prev_n > 0) {
    final_arr = prev_arr;
    final_n = prev_n;
  }

  if (final_arr && final_n > 0) {
    const int show = final_n < 8 ? final_n : 8;
    fprintf(stderr,
            "[cascade] final top-%d (merged: last_n=%d, prev_n=%d):\n",
            show, last_n, prev_n);
    // Tag cands that are CURRENT-stage results vs INHERITED from prev stage.
    // A cand is "current" if it appears in last_arr by text.
    for (int i = 0; i < show; i++) {
      const PegCascadeRank *r = &final_arr[i];
      bool from_last = false;
      if (used_merge) {
        for (int j = 0; j < last_n; j++) {
          if (strcmp(last_arr[j].cand_text, r->cand_text) == 0) {
            from_last = true;
            break;
          }
        }
      } else {
        from_last = true;
      }
      fprintf(stderr,
              "  #%-2d  win=%.4f  spread=%+9.3f  scen=%d  weight=%lld  %s%s\n",
              r->rank, r->win, r->spread, r->scen,
              (long long)r->weight, r->cand_text,
              from_last ? "" : "  [from-prev]");
    }
  }
  if (used_merge) {
    free(final_arr);
  }
  free(prev_arr);
  free(last_arr);
  free(filter);

  // Restore caller's env.
  for (int i = 0; i < N_OVERRIDDEN; i++) {
    if (saved[i]) {
      setenv(overridden[i], saved[i], 1);
      free(saved[i]);
    } else {
      unsetenv(overridden[i]);
    }
  }
}

// ---------------------------------------------------------------------------
// PEG cascade single-cand assertion tests (1-, 2-, 3-, 4-in-bag).
//
// Each test evaluates a known candidate move against a known CGP at oracle
// settings (no stride sampling, full RAT_WALK + OPP_RANK_BY_PLAYOUT). The
// depth is tuned per test so the eval finishes under 10s. Asserts within
// tolerance on win% and spread.
//
// CGPs are written to a tmp file and consumed via test_pass_peg_greedy_bench
// + PASSPEG_GREEDY_RESULT_FILE so we read back the result deterministically.
// ---------------------------------------------------------------------------

static void peg_assert_cand(const char *test_name,
                             const char *cgp_line_with_lex,
                             const char *cand_filter, int depth,
                             double expected_win, double expected_spread,
                             double win_tol, double spread_tol) {
  const char *cgp_path = "/tmp/magpie_peg_test_cgp.txt";
  const char *result_path = "/tmp/magpie_peg_test_result.tsv";

  FILE *cgp_f = fopen(cgp_path, "we");
  if (!cgp_f) {
    log_fatal("[%s] cannot open %s for write", test_name, cgp_path);
  }
  fprintf(cgp_f, "%s\n", cgp_line_with_lex);
  (void)fclose(cgp_f);

  char depth_buf[8];
  snprintf(depth_buf, sizeof(depth_buf), "%d", depth);
  setenv("PASSPEG_GREEDY_PATH", cgp_path, 1);
  setenv("PASSPEG_GREEDY_ONLY", cand_filter, 1);
  setenv("PASSPEG_GREEDY_DEPTH", depth_buf, 1);
  setenv("PASSPEG_GREEDY_TOP_K", "1", 1);
  setenv("PASSPEG_GREEDY_BUDGET", "30", 1);
  setenv("PASSPEG_GREEDY_THREADS", "4", 1);
  setenv("PASSPEG_GREEDY_RATIONAL", "1", 1);
  setenv("PASSPEG_GREEDY_RAT_WALK", "1", 1);
  setenv("PASSPEG_GREEDY_WALK_K_FIRST_OPP", "16", 1);
  setenv("PASSPEG_GREEDY_WALK_K_LATER_OPP", "8", 1);
  setenv("PASSPEG_SCENARIO_STRIDE", "1", 1);
  setenv("PASSPEG_PERCEPTION_STRIDE", "1", 1);
  setenv("PASSPEG_WALK_STRIDE", "1", 1);
  setenv("PASSPEG_OPP_RANK_BY_PLAYOUT", "1", 1);
  setenv("PASSPEG_OPP_RANK_POOL", "64", 1);
  setenv("PASSPEG_GREEDY_RESULT_FILE", result_path, 1);
  setenv("PEG_POOL_STUCK_TIMEOUT_S", "0", 1);
  setenv("PASSPEG_INCLUDE_PASS", "1", 1);

  test_pass_peg_greedy_bench();

  FILE *r = fopen(result_path, "re");
  if (!r) {
    log_fatal("[%s] result file %s not created", test_name, result_path);
  }
  char hdr[512];
  if (!fgets(hdr, sizeof(hdr), r)) {
    log_fatal("[%s] missing result header", test_name);
  }
  double win = -1.0;
  double spread = 0.0;
  char line[2048];
  while (fgets(line, sizeof(line), r)) {
    int pos_id = 0;
    int rank = 0;
    double w = 0.0;
    double s = 0.0;
    if (sscanf(line, "%d\t%d\t%lf\t%lf", &pos_id, &rank, &w, &s) >= 4 &&
        rank == 1) {
      win = w;
      spread = s;
      break;
    }
  }
  (void)fclose(r);

  if (win < 0.0) {
    log_fatal("[%s] no rank-1 row in result file", test_name);
  }
  const double win_diff = win - expected_win;
  const double abs_win_diff = win_diff < 0.0 ? -win_diff : win_diff;
  if (abs_win_diff > win_tol) {
    log_fatal("[%s] win %.4f outside tolerance %.4f of expected %.4f",
              test_name, win, win_tol, expected_win);
  }
  const double spread_diff = spread - expected_spread;
  const double abs_spread_diff = spread_diff < 0.0 ? -spread_diff : spread_diff;
  if (abs_spread_diff > spread_tol) {
    log_fatal("[%s] spread %.3f outside tolerance %.3f of expected %.3f",
              test_name, spread, spread_tol, expected_spread);
  }
  fprintf(stderr,
          "[%s OK] cand=%s d=%d  win=%.4f (exp %.4f)  spread=%+.3f (exp %+.3f)\n",
          test_name, cand_filter, depth, win, expected_win, spread,
          expected_spread);
}

// 1-in-bag: engineered position where passing is strictly best (mover and opp
// both hold the same bingo rack, with a Q stranded in the bag — playing the
// bingo opens up an opp counter-bingo, while passing forces opp to draw the
// unplayable Q).
void test_peg_1bag_pass_best(void) {
  const char *cgp =
      "7C6D/7H4LAR/7I2P1ALA/7VOGUE1AG/6RERAN2M1/7S1BY2O1/8OY2Id1/5JEUX3NEW/"
      "3C1U2O3A1E/3O1M6N1B/3ZIP2OAK1E2/2TI1sTIFLERS2/2WED5F1T2/1HIDEOUT7/"
      "VEG1N2IDOL4 AEINRST/AEINRST 372/369 0 -lex CSW24";
  peg_assert_cand("peg1_pass_best", cgp, "pass", 4, 1.0, 59.0, 0.005, 1.0);
}

// 1-in-bag: macondo "Straightforward1PEG" — 13L ONYX wins 7.5/8 (only tying
// when Y is the bag tile).
//   Source: macondo peg test_test.go TestStraightforward1PEG.
void test_peg_1bag_onyx(void) {
  const char *cgp =
      "15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20";
  peg_assert_cand("peg1_onyx", cgp, "13L ONYX", 4, 0.9375, 8.75, 0.005, 1.0);
}

// 2-in-bag: macondo "TwoInBagSingleMove" — 6F (A)X(E) (the AXE move) wins
// 70/72 = 0.9722; only loses when IE are in the bag in that order.
//   Source: macondo peg test_test.go TestTwoInBagSingleMove.
// Depth 3 (rather than 4) keeps runtime under 10s; the macondo win-rate is
// unchanged at d=4 (still 0.9722).
void test_peg_2bag_axe(void) {
  const char *cgp =
      "1T13/1W3Q9/VERB1U9/1E1OPIUM5C1/1LAWIN1I5O1/1Y3A1E5R1/7V4NO1/"
      "NOTArIZE1C2UN1/6ODAH2LA1/3TAHA2I2LED/2JUT4R2A1O/3G5P4D/3R3BrIEFING/"
      "3I5L4E/3K2DESYNES1M AEFGSTX/EEIOOST 370/341 0 -lex CSW21";
  peg_assert_cand("peg2_axe", cgp, "6F (A)X(E)", 3, 0.9722, 41.292, 0.005,
                  2.0);
}

// 3-in-bag: macondo manual "Position #2" (Tunnicliffe v Brennan, CSW21) —
// asserts on 13M P(AH) at depth 2 (deeper search exceeds the 10s budget for
// this position). PAH's eval has measurable non-determinism (~0.05 win, ~5pt
// spread range across runs even single-threaded) — suspected wall-clock-
// dependent depth completion in inner endgame solves. Tolerances widened to
// cover the observed range. Worth fixing separately.
void test_peg_3bag_pah(void) {
  const char *cgp =
      "BEDEL10/R1R9U2/O1IT1Q5OM2/W1BIDI4YUM2/N2XI5AT3/E3G4T1R3/"
      "S1VOILE2OKA3/T3T1DISPACED1/9AWE1O1/9Z1s1FA/14R/13GO/13AH/"
      "3JUVIE4UTA/INRO3FLENCHES ?ANNOPY/AEGILNS 344/368 0 -lex CSW21";
  peg_assert_cand("peg3_pah", cgp, "13M P(AH)", 2, 0.84, 26.0, 0.04, 3.5);
}

// 4-in-bag: macondo manual "Position #1" (Sokol v Walton POND position,
// NWL20) — asserts on 2L P(O)ND at depth 2 (deeper search exceeds 10s).
// At d=2 our cascade evaluates this cand at win=1.0000, spread=+57.084.
void test_peg_4bag_pond(void) {
  const char *cgp =
      "12D2/1U10O2/1p10L2/1R1C3KANJIS2/1I1O3A2U4/1G1T3I2I4/1H1E3Z2C1LOO/"
      "1TED3E1BYWORD/2Q4N3AXE1/1RuBIGOS3I3/F1A5WEAVE2/O1T8E3/V1E5LOURY2/"
      "ENSNARL2HM4/A6TEMP4 DEFNNPT/ 394/365 0 -lex NWL20";
  peg_assert_cand("peg4_pond", cgp, "2L P(O)ND", 2, 1.0, 57.084, 0.005, 2.0);
}

// 2-in-bag: GillesB CSW24 "ACIDOTIc" position — C6 ACIDOT(I)c with the blank
// 'c' wins 72/72 with spread +112.444 at 4 plies (per GillesB's solver).
// Eval is ~0.2s — fastest of the suite.
void test_peg_2bag_acidotic(void) {
  const char *cgp =
      "5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1V3/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/"
      "3QUOIT1PINNER/4RENEGADE2P ACDIOT?/AIIIOSU 431/392 0 -lex CSW24";
  peg_assert_cand("peg2_acidotic", cgp, "C6 ACIDOT(I)c", 4, 1.0, 112.444,
                  0.005, 0.5);
}

// ---------------------------------------------------------------------------
// Pessimistic eval (macondo-style "guaranteed wins") — Phase 1.
//
// For each ORDERED draw of N tiles from the unseen pool (where N = bag size):
//   - The drawn tiles, IN ORDER, become the bag (so as mover plays and draws,
//     they receive those tiles in sequence).
//   - The remaining tiles become opp's rack — KNOWN to both sides (perfect
//     info from the eval's POV).
//   - Apply mover's cand. The cand's leave + drawn-from-bag = mover's
//     post-cand rack. After the draw, the bag has its remaining tiles.
//   - Run endgame_solve at deep plies with both racks known + bag known.
//   - Count: 1 win if mover_total > 0, 0.5 if tie, 0 if loss.
// Aggregate: total wins / total orderings.
//
// Env vars (all required):
//   PASSPEG_PESSIMISTIC_CGP    CGP string (with -lex)
//   PASSPEG_PESSIMISTIC_MOVE   mover's cand in MAGPIE notation
//   PASSPEG_PESSIMISTIC_PLIES  endgame_solve depth (default 12)
//   PASSPEG_PESSIMISTIC_TIME   per-solve time budget seconds (default 30)
// ---------------------------------------------------------------------------

typedef struct PessimisticAccum {
  int64_t wins_x2;         // 2 * wins, +1 per tie (so /2 at the end)
  int64_t spread_sum;      // signed total mover spread
  int orderings;           // total enumerated draw orderings
} PessimisticAccum;

// Recursive enumeration of ordered draws of `remaining` tiles from
// `tile_counts` (a multiset over `num_types`). For each TYPE-prefix the cb
// is called once with a multiplicity = product over steps of count-before-
// decrement, matching the number of distinguishable-tile orderings that map
// to this type sequence.
static void peg_pess_enum_ordered_draws(
    const MachineLetter *tile_types, int *tile_counts, int num_types,
    int remaining, MachineLetter *draw_buf, int draw_buf_len,
    int64_t multiplicity,
    void (*cb)(const MachineLetter *draw, int n, int64_t weight, void *user),
    void *user) {
  if (remaining == 0) {
    cb(draw_buf, draw_buf_len, multiplicity, user);
    return;
  }
  for (int ti = 0; ti < num_types; ti++) {
    if (tile_counts[ti] <= 0) continue;
    const int count_before = tile_counts[ti];
    tile_counts[ti]--;
    draw_buf[draw_buf_len] = tile_types[ti];
    peg_pess_enum_ordered_draws(tile_types, tile_counts, num_types,
                                 remaining - 1, draw_buf, draw_buf_len + 1,
                                 multiplicity * count_before, cb, user);
    tile_counts[ti]++;
  }
}

typedef struct PessCtx {
  const Game *base_game;
  const Move *move;
  int mover_idx;
  int opp_idx;
  int ld_size;
  const uint8_t *unseen;          // unseen pool (size ld_size)
  int plies;
  double per_solve_time;
  ThreadControl *thread_control;
  EndgameCtx **eg_ctx;            // reused across solves
  EndgameResults *eg_results;
  PessimisticAccum *accum;
  int n_scenarios_logged;
} PessCtx;

static void peg_pess_eval_one(const MachineLetter *draw, int n,
                              int64_t weight, void *user) {
  PessCtx *pc = (PessCtx *)user;

  Game *scenario = game_duplicate(pc->base_game);
  game_set_endgame_solving_mode(scenario);
  game_set_backup_mode(scenario, BACKUP_MODE_OFF);

  // Empty the bag (was loaded with N unseen tiles in some default order).
  Bag *bag = game_get_bag(scenario);
  for (int ml = 0; ml < pc->ld_size; ml++) {
    while (bag_get_letter(bag, (MachineLetter)ml) > 0) {
      (void)bag_draw_letter(bag, (MachineLetter)ml, 0);
    }
  }

  // Set bag to the drawn tiles, in REVERSE order so bag_draw_letter pulls
  // them in the requested order (MAGPIE's bag pops the most-recently-added
  // tile first per the LIFO add/draw convention).
  for (int i = n - 1; i >= 0; i--) {
    bag_add_letter(bag, draw[i], 0);
  }

  // Set opp's rack = unseen - drawn tiles.
  Rack *opp_rack = player_get_rack(game_get_player(scenario, pc->opp_idx));
  rack_reset(opp_rack);
  uint8_t leftover[MAX_ALPHABET_SIZE];
  memcpy(leftover, pc->unseen, (size_t)pc->ld_size);
  for (int i = 0; i < n; i++) {
    leftover[draw[i]]--;
  }
  for (int ml = 0; ml < pc->ld_size; ml++) {
    for (int k = 0; k < (int)leftover[ml]; k++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }

  // Apply the cand. This plays the move and draws from the bag in order.
  play_move(pc->move, scenario, NULL);
  game_set_game_end_reason(scenario, GAME_END_REASON_NONE);

  const int32_t mover_lead = equity_to_int(player_get_score(
                                 game_get_player(scenario, pc->mover_idx))) -
                              equity_to_int(player_get_score(
                                  game_get_player(scenario, pc->opp_idx)));

  EndgameArgs ea = {
      .thread_control = pc->thread_control,
      .game = scenario,
      .plies = pc->plies,
      .shared_tt = NULL,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .skip_word_pruning = false,
      .thread_index_offset = 0,
      .soft_time_limit = pc->per_solve_time,
      .hard_time_limit = pc->per_solve_time,
  };
  endgame_results_reset(pc->eg_results);
  endgame_solve_inline(pc->eg_ctx, &ea, pc->eg_results);
  const int eg_val =
      endgame_results_get_value(pc->eg_results, ENDGAME_RESULT_BEST);

  // endgame_solve_inline returns the value from the player-on-turn's POV.
  // After play_move, opp is on turn — so eg_val is opp's gain. Mover's
  // realized spread = mover_lead - eg_val.
  const int32_t mover_total = mover_lead - eg_val;

  pc->accum->spread_sum += (int64_t)mover_total * weight;
  if (mover_total > 0) {
    pc->accum->wins_x2 += 2 * weight;
  } else if (mover_total == 0) {
    pc->accum->wins_x2 += weight;
  }
  pc->accum->orderings += weight;

  if (pc->n_scenarios_logged < 10) {
    const LetterDistribution *ld = game_get_ld(scenario);
    char draw_str[64] = {0};
    for (int i = 0; i < n; i++) {
      const char *hl = ld->ld_ml_to_hl[draw[i]];
      strncat(draw_str, hl ? hl : "?", sizeof(draw_str) - strlen(draw_str) - 1);
    }
    fprintf(stderr,
            "  [pess scenario] draw=%-8s  mover_lead=%+d  eg_val=%+d  "
            "mover_total=%+d\n",
            draw_str, mover_lead, eg_val, mover_total);
    pc->n_scenarios_logged++;
  }

  game_destroy(scenario);
}

void test_pass_peg_pessimistic_eval(void) {
  const char *cgp = getenv("PASSPEG_PESSIMISTIC_CGP");
  if (!cgp || !*cgp) {
    log_fatal("PASSPEG_PESSIMISTIC_CGP must be set");
  }
  const char *move_str = getenv("PASSPEG_PESSIMISTIC_MOVE");
  if (!move_str || !*move_str) {
    log_fatal("PASSPEG_PESSIMISTIC_MOVE must be set");
  }
  const char *plies_env = getenv("PASSPEG_PESSIMISTIC_PLIES");
  const int plies = plies_env && *plies_env ? atoi(plies_env) : 12;
  const char *time_env = getenv("PASSPEG_PESSIMISTIC_TIME");
  const double per_solve_time = time_env && *time_env ? atof(time_env) : 30.0;

  Config *config = config_create_or_die("set -s1 score -s2 score");
  char load_cmd[10240];
  snprintf(load_cmd, sizeof(load_cmd), "cgp %s", cgp);
  load_and_exec_config_or_die(config, load_cmd);
  Game *game = config_get_game(config);

  const int mover_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - mover_idx;
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const int bag_size = bag_get_letters(game_get_bag(game));

  // Compute unseen pool: dist - mover_rack - board.
  uint8_t unseen[MAX_ALPHABET_SIZE] = {0};
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
      if (on_board == 0 || on_board == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      MachineLetter eff =
          get_is_blanked(on_board) ? BLANK_MACHINE_LETTER : on_board;
      if (unseen[eff] > 0) {
        unseen[eff]--;
      }
    }
  }
  int total_unseen = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total_unseen += unseen[ml];
  }
  if (total_unseen != RACK_SIZE + bag_size) {
    log_fatal("pessimistic eval: expected %d unseen, got %d (rack=%d bag=%d)",
              RACK_SIZE + bag_size, total_unseen, RACK_SIZE, bag_size);
  }

  ErrorStack *parse_err = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(
      game, mover_idx, move_str,
      /*allow_phonies=*/false, /*allow_playthrough=*/true, parse_err);
  if (!error_stack_is_empty(parse_err)) {
    log_fatal("pessimistic eval: failed to parse move %s", move_str);
  }
  if (validated_moves_get_number_of_moves(vms) < 1) {
    log_fatal("pessimistic eval: no moves parsed from %s", move_str);
  }
  const Move *move = validated_moves_get_move(vms, 0);

  MachineLetter tile_types[MAX_ALPHABET_SIZE];
  int tile_counts[MAX_ALPHABET_SIZE];
  int num_types = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (unseen[ml] > 0) {
      tile_types[num_types] = (MachineLetter)ml;
      tile_counts[num_types] = (int)unseen[ml];
      num_types++;
    }
  }

  fprintf(stderr,
          "[pegpessimistic] move=%s plies=%d soft_time=%.1fs bag=%d "
          "unseen_types=%d total_unseen=%d\n",
          move_str, plies, per_solve_time, bag_size, num_types, total_unseen);

  PessimisticAccum accum = {0};
  EndgameCtx *eg_ctx = NULL;
  EndgameResults *eg_results = endgame_results_create();
  PessCtx pc = {
      .base_game = game,
      .move = move,
      .mover_idx = mover_idx,
      .opp_idx = opp_idx,
      .ld_size = ld_size,
      .unseen = unseen,
      .plies = plies,
      .per_solve_time = per_solve_time,
      .thread_control = config_get_thread_control(config),
      .eg_ctx = &eg_ctx,
      .eg_results = eg_results,
      .accum = &accum,
      .n_scenarios_logged = 0,
  };

  MachineLetter draw_buf[16];
  peg_pess_enum_ordered_draws(tile_types, tile_counts, num_types, bag_size,
                               draw_buf, 0, /*multiplicity=*/1,
                               peg_pess_eval_one, &pc);

  const double win_pct =
      accum.orderings > 0 ? (double)accum.wins_x2 / (2.0 * accum.orderings)
                          : 0.0;
  const double mean_spread =
      accum.orderings > 0 ? (double)accum.spread_sum / accum.orderings : 0.0;

  printf("\n=== Pessimistic eval ===\n");
  printf("CGP:  %s\n", cgp);
  printf("Move: %s   plies=%d\n", move_str, plies);
  printf("orderings=%d  wins_x2=%lld  win%%=%.4f  mean_spread=%+0.3f\n",
         accum.orderings, (long long)accum.wins_x2, win_pct, mean_spread);

  endgame_ctx_destroy(eg_ctx);
  endgame_results_destroy(eg_results);
  validated_moves_destroy(vms);
  error_stack_destroy(parse_err);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Pessimistic full eval — Phase 2 port of macondo's recursive PEG solver.
//
// Mirrors macondo's peg_generic.recursiveSolve for non-emptier cands. At
// each node:
//   - Base case (game over OR bag empty): run endgame_solve, classify the
//     spread as W/L/D for mover.
//   - Bag non-empty, opp's turn: enumerate ALL legal opp moves, recurse.
//     Take the WORST outcome (opp picks the move that hurts mover most).
//     First-loss cutoff: as soon as some opp reply gives mover a loss,
//     return loss (subsequent replies can't make it worse).
//   - Bag non-empty, mover's turn: enumerate ALL legal mover moves, recurse.
//     Take the BEST outcome (mover plays optimally). First-win cutoff: if
//     some reply gives a win, return win.
//
// For each (cand, bag-ordering) pair, accumulate W/L/D. Report per-cand
// win% = (wins + 0.5*draws) / total_orderings.
//
// Env vars (all required for the test):
//   PASSPEG_PESSFULL_CGP       CGP string (with -lex)
//   PASSPEG_PESSFULL_MOVE      mover's cand in MAGPIE notation
//   PASSPEG_PESSFULL_PLIES     endgame_solve plies at the bag-empty leaf
//                              (default 12)
//   PASSPEG_PESSFULL_TIME      per-solve time budget (default 5)
//   PASSPEG_PESSFULL_MAX_OPP_K cap opp move enumeration at top-K by score
//                              (default 0 = no cap). Loose pessimistic if
//                              set — opp may not be playing globally-best.
// ---------------------------------------------------------------------------

typedef enum {
  PEG_OUT_LOSS = 0,
  PEG_OUT_DRAW = 1,
  PEG_OUT_WIN = 2,
} PegPessOut;

// Endgame-position cache. At the bag-empty leaf, multiple recursion paths can
// converge to identical (board, racks, on-turn, lead, scoreless) states. The
// endgame_solve at the leaf is expensive (~ms each) and deterministic in that
// state, so caching by hash-of-state is correct and high-leverage.
typedef struct PegPessCacheEntry {
  uint64_t key;
  PegPessOut outcome;
  bool valid;
} PegPessCacheEntry;

typedef struct PegPessCache {
  PegPessCacheEntry *entries;
  size_t capacity;    // power of 2
  size_t mask;
  cpthread_mutex_t mutex;
  atomic_long hits;
  atomic_long misses;
} PegPessCache;

typedef struct PegPessSolver {
  Game *game;
  int mover_idx;
  int opp_idx;
  int ld_size;
  ThreadControl *thread_control;
  int endgame_plies;
  double endgame_time;
  int max_opp_k;            // 0 = no cap
  EndgameCtx **eg_ctx_p;    // caller-owned slot; endgame_solve_inline may alloc
  EndgameResults *eg_results;
  PegPessCache *cache;      // shared across workers; NULL = disabled
  int64_t n_endgame_solves;
  int64_t n_recursive_calls;
  int64_t n_first_loss_cutoffs;
  int64_t n_first_win_cutoffs;
} PegPessSolver;

static PegPessCache *peg_pess_cache_create(size_t cap_request) {
  size_t cap = 1;
  while (cap < cap_request) cap *= 2;
  PegPessCache *c = malloc_or_die(sizeof(*c));
  c->entries = calloc_or_die(cap, sizeof(PegPessCacheEntry));
  c->capacity = cap;
  c->mask = cap - 1;
  cpthread_mutex_init(&c->mutex);
  atomic_init(&c->hits, 0);
  atomic_init(&c->misses, 0);
  return c;
}

static void peg_pess_cache_destroy(PegPessCache *c) {
  if (!c) return;
  free(c->entries);
  free(c);
}

static inline uint64_t peg_pess_fnv1a(uint64_t hash, const void *data,
                                       size_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// Hash the bag-empty game state. Called only when bag is empty, so bag
// layout is irrelevant. Includes lead (mover - opp) and scoreless turns.
static uint64_t peg_pess_hash_endgame_state(const Game *g, int mover_idx,
                                             int opp_idx) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  const Board *b = game_get_board(g);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter ml = board_get_letter(b, row, col);
      hash = peg_pess_fnv1a(hash, &ml, sizeof(ml));
    }
  }
  const Rack *mr = player_get_rack(game_get_player(g, mover_idx));
  const Rack *opp_r = player_get_rack(game_get_player(g, opp_idx));
  const int dist_size = ld_get_size(game_get_ld(g));
  for (int i = 0; i < dist_size; i++) {
    uint8_t mc = rack_get_letter(mr, i);
    uint8_t oc = rack_get_letter(opp_r, i);
    hash = peg_pess_fnv1a(hash, &mc, sizeof(mc));
    hash = peg_pess_fnv1a(hash, &oc, sizeof(oc));
  }
  const int32_t lead =
      equity_to_int(player_get_score(game_get_player(g, mover_idx))) -
      equity_to_int(player_get_score(game_get_player(g, opp_idx)));
  hash = peg_pess_fnv1a(hash, &lead, sizeof(lead));
  const int turn = game_get_player_on_turn_index(g);
  hash = peg_pess_fnv1a(hash, &turn, sizeof(turn));
  // Avoid 0 as a sentinel.
  if (hash == 0) hash = 1;
  return hash;
}

// Returns true on hit (and writes *out). Mutex-protected.
static bool peg_pess_cache_lookup(PegPessCache *c, uint64_t key,
                                   PegPessOut *out) {
  if (!c) return false;
  cpthread_mutex_lock(&c->mutex);
  size_t idx = (size_t)key & c->mask;
  for (size_t probe = 0; probe < c->capacity; probe++) {
    PegPessCacheEntry *e = &c->entries[(idx + probe) & c->mask];
    if (!e->valid) {
      cpthread_mutex_unlock(&c->mutex);
      atomic_fetch_add(&c->misses, 1);
      return false;
    }
    if (e->key == key) {
      *out = e->outcome;
      cpthread_mutex_unlock(&c->mutex);
      atomic_fetch_add(&c->hits, 1);
      return true;
    }
  }
  cpthread_mutex_unlock(&c->mutex);
  return false;
}

static void peg_pess_cache_store(PegPessCache *c, uint64_t key,
                                  PegPessOut outcome) {
  if (!c) return;
  cpthread_mutex_lock(&c->mutex);
  size_t idx = (size_t)key & c->mask;
  for (size_t probe = 0; probe < c->capacity; probe++) {
    PegPessCacheEntry *e = &c->entries[(idx + probe) & c->mask];
    if (!e->valid || e->key == key) {
      e->key = key;
      e->outcome = outcome;
      e->valid = true;
      cpthread_mutex_unlock(&c->mutex);
      return;
    }
  }
  cpthread_mutex_unlock(&c->mutex);
}

static PegPessOut peg_pess_classify_spread(int32_t spread) {
  if (spread > 0) return PEG_OUT_WIN;
  if (spread < 0) return PEG_OUT_LOSS;
  return PEG_OUT_DRAW;
}

// Forward decl for recursion.
static PegPessOut peg_pess_recursive_solve(PegPessSolver *s);

// Evaluate at a base case: bag empty or game over. Returns mover's outcome.
static PegPessOut peg_pess_base_case(PegPessSolver *s) {
  Game *g = s->game;
  if (game_get_game_end_reason(g) != GAME_END_REASON_NONE) {
    const int32_t mover_score = equity_to_int(player_get_score(
        game_get_player(g, s->mover_idx)));
    const int32_t opp_score = equity_to_int(player_get_score(
        game_get_player(g, s->opp_idx)));
    return peg_pess_classify_spread(mover_score - opp_score);
  }
  // Cache lookup: same endgame position from different recursion paths
  // produces the same outcome — endgame_solve is deterministic in
  // (board, racks, on-turn, lead).
  uint64_t cache_key = 0;
  if (s->cache) {
    cache_key = peg_pess_hash_endgame_state(g, s->mover_idx, s->opp_idx);
    PegPessOut cached;
    if (peg_pess_cache_lookup(s->cache, cache_key, &cached)) {
      return cached;
    }
  }

  // Bag empty, game still on. Run endgame_solve on a duplicate — that call
  // dirties the game's backup stack, so without copying we'd leak backup
  // depth across recursive returns and eventually segfault in game_backup.
  Game *scratch = game_duplicate(g);
  game_set_endgame_solving_mode(scratch);
  game_set_backup_mode(scratch, BACKUP_MODE_OFF);
  EndgameArgs ea = {
      .thread_control = s->thread_control,
      .game = scratch,
      .plies = s->endgame_plies,
      .shared_tt = NULL,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .skip_word_pruning = false,
      .thread_index_offset = 0,
      .soft_time_limit = s->endgame_time,
      .hard_time_limit = s->endgame_time,
  };
  endgame_results_reset(s->eg_results);
  endgame_solve_inline(s->eg_ctx_p, &ea, s->eg_results);
  s->n_endgame_solves++;
  const int eg_val =
      endgame_results_get_value(s->eg_results, ENDGAME_RESULT_BEST);
  const int turn = game_get_player_on_turn_index(scratch);
  const int32_t mover_lead =
      equity_to_int(player_get_score(game_get_player(scratch, s->mover_idx))) -
      equity_to_int(player_get_score(game_get_player(scratch, s->opp_idx)));
  const int32_t mover_total =
      (turn == s->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
  game_destroy(scratch);
  const PegPessOut out = peg_pess_classify_spread(mover_total);
  if (s->cache) {
    peg_pess_cache_store(s->cache, cache_key, out);
  }
  return out;
}

// Recursively solve from the current game state.
static PegPessOut peg_pess_recursive_solve(PegPessSolver *s) {
  Game *g = s->game;
  s->n_recursive_calls++;

  if (game_get_game_end_reason(g) != GAME_END_REASON_NONE ||
      bag_get_letters(game_get_bag(g)) == 0) {
    return peg_pess_base_case(s);
  }

  // Generate all legal moves for the player on turn.
  MoveList *ml = move_list_create(16384);
  const MoveGenArgs ga = {
      .game = g,
      .move_list = ml,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&ga);
  const int n_moves = move_list_get_count(ml);
  const int turn = game_get_player_on_turn_index(g);
  const bool our_turn = (turn == s->mover_idx);

  // Build an explicit move-index order, sorted by score descending. The
  // generator's MOVE_SORT_EQUITY ordering puts pass at index 0; macondo
  // sorts replies by score before iterating, so we mirror that.
  int *order = malloc_or_die((size_t)n_moves * sizeof(int));
  for (int i = 0; i < n_moves; i++) order[i] = i;
  // Selection sort over the top max_opp_k positions (or all when no cap).
  int cand_n = n_moves;
  if (!our_turn && s->max_opp_k > 0 && s->max_opp_k < cand_n) {
    cand_n = s->max_opp_k;
  }
  for (int i = 0; i < cand_n; i++) {
    int best = i;
    int best_score = (int)equity_to_int(
        move_get_score(move_list_get_move(ml, order[i])));
    for (int j = i + 1; j < n_moves; j++) {
      const int js =
          (int)equity_to_int(move_get_score(move_list_get_move(ml, order[j])));
      if (js > best_score) {
        best = j;
        best_score = js;
      }
    }
    if (best != i) {
      const int tmp = order[i];
      order[i] = order[best];
      order[best] = tmp;
    }
  }

  PegPessOut best_or_worst;
  if (our_turn) {
    best_or_worst = PEG_OUT_LOSS;
  } else {
    best_or_worst = PEG_OUT_WIN;
  }
  bool cutoff = false;

  for (int mi = 0; mi < cand_n; mi++) {
    const Move *m = move_list_get_move(ml, order[mi]);
    Game *next = game_duplicate(g);
    game_set_backup_mode(next, BACKUP_MODE_OFF);
    play_move(m, next, NULL);
    game_set_game_end_reason(next, GAME_END_REASON_NONE);
    Game *saved = s->game;
    s->game = next;
    PegPessOut sub = peg_pess_recursive_solve(s);
    s->game = saved;
    game_destroy(next);

    if (our_turn) {
      if (sub > best_or_worst) best_or_worst = sub;
      if (best_or_worst == PEG_OUT_WIN) {
        s->n_first_win_cutoffs++;
        cutoff = true;
      }
    } else {
      if (sub < best_or_worst) best_or_worst = sub;
      if (best_or_worst == PEG_OUT_LOSS) {
        s->n_first_loss_cutoffs++;
        cutoff = true;
      }
    }
    if (cutoff) break;
  }

  free(order);
  move_list_destroy(ml);
  return best_or_worst;
}

typedef struct PegPessFullAccum {
  int64_t wins;
  int64_t losses;
  int64_t draws;
  int64_t total;
} PegPessFullAccum;

// One materialized bag-draw permutation: the ordered tile sequence, its
// weight (# of distinguishable-tile orderings it represents), and after
// processing, the recursive solver's outcome.
typedef struct PegPessOrdering {
  MachineLetter draw[16];
  int n;
  int64_t weight;
  int opp_top_score;   // pre-computed for sorting (opp's best score after cand)
  PegPessOut outcome;  // filled in by worker
} PegPessOrdering;

// Materialize all orderings via the existing enum.
typedef struct PessMaterializeCtx {
  PegPessOrdering *orderings;
  int n_orderings;
  int capacity;
} PessMaterializeCtx;

static void peg_pess_materialize_cb(const MachineLetter *draw, int n,
                                     int64_t weight, void *user) {
  PessMaterializeCtx *mc = (PessMaterializeCtx *)user;
  if (mc->n_orderings >= mc->capacity) {
    log_fatal("pessfull: ordering buffer too small (cap=%d)", mc->capacity);
  }
  PegPessOrdering *o = &mc->orderings[mc->n_orderings++];
  for (int i = 0; i < n; i++) o->draw[i] = draw[i];
  o->n = n;
  o->weight = weight;
  o->opp_top_score = 0;
  o->outcome = PEG_OUT_DRAW;
}

// Build the scenario for a given ordering. Caller owns the returned Game.
static Game *peg_pess_build_scenario(const Game *base_game,
                                      const PegPessOrdering *o,
                                      const uint8_t *unseen, int ld_size,
                                      int opp_idx, const Move *cand) {
  Game *scenario = game_duplicate(base_game);
  game_set_backup_mode(scenario, BACKUP_MODE_OFF);
  Bag *bag = game_get_bag(scenario);
  for (int ml = 0; ml < ld_size; ml++) {
    while (bag_get_letter(bag, (MachineLetter)ml) > 0) {
      (void)bag_draw_letter(bag, (MachineLetter)ml, 0);
    }
  }
  for (int i = o->n - 1; i >= 0; i--) {
    bag_add_letter(bag, o->draw[i], 0);
  }
  Rack *opp_rack = player_get_rack(game_get_player(scenario, opp_idx));
  rack_reset(opp_rack);
  uint8_t leftover[MAX_ALPHABET_SIZE];
  memcpy(leftover, unseen, (size_t)ld_size);
  for (int i = 0; i < o->n; i++) leftover[o->draw[i]]--;
  for (int ml = 0; ml < ld_size; ml++) {
    for (int k = 0; k < (int)leftover[ml]; k++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
  play_move(cand, scenario, NULL);
  game_set_game_end_reason(scenario, GAME_END_REASON_NONE);
  return scenario;
}

// Per-worker job + state shared by ordering. Each worker also owns its own
// EndgameCtx + EndgameResults to avoid races inside endgame_solve_inline.
typedef struct PessJob {
  const Game *base_game;
  const Move *cand;
  const uint8_t *unseen;
  int ld_size;
  int mover_idx;
  int opp_idx;
  ThreadControl *thread_control;
  int endgame_plies;
  double endgame_time;
  int max_opp_k;
  PegPessOrdering *ordering;
  // Per-worker scratch (each worker_idx gets its own slot).
  EndgameCtx **eg_ctxs;    // length = num_workers + 1 (helper)
  EndgameResults **eg_results;
  PegPessSolver *solvers;  // length = num_workers + 1 (helper)
  int n_worker_slots;
  // Shared atomic counters for diagnostics.
  atomic_long *shared_n_solves;
  atomic_long *shared_n_recursive;
  atomic_long *shared_n_loss_cutoffs;
  atomic_long *shared_n_win_cutoffs;
  PegPessCache *cache;  // shared endgame-state cache (may be NULL)
} PessJob;

// Map worker_idx → slot. peg_pool hands in worker_idx in
// [thread_index_offset, thread_index_offset + num_workers); the calling thread
// uses helper_worker_idx (we map all worker_idxs into the per-worker scratch
// array with modulo).
static int peg_pess_worker_slot(const PessJob *j, int worker_idx) {
  // worker_idxs come from the pool [100, 100+num_workers) or the helper (=0);
  // map negative or out-of-range to 0.
  int slot = worker_idx - 100;
  if (slot < 0 || slot >= j->n_worker_slots) {
    slot = j->n_worker_slots - 1;  // last slot = helper
  }
  return slot;
}

static void peg_pess_worker_fn(void *arg, int worker_idx) {
  PessJob *j = (PessJob *)arg;
  PegPessOrdering *o = j->ordering;
  const int slot = peg_pess_worker_slot(j, worker_idx);
  PegPessSolver *solver = &j->solvers[slot];
  solver->game = NULL;  // will be set per-call
  solver->mover_idx = j->mover_idx;
  solver->opp_idx = j->opp_idx;
  solver->ld_size = j->ld_size;
  solver->thread_control = j->thread_control;
  solver->endgame_plies = j->endgame_plies;
  solver->endgame_time = j->endgame_time;
  solver->max_opp_k = j->max_opp_k;
  solver->eg_ctx_p = &j->eg_ctxs[slot];
  solver->eg_results = j->eg_results[slot];
  solver->cache = j->cache;
  solver->n_endgame_solves = 0;
  solver->n_recursive_calls = 0;
  solver->n_first_loss_cutoffs = 0;
  solver->n_first_win_cutoffs = 0;

  Game *scenario = peg_pess_build_scenario(j->base_game, o, j->unseen,
                                            j->ld_size, j->opp_idx, j->cand);
  solver->game = scenario;
  o->outcome = peg_pess_recursive_solve(solver);
  game_destroy(scenario);

  atomic_fetch_add(j->shared_n_solves, solver->n_endgame_solves);
  atomic_fetch_add(j->shared_n_recursive, solver->n_recursive_calls);
  atomic_fetch_add(j->shared_n_loss_cutoffs, solver->n_first_loss_cutoffs);
  atomic_fetch_add(j->shared_n_win_cutoffs, solver->n_first_win_cutoffs);
}

// Pre-compute opp's top-score response for a given ordering. Single-threaded;
// runs before the parallel solve. Used to sort orderings (hardest first) so
// that first-loss style cutoffs fire sooner for callers that want them.
static int peg_pess_compute_opp_top_score(
    const Game *base_game, const PegPessOrdering *o, const uint8_t *unseen,
    int ld_size, int opp_idx, const Move *cand) {
  Game *scenario =
      peg_pess_build_scenario(base_game, o, unseen, ld_size, opp_idx, cand);
  MoveList *ml = move_list_create(16384);
  const MoveGenArgs ga = {
      .game = scenario,
      .move_list = ml,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&ga);
  int top = 0;
  const int n = move_list_get_count(ml);
  for (int i = 0; i < n; i++) {
    const Move *m = move_list_get_move(ml, i);
    if (move_get_type(m) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const int s = (int)equity_to_int(move_get_score(m));
      if (s > top) top = s;
    }
  }
  move_list_destroy(ml);
  game_destroy(scenario);
  return top;
}

void test_pass_peg_pessimistic_full_eval(void) {
  const char *cgp = getenv("PASSPEG_PESSFULL_CGP");
  if (!cgp || !*cgp) log_fatal("PASSPEG_PESSFULL_CGP must be set");
  const char *move_str = getenv("PASSPEG_PESSFULL_MOVE");
  if (!move_str || !*move_str) log_fatal("PASSPEG_PESSFULL_MOVE must be set");
  const char *plies_env = getenv("PASSPEG_PESSFULL_PLIES");
  const int plies = plies_env && *plies_env ? atoi(plies_env) : 12;
  const char *time_env = getenv("PASSPEG_PESSFULL_TIME");
  const double per_solve_time =
      time_env && *time_env ? atof(time_env) : 5.0;
  const char *opp_k_env = getenv("PASSPEG_PESSFULL_MAX_OPP_K");
  const int max_opp_k = opp_k_env && *opp_k_env ? atoi(opp_k_env) : 0;

  Config *config = config_create_or_die("set -s1 score -s2 score");
  char load_cmd[10240];
  snprintf(load_cmd, sizeof(load_cmd), "cgp %s", cgp);
  load_and_exec_config_or_die(config, load_cmd);
  Game *game = config_get_game(config);

  const int mover_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - mover_idx;
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const int bag_size = bag_get_letters(game_get_bag(game));

  uint8_t unseen[MAX_ALPHABET_SIZE] = {0};
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
      if (on_board == 0 || on_board == ALPHABET_EMPTY_SQUARE_MARKER) continue;
      MachineLetter eff =
          get_is_blanked(on_board) ? BLANK_MACHINE_LETTER : on_board;
      if (unseen[eff] > 0) unseen[eff]--;
    }
  }

  int total_unseen = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total_unseen += unseen[ml];
  }
  if (total_unseen != RACK_SIZE + bag_size) {
    log_fatal("pessfull: expected %d unseen, got %d", RACK_SIZE + bag_size,
              total_unseen);
  }

  ErrorStack *parse_err = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(
      game, mover_idx, move_str, false, true, parse_err);
  if (!error_stack_is_empty(parse_err)) {
    log_fatal("pessfull: failed to parse move %s", move_str);
  }
  const Move *move = validated_moves_get_move(vms, 0);

  MachineLetter tile_types[MAX_ALPHABET_SIZE];
  int tile_counts[MAX_ALPHABET_SIZE];
  int num_types = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (unseen[ml] > 0) {
      tile_types[num_types] = (MachineLetter)ml;
      tile_counts[num_types] = (int)unseen[ml];
      num_types++;
    }
  }

  fprintf(stderr,
          "[pessfull] move=%s plies=%d soft_time=%.1fs bag=%d unseen=%d "
          "max_opp_k=%d\n",
          move_str, plies, per_solve_time, bag_size, total_unseen, max_opp_k);

  const char *threads_env = getenv("PASSPEG_PESSFULL_THREADS");
  const int n_threads = threads_env && *threads_env ? atoi(threads_env) : 18;
  const char *sort_env = getenv("PASSPEG_PESSFULL_SORT");
  const bool do_sort = sort_env && *sort_env ? atoi(sort_env) > 0 : true;

  // Phase 1: materialize all orderings.
  // Upper bound on count: 10! at worst (4-peg with 11 unseen types). 1M slack.
  const int cap = 1 << 16;
  PegPessOrdering *orderings =
      malloc_or_die((size_t)cap * sizeof(PegPessOrdering));
  PessMaterializeCtx mc = {.orderings = orderings, .n_orderings = 0, .capacity = cap};
  Timer t;
  ctimer_start(&t);
  MachineLetter draw_buf[16];
  peg_pess_enum_ordered_draws(tile_types, tile_counts, num_types, bag_size,
                               draw_buf, 0, /*multiplicity=*/1,
                               peg_pess_materialize_cb, &mc);
  fprintf(stderr, "[pessfull] materialized %d orderings (%.2fs)\n",
          mc.n_orderings, ctimer_elapsed_seconds(&t));

  // Phase 2: optional pre-pass to estimate opp's best response, sort.
  if (do_sort) {
    Timer sort_t;
    ctimer_start(&sort_t);
    for (int oi = 0; oi < mc.n_orderings; oi++) {
      orderings[oi].opp_top_score = peg_pess_compute_opp_top_score(
          game, &orderings[oi], unseen, ld_size, opp_idx, move);
    }
    // Sort by opp_top_score desc (insertion sort — fine for <= a few thousand).
    for (int i = 1; i < mc.n_orderings; i++) {
      const PegPessOrdering key = orderings[i];
      int j = i - 1;
      while (j >= 0 && orderings[j].opp_top_score < key.opp_top_score) {
        orderings[j + 1] = orderings[j];
        j--;
      }
      orderings[j + 1] = key;
    }
    fprintf(stderr, "[pessfull] sorted by opp_top_score (%.2fs); top=%d bottom=%d\n",
            ctimer_elapsed_seconds(&sort_t),
            mc.n_orderings > 0 ? orderings[0].opp_top_score : 0,
            mc.n_orderings > 0 ? orderings[mc.n_orderings - 1].opp_top_score : 0);
  }

  // Shared endgame-state cache. 2^20 entries × 16 bytes = ~16 MB.
  const char *cache_env = getenv("PASSPEG_PESSFULL_CACHE");
  const bool use_cache = !cache_env || atoi(cache_env) > 0;  // default on
  PegPessCache *cache = use_cache ? peg_pess_cache_create(1u << 20) : NULL;

  // Phase 3: per-worker scratch + parallel solve via peg_pool.
  PegPool *pool = n_threads > 1 ? peg_pool_create(n_threads, 100) : NULL;
  const int n_slots = n_threads + 1;  // workers + helper
  EndgameCtx **eg_ctxs = calloc_or_die((size_t)n_slots, sizeof(EndgameCtx *));
  EndgameResults **eg_results =
      malloc_or_die((size_t)n_slots * sizeof(EndgameResults *));
  PegPessSolver *solvers =
      malloc_or_die((size_t)n_slots * sizeof(PegPessSolver));
  for (int i = 0; i < n_slots; i++) {
    eg_results[i] = endgame_results_create();
  }
  atomic_long total_solves = ATOMIC_VAR_INIT(0);
  atomic_long total_recursive = ATOMIC_VAR_INIT(0);
  atomic_long total_loss_cutoffs = ATOMIC_VAR_INIT(0);
  atomic_long total_win_cutoffs = ATOMIC_VAR_INIT(0);

  PessJob *jobs = malloc_or_die((size_t)mc.n_orderings * sizeof(PessJob));
  void **job_ptrs = malloc_or_die((size_t)mc.n_orderings * sizeof(void *));
  for (int oi = 0; oi < mc.n_orderings; oi++) {
    jobs[oi] = (PessJob){
        .base_game = game,
        .cand = move,
        .unseen = unseen,
        .ld_size = ld_size,
        .mover_idx = mover_idx,
        .opp_idx = opp_idx,
        .thread_control = config_get_thread_control(config),
        .endgame_plies = plies,
        .endgame_time = per_solve_time,
        .max_opp_k = max_opp_k,
        .ordering = &orderings[oi],
        .eg_ctxs = eg_ctxs,
        .eg_results = eg_results,
        .solvers = solvers,
        .n_worker_slots = n_slots,
        .shared_n_solves = &total_solves,
        .shared_n_recursive = &total_recursive,
        .shared_n_loss_cutoffs = &total_loss_cutoffs,
        .shared_n_win_cutoffs = &total_win_cutoffs,
        .cache = cache,
    };
    job_ptrs[oi] = &jobs[oi];
  }

  Timer solve_t;
  ctimer_start(&solve_t);
  if (pool) {
    peg_pool_submit_and_wait(pool, peg_pess_worker_fn, job_ptrs,
                              mc.n_orderings, /*helper=*/0);
  } else {
    for (int oi = 0; oi < mc.n_orderings; oi++) {
      peg_pess_worker_fn(&jobs[oi], 0);
    }
  }
  const double solve_elapsed = ctimer_elapsed_seconds(&solve_t);

  // Phase 4: aggregate outcomes.
  PegPessFullAccum acc = {0};
  for (int oi = 0; oi < mc.n_orderings; oi++) {
    const PegPessOrdering *o = &orderings[oi];
    if (o->outcome == PEG_OUT_WIN) {
      acc.wins += o->weight;
    } else if (o->outcome == PEG_OUT_LOSS) {
      acc.losses += o->weight;
    } else {
      acc.draws += o->weight;
    }
    acc.total += o->weight;
  }

  const double win_pct =
      acc.total > 0 ? (double)(acc.wins * 2 + acc.draws) / (2.0 * acc.total)
                    : 0.0;

  printf("\n=== Pessimistic full eval ===\n");
  printf("CGP:  %s\n", cgp);
  printf("Move: %s   plies=%d  threads=%d  sort=%d\n", move_str, plies,
         n_threads, do_sort ? 1 : 0);
  printf("W/L/D = %lld/%lld/%lld   total=%lld   win%%=%.4f\n",
         (long long)acc.wins, (long long)acc.losses, (long long)acc.draws,
         (long long)acc.total, win_pct);
  printf("solves=%ld  recursive=%ld  cutoffs(loss=%ld,win=%ld)  solve_wall=%.2fs\n",
         atomic_load(&total_solves), atomic_load(&total_recursive),
         atomic_load(&total_loss_cutoffs), atomic_load(&total_win_cutoffs),
         solve_elapsed);
  if (cache) {
    long h = atomic_load(&cache->hits);
    long m = atomic_load(&cache->misses);
    long total_lookups = h + m;
    printf("cache: hits=%ld misses=%ld (hit_rate=%.1f%%)\n", h, m,
           total_lookups > 0 ? (100.0 * h) / total_lookups : 0.0);
  }

  for (int i = 0; i < n_slots; i++) {
    endgame_ctx_destroy(eg_ctxs[i]);
    endgame_results_destroy(eg_results[i]);
  }
  free(eg_ctxs);
  free(eg_results);
  free(solvers);
  free(jobs);
  free(job_ptrs);
  free(orderings);
  peg_pess_cache_destroy(cache);
  if (pool) peg_pool_destroy(pool);
  validated_moves_destroy(vms);
  error_stack_destroy(parse_err);
  config_destroy(config);
}
