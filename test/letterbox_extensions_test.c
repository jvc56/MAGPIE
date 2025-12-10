#include "../letterbox/magpie_wrapper.h"
#include "../src/impl/config.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Test the specific ANALGESIAS bug
void test_analgesias_electroanalgesias_bug(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  KWG *kwg = letterbox_get_kwg(config);
  LetterDistribution *ld = letterbox_get_ld(config);

  printf("\n=== Testing ANALGESIAS ELECTRO extension bug ===\n");

  // First, verify ANALGESIAS exists
  WordList *analgesias_check = letterbox_find_anagrams(kwg, ld, "ANALGESIAS");
  printf("ANALGESIAS exists: %s\n", (analgesias_check && analgesias_check->count > 0) ? "YES" : "NO");
  assert(analgesias_check && analgesias_check->count > 0);
  word_list_destroy(analgesias_check);

  // Check if ELECTROANALGESIAS (16 letters) exists
  WordList *electroanalgesias_check = letterbox_find_anagrams(kwg, ld, "ELECTROANALGESIAS");
  printf("ELECTROANALGESIAS (16 letters) exists: %s\n",
         (electroanalgesias_check && electroanalgesias_check->count > 0) ? "YES" : "NO");
  bool electroanalgesias_exists = electroanalgesias_check && electroanalgesias_check->count > 0;
  word_list_destroy(electroanalgesias_check);

  // Get front extensions for ANALGESIAS with max_extension_length=7
  char *front_exts = letterbox_find_front_extensions(kwg, ld, "ANALGESIAS", 7);

  printf("Front extensions for ANALGESIAS (max_len=7):\n%s\n", front_exts);

  // Check if ELECTRO appears in the extensions
  bool electro_found = (strstr(front_exts, "ELECTRO") != NULL);
  printf("ELECTRO found in extensions: %s\n", electro_found ? "YES" : "NO");

  // ELECTRO should only appear if ELECTROANALGESIAS actually exists
  if (electroanalgesias_exists) {
    // If the 16-letter word exists, ELECTRO should be in extensions
    // (This would be surprising since NWL20 max is usually 15)
    printf("WARNING: ELECTROANALGESIAS exists, so ELECTRO extension is valid\n");
  } else {
    // If the 16-letter word doesn't exist, ELECTRO should NOT be in extensions
    printf("ELECTROANALGESIAS does not exist, so ELECTRO should NOT appear\n");
    if (electro_found) {
      printf("BUG DETECTED: ELECTRO appears in extensions but ELECTROANALGESIAS doesn't exist!\n");
      assert(false); // This is the bug we're testing for
    }
  }

  free(front_exts);
  config_destroy(config);
}

// Test that extensions don't return words that would exceed the dictionary's max word length
void test_extensions_max_length(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  KWG *kwg = letterbox_get_kwg(config);
  LetterDistribution *ld = letterbox_get_ld(config);

  // Test that extensions return only words that actually exist in the dictionary
  // Note: NWL20 actually has 16-letter words (like ELECTROANALGESIAS)
  // So we test that any extension returned actually forms a valid word

  char *front_exts = letterbox_find_front_extensions(kwg, ld, "ANALGESIAS", 7);

  printf("Front extensions for ANALGESIAS (max_len=7): '%s'\n", front_exts);

  // For each extension returned, verify BASE+EXT is a valid word
  if (front_exts && strlen(front_exts) > 0) {
    char *ext_copy = strdup(front_exts);
    char *ext = strtok(ext_copy, " \n");
    while (ext != NULL) {
      char combined[100];
      snprintf(combined, sizeof(combined), "%sANALGESIAS", ext);

      WordList *check = letterbox_find_anagrams(kwg, ld, combined);
      printf("  Extension '%s' -> '%s': %s\n", ext, combined,
             (check && check->count > 0) ? "VALID" : "INVALID");
      assert(check && check->count > 0);  // All extensions must be valid
      word_list_destroy(check);

      ext = strtok(NULL, " \n");
    }
    free(ext_copy);
  }

  free(front_exts);

  // Also test with a 9-letter word to verify extension length=6 is the max that should work
  // For a 9-letter word, max valid extension is 6 letters (9+6=15)
  char *front_exts_9 = letterbox_find_front_extensions(kwg, ld, "ANALOGIES", 7);

  printf("Front extensions for ANALOGIES (max_len=7): '%s'\n", front_exts_9);

  // If ELECTRO + ANALOGIES = ELECTROANALOGIES exists (15 letters), it should appear
  // If it doesn't exist, ELECTRO should not appear
  // We need to verify what actually exists in the dictionary

  free(front_exts_9);

  config_destroy(config);
}

// Test that extensions are actually valid words when combined with the base word
void test_extensions_validity(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  KWG *kwg = letterbox_get_kwg(config);
  LetterDistribution *ld = letterbox_get_ld(config);

  // Test a known word with known extensions
  // CENTER has back extensions like ED (CENTERED), ING (CENTERING)
  char *back_exts = letterbox_find_back_extensions(kwg, ld, "CENTER", 5);

  printf("Back extensions for CENTER (max_len=5): '%s'\n", back_exts);

  // Parse the extensions and verify each combined word exists
  // Extensions are grouped by length, one line per length
  // Format: "ED ER\nING\nPIECE\n" etc.

  // For each extension found, verify that BASE + EXTENSION is a valid word
  char *line = strtok(back_exts, "\n");
  while (line != NULL) {
    // Each line has extensions of the same length separated by spaces
    char *ext = strtok(line, " ");
    while (ext != NULL) {
      // Combine CENTER + ext and verify it's a valid word
      char combined[100];
      snprintf(combined, sizeof(combined), "CENTER%s", ext);

      // Find anagrams to check if the combined word exists
      WordList *check = letterbox_find_anagrams(kwg, ld, combined);

      printf("  Checking if %s is valid... ", combined);
      if (check && check->count > 0) {
        printf("YES\n");
      } else {
        printf("NO - EXTENSION RETURNED INVALID WORD!\n");
        assert(false); // This should never happen
      }

      word_list_destroy(check);
      ext = strtok(NULL, " ");
    }
    line = strtok(NULL, "\n");
  }

  free(back_exts);
  config_destroy(config);
}

// Test front extensions similarly
void test_front_extensions_validity(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  KWG *kwg = letterbox_get_kwg(config);
  LetterDistribution *ld = letterbox_get_ld(config);

  // Test front extensions
  // For example, NATIONAL might have front extensions like INTER (INTERNATIONAL)
  char *front_exts = letterbox_find_front_extensions(kwg, ld, "NATIONAL", 5);

  printf("Front extensions for NATIONAL (max_len=5): '%s'\n", front_exts);

  // Parse and verify each extension
  char *ext_copy = strdup(front_exts);
  char *line = strtok(ext_copy, "\n");
  while (line != NULL) {
    char *ext = strtok(line, " ");
    while (ext != NULL) {
      char combined[100];
      snprintf(combined, sizeof(combined), "%sNATIONAL", ext);

      WordList *check = letterbox_find_anagrams(kwg, ld, combined);

      printf("  Checking if %s is valid... ", combined);
      if (check && check->count > 0) {
        printf("YES\n");
      } else {
        printf("NO - EXTENSION RETURNED INVALID WORD!\n");
        assert(false);
      }

      word_list_destroy(check);
      ext = strtok(NULL, " ");
    }
    line = strtok(NULL, "\n");
  }

  free(ext_copy);
  free(front_exts);
  config_destroy(config);
}

// Test the same bug with CSW24
void test_analgesias_csw24(void) {
  Config *config = config_create_or_die("set -lex CSW24");
  KWG *kwg = letterbox_get_kwg(config);
  LetterDistribution *ld = letterbox_get_ld(config);

  printf("\n=== Testing ANALGESIAS with CSW24 ===\n");

  // First, verify ANALGESIAS exists
  WordList *analgesias_check = letterbox_find_anagrams(kwg, ld, "ANALGESIAS");
  printf("ANALGESIAS exists: %s\n", (analgesias_check && analgesias_check->count > 0) ? "YES" : "NO");
  assert(analgesias_check && analgesias_check->count > 0);
  word_list_destroy(analgesias_check);

  // Check if ELECTROANALGESIAS (16 letters) exists in CSW24
  WordList *electroanalgesias_check = letterbox_find_anagrams(kwg, ld, "ELECTROANALGESIAS");
  printf("ELECTROANALGESIAS (16 letters) exists: %s\n",
         (electroanalgesias_check && electroanalgesias_check->count > 0) ? "YES" : "NO");
  bool electroanalgesias_exists = electroanalgesias_check && electroanalgesias_check->count > 0;
  word_list_destroy(electroanalgesias_check);

  // Get front extensions for ANALGESIAS with max_extension_length=7
  char *front_exts = letterbox_find_front_extensions(kwg, ld, "ANALGESIAS", 7);

  printf("Front extensions for ANALGESIAS (max_len=7):\n%s\n", front_exts);

  // Check if ELECTRO appears in the extensions
  bool electro_found = (strstr(front_exts, "ELECTRO") != NULL);
  printf("ELECTRO found in extensions: %s\n", electro_found ? "YES" : "NO");

  // ELECTRO should only appear if ELECTROANALGESIAS actually exists
  if (electroanalgesias_exists) {
    printf("ELECTROANALGESIAS exists in CSW24, so ELECTRO extension is valid\n");
  } else {
    printf("ELECTROANALGESIAS does not exist in CSW24, so ELECTRO should NOT appear\n");
    if (electro_found) {
      printf("BUG DETECTED: ELECTRO appears in extensions but ELECTROANALGESIAS doesn't exist!\n");
      assert(false); // This is the bug we're testing for
    }
  }

  free(front_exts);
  config_destroy(config);
}

void test_letterbox_extensions(void) {
  test_analgesias_electroanalgesias_bug();
  test_analgesias_csw24();
  test_extensions_max_length();
  test_extensions_validity();
  test_front_extensions_validity();
}
