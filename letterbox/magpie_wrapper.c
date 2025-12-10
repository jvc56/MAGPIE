// C wrapper for MAGPIE word study functions
#include "magpie_wrapper.h"
#include "../src/ent/kwg.h"
#include "../src/ent/rack.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/players_data.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "../src/str/rack_string.h"
#include "../src/str/letter_distribution_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum word length for anagram search (should match or exceed dictionary max word length)
#define MAX_WORD_LENGTH 20

Config* letterbox_create_config(const char *data_path, const char *lexicon) {
    ErrorStack *error_stack = error_stack_create();

    // Create config with data path
    Config *config = config_create_default_with_data_paths(error_stack, data_path);

    if (!error_stack_is_empty(error_stack)) {
        fprintf(stderr, "Error creating config:\n");
        error_stack_print_and_reset(error_stack);
        error_stack_destroy(error_stack);
        return NULL;
    }

    // Set lexicon
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "set -lex %s -ld english", lexicon);
    config_load_command(config, cmd, error_stack);
    config_execute_command(config, error_stack);

    if (!error_stack_is_empty(error_stack)) {
        fprintf(stderr, "Error setting lexicon:\n");
        error_stack_print_and_reset(error_stack);
        config_destroy(config);
        error_stack_destroy(error_stack);
        return NULL;
    }

    error_stack_destroy(error_stack);
    return config;
}

void letterbox_destroy_config(Config *config) {
    if (config) {
        config_destroy(config);
    }
}

KWG* letterbox_get_kwg(Config *config) {
    if (!config) {
        return NULL;
    }
    // Get the KWG from player 0's data
    PlayersData *pd = config_get_players_data(config);
    if (!pd) {
        return NULL;
    }
    return players_data_get_kwg(pd, 0);
}

LetterDistribution* letterbox_get_ld(Config *config) {
    if (!config) {
        return NULL;
    }
    return config_get_ld(config);
}

WordList* word_list_create(void) {
    WordList *list = malloc(sizeof(WordList));
    list->words = NULL;
    list->count = 0;
    return list;
}

void word_list_destroy(WordList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->words[i]);
    }
    free(list->words);
    free(list);
}

// Recursive anagram finder (based on word_prune.c pattern)
static void find_anagrams_recursive(const KWG *kwg, uint32_t node_index,
                                   Rack *rack, const LetterDistribution *ld,
                                   MachineLetter *word, int tiles_played,
                                   bool accepts,
                                   DictionaryWordList *result_list) {
    if (accepts && tiles_played > 0) {
        dictionary_word_list_add_word(result_list, word, tiles_played);
    }

    if (node_index == 0) {
        return;
    }

    // Iterate through all possible letters at this node
    for (uint32_t i = node_index;; i++) {
        const uint32_t node = kwg_node(kwg, i);
        const MachineLetter ml = kwg_node_tile(node);
        const uint32_t new_node_index = kwg_node_arc_index_prefetch(node, kwg);

        if (rack_get_letter(rack, ml) > 0) {
            bool node_accepts = kwg_node_accepts(node);
            rack_take_letter(rack, ml);
            word[tiles_played] = ml;
            find_anagrams_recursive(kwg, new_node_index, rack, ld, word,
                                   tiles_played + 1, node_accepts, result_list);
            rack_add_letter(rack, ml);
        }

        if (kwg_node_is_end(node)) {
            break;
        }
    }
}

WordList* letterbox_find_anagrams(const KWG *kwg, const LetterDistribution *ld, const char *letters) {
    if (!kwg || !ld || !letters) {
        return word_list_create();
    }

    // Create a rack from the input letters
    Rack rack;
    rack_set_dist_size_and_reset(&rack, ld_get_size(ld));

    // Use the simpler rack_set_to_string that doesn't need ErrorStack
    int num_letters = rack_set_to_string(ld, &rack, letters);
    if (num_letters < 0) {
        return word_list_create();
    }

    // Create dictionary word list to collect results
    DictionaryWordList *dict_list = dictionary_word_list_create();

    // Find all anagrams using DAWG root
    MachineLetter word[MAX_WORD_LENGTH];
    find_anagrams_recursive(kwg, kwg_get_dawg_root_node_index(kwg),
                          &rack, ld, word, 0, false, dict_list);

    // Sort and get unique words
    dictionary_word_list_sort(dict_list);
    DictionaryWordList *unique_list = dictionary_word_list_create();
    dictionary_word_list_unique(dict_list, unique_list);
    dictionary_word_list_destroy(dict_list);

    // Convert to WordList format
    WordList *result = word_list_create();
    int count = dictionary_word_list_get_count(unique_list);

    if (count > 0) {
        result->words = malloc(count * sizeof(char*));
        result->count = count;

        StringBuilder *sb = string_builder_create();
        for (int i = 0; i < count; i++) {
            const DictionaryWord *dw = dictionary_word_list_get_word(unique_list, i);
            string_builder_clear(sb);
            // Convert each machine letter to user visible
            for (int j = 0; j < dictionary_word_get_length(dw); j++) {
                MachineLetter ml = dictionary_word_get_word(dw)[j];
                string_builder_add_user_visible_letter(sb, ld, ml);
            }
            result->words[i] = string_builder_dump(sb, NULL);
        }
        string_builder_destroy(sb);
    }

    dictionary_word_list_destroy(unique_list);
    return result;
}

char* letterbox_find_front_hooks(const KWG *kwg, const LetterDistribution *ld, const char *word) {
    if (!kwg || !ld || !word || !*word) {
        return string_duplicate("");
    }

    // Convert word to machine letters
    MachineLetter ml_word[BOARD_DIM];
    int word_len = ld_str_to_mls(ld, word, false, ml_word, BOARD_DIM);
    if (word_len <= 0) {
        return string_duplicate("");
    }

    // Use GADDAG root to find front hooks
    // Traverse the word backward from GADDAG root
    uint32_t node_index = kwg_get_root_node_index(kwg);

    for (int i = word_len - 1; i >= 0; i--) {
        node_index = kwg_get_next_node_index(kwg, node_index, ml_word[i]);
        if (node_index == 0) {
            return string_duplicate("");
        }
    }

    // Front hooks are letters that accept at THIS node (before separator)
    // This matches game.c:338-339
    StringBuilder *sb = string_builder_create();
    for (uint32_t i = node_index;; i++) {
        const uint32_t node = kwg_node(kwg, i);
        const MachineLetter ml = kwg_node_tile(node);

        // If this node accepts, it means adding this letter to the front
        // of the word forms a valid complete word
        if (ml != SEPARATION_MACHINE_LETTER && kwg_node_accepts(node)) {
            string_builder_add_user_visible_letter(sb, ld, ml);
        }

        if (kwg_node_is_end(node)) {
            break;
        }
    }

    char *result = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return result;
}

char* letterbox_find_back_hooks(const KWG *kwg, const LetterDistribution *ld, const char *word) {
    if (!kwg || !ld || !word || !*word) {
        return string_duplicate("");
    }

    // Convert word to machine letters
    MachineLetter ml_word[BOARD_DIM];
    int word_len = ld_str_to_mls(ld, word, false, ml_word, BOARD_DIM);
    if (word_len <= 0) {
        return string_duplicate("");
    }

    // Use DAWG root to find back hooks
    // Traverse the word forward from DAWG root
    uint32_t node_index = kwg_get_dawg_root_node_index(kwg);

    for (int i = 0; i < word_len; i++) {
        node_index = kwg_get_next_node_index(kwg, node_index, ml_word[i]);
        if (node_index == 0) {
            return string_duplicate("");
        }
    }

    // Collect only letters that ACCEPT (form complete words)
    StringBuilder *sb = string_builder_create();
    for (uint32_t i = node_index;; i++) {
        const uint32_t node = kwg_node(kwg, i);
        const MachineLetter ml = kwg_node_tile(node);

        if (ml != SEPARATION_MACHINE_LETTER && kwg_node_accepts(node)) {
            string_builder_add_user_visible_letter(sb, ld, ml);
        }

        if (kwg_node_is_end(node)) {
            break;
        }
    }

    char *result = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return result;
}

// Helper to recursively find all extensions from a given node
// base_word_length: length of the word being extended (to enforce 15-letter max)
static void find_extensions_recursive(const KWG *kwg, const LetterDistribution *ld,
                                     uint32_t node_index, MachineLetter *extension,
                                     int depth, int max_depth, int base_word_length,
                                     DictionaryWordList *result_list) {
    if (depth >= max_depth || node_index == 0) {
        return;
    }

    for (uint32_t i = node_index;; i++) {
        const uint32_t node = kwg_node(kwg, i);
        const MachineLetter ml = kwg_node_tile(node);
        const uint32_t next_node_index = kwg_node_arc_index_prefetch(node, kwg);

        if (ml != SEPARATION_MACHINE_LETTER) {
            extension[depth] = ml;

            // If this node accepts, we have a valid extension
            // Only add if total word length (base + extension) <= 15
            if (kwg_node_accepts(node) && (base_word_length + depth + 1) <= 15) {
                dictionary_word_list_add_word(result_list, extension, depth + 1);
            }

            // Continue searching deeper
            find_extensions_recursive(kwg, ld, next_node_index, extension,
                                    depth + 1, max_depth, base_word_length, result_list);
        }

        if (kwg_node_is_end(node)) {
            break;
        }
    }
}

char* letterbox_find_back_extensions(const KWG *kwg, const LetterDistribution *ld,
                                     const char *word, int max_extension_length) {
    if (!kwg || !ld || !word || !*word || max_extension_length <= 0) {
        return string_duplicate("");
    }

    // Convert word to machine letters
    MachineLetter ml_word[BOARD_DIM];
    int word_len = ld_str_to_mls(ld, word, false, ml_word, BOARD_DIM);
    if (word_len <= 0) {
        return string_duplicate("");
    }

    // Traverse to the end of the word
    uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
    for (int i = 0; i < word_len; i++) {
        node_index = kwg_get_next_node_index(kwg, node_index, ml_word[i]);
        if (node_index == 0) {
            return string_duplicate("");
        }
    }

    // Find all extensions up to max_extension_length
    MachineLetter extension[BOARD_DIM];
    DictionaryWordList *ext_list = dictionary_word_list_create();
    find_extensions_recursive(kwg, ld, node_index, extension, 0,
                             max_extension_length, word_len, ext_list);

    // Sort alphabetically
    dictionary_word_list_sort(ext_list);

    // Group extensions by length and format, limiting to 40 chars per line
    StringBuilder *sb = string_builder_create();
    int count = dictionary_word_list_get_count(ext_list);

    const int MAX_CHARS_PER_LINE = 10000;  // Get all extensions, truncation done in Qt

    // Group by length (shorter extensions are more playable)
    // Skip length 1 since those are shown as hooks
    for (int len = 2; len <= max_extension_length; len++) {
        bool found_any_this_length = false;
        bool line_truncated = false;
        StringBuilder *line_sb = string_builder_create();
        int letter_count = 0;  // Only count letters, not spaces

        for (int i = 0; i < count; i++) {
            const DictionaryWord *dw = dictionary_word_list_get_word(ext_list, i);
            if (dictionary_word_get_length(dw) == len) {
                // Check if adding this extension would exceed limit (only counting letters)
                if (letter_count + len > MAX_CHARS_PER_LINE) {
                    line_truncated = true;
                    break;
                }

                if (found_any_this_length) {
                    string_builder_add_string(line_sb, " ");
                }
                found_any_this_length = true;

                // Add the extension letters
                for (int j = 0; j < len; j++) {
                    MachineLetter ml = dictionary_word_get_word(dw)[j];
                    string_builder_add_user_visible_letter(line_sb, ld, ml);
                }

                letter_count += len;
            }
        }

        if (found_any_this_length) {
            if (string_builder_length(sb) > 0) {
                string_builder_add_string(sb, "\n");
            }
            char *line = string_builder_dump(line_sb, NULL);
            string_builder_add_string(sb, line);

            // Add ellipsis to this line if it was truncated
            if (line_truncated) {
                string_builder_add_string(sb, "…");
            }

            free(line);
        }
        string_builder_destroy(line_sb);
    }

    char *result = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    dictionary_word_list_destroy(ext_list);
    return result;
}

char* letterbox_find_front_extensions(const KWG *kwg, const LetterDistribution *ld,
                                      const char *word, int max_extension_length) {
    if (!kwg || !ld || !word || !*word || max_extension_length <= 0) {
        return string_duplicate("");
    }

    // Convert word to machine letters
    MachineLetter ml_word[BOARD_DIM];
    int word_len = ld_str_to_mls(ld, word, false, ml_word, BOARD_DIM);
    if (word_len <= 0) {
        return string_duplicate("");
    }

    // Traverse backward from GADDAG root
    uint32_t node_index = kwg_get_root_node_index(kwg);
    for (int i = word_len - 1; i >= 0; i--) {
        node_index = kwg_get_next_node_index(kwg, node_index, ml_word[i]);
        if (node_index == 0) {
            return string_duplicate("");
        }
    }

    // Find all extensions up to max_extension_length
    MachineLetter extension[BOARD_DIM];
    DictionaryWordList *ext_list = dictionary_word_list_create();
    find_extensions_recursive(kwg, ld, node_index, extension, 0,
                             max_extension_length, word_len, ext_list);

    // Reverse all extensions (they come out backwards from GADDAG)
    int ext_count = dictionary_word_list_get_count(ext_list);
    for (int i = 0; i < ext_count; i++) {
        DictionaryWord *dw = dictionary_word_list_get_word(ext_list, i);
        int len = dictionary_word_get_length(dw);
        MachineLetter *letters = (MachineLetter *)dictionary_word_get_word(dw);
        // Reverse the letters in place
        for (int j = 0; j < len / 2; j++) {
            MachineLetter temp = letters[j];
            letters[j] = letters[len - 1 - j];
            letters[len - 1 - j] = temp;
        }
    }

    // Sort alphabetically
    dictionary_word_list_sort(ext_list);

    // Group extensions by length and format, limiting to 40 chars per line
    StringBuilder *sb = string_builder_create();
    int count = dictionary_word_list_get_count(ext_list);

    const int MAX_CHARS_PER_LINE = 10000;  // Get all extensions, truncation done in Qt

    // Group by length (shorter extensions are more playable)
    // Skip length 1 since those are shown as hooks
    for (int len = 2; len <= max_extension_length; len++) {
        bool found_any_this_length = false;
        bool line_truncated = false;
        StringBuilder *line_sb = string_builder_create();
        int letter_count = 0;  // Only count letters, not spaces

        for (int i = 0; i < count; i++) {
            const DictionaryWord *dw = dictionary_word_list_get_word(ext_list, i);
            if (dictionary_word_get_length(dw) == len) {
                // Check if adding this extension would exceed limit (only counting letters)
                if (letter_count + len > MAX_CHARS_PER_LINE) {
                    line_truncated = true;
                    break;
                }

                if (found_any_this_length) {
                    string_builder_add_string(line_sb, " ");
                }
                found_any_this_length = true;

                // Add the extension letters
                for (int j = 0; j < len; j++) {
                    MachineLetter ml = dictionary_word_get_word(dw)[j];
                    string_builder_add_user_visible_letter(line_sb, ld, ml);
                }

                letter_count += len;
            }
        }

        if (found_any_this_length) {
            if (string_builder_length(sb) > 0) {
                string_builder_add_string(sb, "\n");
            }
            char *line = string_builder_dump(line_sb, NULL);
            string_builder_add_string(sb, line);

            // Add ellipsis to this line if it was truncated
            if (line_truncated) {
                string_builder_add_string(sb, "…");
            }

            free(line);
        }
        string_builder_destroy(line_sb);
    }

    char *result = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    dictionary_word_list_destroy(ext_list);
    return result;
}

// Pattern constraint structure
typedef struct {
    int required_letters[27];      // Count of each required letter (A=1, B=2, ...)
    uint64_t char_classes[10];     // Bitsets for character classes [ABC] etc
    int num_char_classes;          // Number of character classes
    int num_wildcards;             // Number of wildcard positions
    int pattern_length;            // Total pattern length
} PatternConstraints;

// Parse pattern string into constraints
static bool parse_pattern(const char *pattern, const LetterDistribution *ld,
                         PatternConstraints *constraints) {
    memset(constraints, 0, sizeof(PatternConstraints));

    int i = 0;
    while (pattern[i]) {
        if (pattern[i] == '[') {
            // Character class
            if (constraints->num_char_classes >= 10) {
                return false;  // Too many character classes
            }

            i++;  // Skip '['
            uint64_t char_class = 0;
            while (pattern[i] && pattern[i] != ']') {
                char c = pattern[i];
                if (c >= 'A' && c <= 'Z') {
                    char letter_str[2] = {c, '\0'};
                    MachineLetter ml = ld_hl_to_ml(ld, letter_str);
                    if (ml > 0) {
                        char_class |= (1ULL << ml);
                    }
                } else if (c >= 'a' && c <= 'z') {
                    char letter_str[2] = {c - 'a' + 'A', '\0'};
                    MachineLetter ml = ld_hl_to_ml(ld, letter_str);
                    if (ml > 0) {
                        char_class |= (1ULL << ml);
                    }
                }
                i++;
            }
            if (pattern[i] == ']') {
                constraints->char_classes[constraints->num_char_classes++] = char_class;
                constraints->pattern_length++;
                i++;
            }
        } else if (pattern[i] == '.' || pattern[i] == '?') {
            // Wildcard
            constraints->num_wildcards++;
            constraints->pattern_length++;
            i++;
        } else if ((pattern[i] >= 'A' && pattern[i] <= 'Z') ||
                   (pattern[i] >= 'a' && pattern[i] <= 'z')) {
            // Required letter
            char c = pattern[i];
            if (c >= 'a' && c <= 'z') {
                c = c - 'a' + 'A';
            }
            char letter_str[2] = {c, '\0'};
            MachineLetter ml = ld_hl_to_ml(ld, letter_str);
            if (ml > 0 && ml < 27) {
                constraints->required_letters[ml]++;
                constraints->pattern_length++;
            }
            i++;
        } else {
            // Skip unknown characters
            i++;
        }
    }

    return constraints->pattern_length > 0;
}

// Check if a rack satisfies the pattern constraints
static bool satisfies_constraints(const Rack *rack, const PatternConstraints *constraints) {
    // Check required letters
    for (int i = 1; i < 27; i++) {
        if (rack_get_letter(rack, i) < constraints->required_letters[i]) {
            return false;
        }
    }

    // Check character classes
    for (int i = 0; i < constraints->num_char_classes; i++) {
        bool found = false;
        for (int j = 1; j < 27; j++) {
            if ((constraints->char_classes[i] & (1ULL << j)) && rack_get_letter(rack, j) > 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

// Recursive pattern-based anagram finder
static void find_pattern_anagrams_recursive(const KWG *kwg, uint32_t node_index,
                                           Rack *rack, const LetterDistribution *ld,
                                           const PatternConstraints *constraints,
                                           MachineLetter *word, int tiles_played,
                                           bool accepts,
                                           DictionaryWordList *result_list) {
    // Safety check: prevent buffer overflow
    if (tiles_played >= BOARD_DIM) {
        return;
    }

    // If we've found a valid word of the right length, add it
    if (accepts && tiles_played == constraints->pattern_length) {
        // Final check: ensure all constraints are satisfied
        Rack temp_rack;
        rack_set_dist_size_and_reset(&temp_rack, ld_get_size(ld));

        // Build rack from used tiles
        for (int i = 0; i < tiles_played; i++) {
            rack_add_letter(&temp_rack, word[i]);
        }

        if (satisfies_constraints(&temp_rack, constraints)) {
            dictionary_word_list_add_word(result_list, word, tiles_played);
        }
    }

    // Don't continue if we've already used all tiles for this pattern length
    if (tiles_played >= constraints->pattern_length || node_index == 0) {
        return;
    }

    // Iterate through all possible letters at this node
    for (uint32_t i = node_index;; i++) {
        const uint32_t node = kwg_node(kwg, i);
        const MachineLetter ml = kwg_node_tile(node);
        const uint32_t new_node_index = kwg_node_arc_index_prefetch(node, kwg);

        if (rack_get_letter(rack, ml) > 0) {
            bool node_accepts = kwg_node_accepts(node);
            rack_take_letter(rack, ml);
            word[tiles_played] = ml;
            find_pattern_anagrams_recursive(kwg, new_node_index, rack, ld, constraints,
                                           word, tiles_played + 1, node_accepts, result_list);
            rack_add_letter(rack, ml);
        }

        if (kwg_node_is_end(node)) {
            break;
        }
    }
}

// Maximum number of results to prevent memory overflow
#define MAX_PATTERN_RESULTS 100000

// Simpler recursive function for pure wildcard patterns (no constraints)
// Just enumerate all words of a given length
static void find_words_by_length_recursive(const KWG *kwg, uint32_t node_index,
                                          MachineLetter *word, int depth,
                                          int target_length,
                                          DictionaryWordList *result_list,
                                          int *result_count) {
    // Safety checks
    if (node_index == 0 || *result_count >= MAX_PATTERN_RESULTS || depth >= BOARD_DIM) {
        return;
    }

    // Iterate through all possible letters at this node
    for (uint32_t i = node_index;; i++) {
        const uint32_t node = kwg_node(kwg, i);
        const MachineLetter ml = kwg_node_tile(node);
        const bool accepts = kwg_node_accepts(node);
        const uint32_t new_node_index = kwg_node_arc_index_prefetch(node, kwg);

        word[depth] = ml;

        // If this forms a word of the target length, add it
        if (accepts && depth + 1 == target_length) {
            dictionary_word_list_add_word(result_list, word, depth + 1);
            (*result_count)++;
            if (*result_count >= MAX_PATTERN_RESULTS) {
                return;
            }
        }

        // Continue searching if we haven't reached the target length
        if (depth + 1 < target_length) {
            find_words_by_length_recursive(kwg, new_node_index, word, depth + 1,
                                          target_length, result_list, result_count);
        }

        if (kwg_node_is_end(node)) {
            break;
        }
    }
}

WordList* letterbox_find_anagrams_by_pattern(const KWG *kwg, const LetterDistribution *ld,
                                             const char *pattern) {
    if (!kwg || !ld || !pattern) {
        return word_list_create();
    }

    // Parse the pattern
    PatternConstraints constraints;
    if (!parse_pattern(pattern, ld, &constraints)) {
        return word_list_create();
    }

    // Validate pattern length to prevent buffer overflows
    if (constraints.pattern_length > BOARD_DIM) {
        fprintf(stderr, "Pattern too long (%d chars). Maximum length is %d\n",
                constraints.pattern_length, BOARD_DIM);
        return word_list_create();
    }

    if (constraints.pattern_length == 0) {
        return word_list_create();
    }

    // Create dictionary word list to collect results
    DictionaryWordList *dict_list = dictionary_word_list_create();

    // Check if this is a pure wildcard pattern (no required letters or character classes)
    bool is_pure_wildcard = (constraints.num_wildcards == constraints.pattern_length &&
                             constraints.num_char_classes == 0);

    bool has_required_letters = false;
    for (int i = 1; i < 27; i++) {
        if (constraints.required_letters[i] > 0) {
            has_required_letters = true;
            break;
        }
    }
    is_pure_wildcard = is_pure_wildcard && !has_required_letters;

    MachineLetter word[BOARD_DIM];  // Use BOARD_DIM instead of RACK_SIZE to support longer patterns

    if (is_pure_wildcard) {
        // For pure wildcard patterns, just enumerate all words of the target length
        // This is much more efficient than the rack-based approach
        int result_count = 0;
        find_words_by_length_recursive(kwg, kwg_get_dawg_root_node_index(kwg),
                                       word, 0, constraints.pattern_length, dict_list,
                                       &result_count);
    } else {
        // For patterns with constraints, use the rack-based approach
        // Create a rack with enough of each letter to form any word of the pattern length
        Rack rack;
        rack_set_dist_size_and_reset(&rack, ld_get_size(ld));

        // Only add enough tiles to satisfy the pattern
        for (int i = 1; i < 27; i++) {
            // Add required letters
            for (int j = 0; j < constraints.required_letters[i]; j++) {
                rack_add_letter(&rack, i);
            }
            // Add one extra of each letter for wildcards and character classes
            if (constraints.num_wildcards > 0 || constraints.num_char_classes > 0) {
                rack_add_letter(&rack, i);
            }
        }

        find_pattern_anagrams_recursive(kwg, kwg_get_dawg_root_node_index(kwg),
                                       &rack, ld, &constraints, word, 0, false, dict_list);
    }

    // Sort the words
    dictionary_word_list_sort(dict_list);

    // Get unique words
    DictionaryWordList *unique_list = dictionary_word_list_create();
    dictionary_word_list_unique(dict_list, unique_list);
    dictionary_word_list_destroy(dict_list);

    // Convert dictionary words to alphagrams (sorted letter strings)
    // Group by alphagram
    DictionaryWordList *alphagram_list = dictionary_word_list_create();

    int count = dictionary_word_list_get_count(unique_list);
    for (int i = 0; i < count; i++) {
        const DictionaryWord *dw = dictionary_word_list_get_word(unique_list, i);
        int len = dictionary_word_get_length(dw);

        // Safety check: skip words that are too long
        if (len > BOARD_DIM) {
            continue;
        }

        // Create sorted version (alphagram)
        MachineLetter alphagram[BOARD_DIM];  // Use BOARD_DIM to support longer patterns
        memcpy(alphagram, dictionary_word_get_word(dw), len);

        // Sort the letters
        for (int j = 0; j < len - 1; j++) {
            for (int k = j + 1; k < len; k++) {
                if (alphagram[j] > alphagram[k]) {
                    MachineLetter temp = alphagram[j];
                    alphagram[j] = alphagram[k];
                    alphagram[k] = temp;
                }
            }
        }

        dictionary_word_list_add_word(alphagram_list, alphagram, len);
    }

    // Sort and get unique alphagrams
    dictionary_word_list_sort(alphagram_list);
    DictionaryWordList *unique_alphagrams = dictionary_word_list_create();
    dictionary_word_list_unique(alphagram_list, unique_alphagrams);
    dictionary_word_list_destroy(alphagram_list);
    dictionary_word_list_destroy(unique_list);

    // Convert to WordList format
    WordList *result = word_list_create();
    int alphagram_count = dictionary_word_list_get_count(unique_alphagrams);

    if (alphagram_count > 0) {
        result->words = malloc(alphagram_count * sizeof(char*));
        result->count = alphagram_count;

        StringBuilder *sb = string_builder_create();
        for (int i = 0; i < alphagram_count; i++) {
            const DictionaryWord *dw = dictionary_word_list_get_word(unique_alphagrams, i);
            string_builder_clear(sb);
            // Convert each machine letter to user visible
            for (int j = 0; j < dictionary_word_get_length(dw); j++) {
                MachineLetter ml = dictionary_word_get_word(dw)[j];
                string_builder_add_user_visible_letter(sb, ld, ml);
            }
            result->words[i] = string_builder_dump(sb, NULL);
        }
        string_builder_destroy(sb);
    }

    dictionary_word_list_destroy(unique_alphagrams);
    return result;
}

// Optimized function to find words exactly one letter longer than input
// This avoids the subanagram problem by only searching for exact length matches
WordList* letterbox_find_anagrams_with_blank(const KWG *kwg, const LetterDistribution *ld,
                                             const char *letters) {
    if (!kwg || !ld || !letters) {
        return word_list_create();
    }

    int base_length = strlen(letters);
    int target_length = base_length + 1;

    // Safety check
    if (target_length > BOARD_DIM) {
        return word_list_create();
    }

    // Create a rack from the input letters
    Rack base_rack;
    rack_set_dist_size_and_reset(&base_rack, ld_get_size(ld));
    int num_letters = rack_set_to_string(ld, &base_rack, letters);
    if (num_letters < 0) {
        return word_list_create();
    }

    // Dictionary list to collect all matching words
    DictionaryWordList *dict_list = dictionary_word_list_create();
    MachineLetter word[BOARD_DIM];

    // Try adding each possible letter (1-26) to the rack
    for (MachineLetter blank_letter = 1; blank_letter < 27; blank_letter++) {
        Rack search_rack = base_rack;
        rack_add_letter(&search_rack, blank_letter);

        // Find all anagrams using this rack, but only keep those of exact target length
        // Use a temporary list for this letter's results
        DictionaryWordList *temp_list = dictionary_word_list_create();
        find_anagrams_recursive(kwg, kwg_get_dawg_root_node_index(kwg),
                               &search_rack, ld, word, 0, false, temp_list);

        // Filter to only words of target length
        int temp_count = dictionary_word_list_get_count(temp_list);
        for (int i = 0; i < temp_count; i++) {
            const DictionaryWord *dw = dictionary_word_list_get_word(temp_list, i);
            if (dictionary_word_get_length(dw) == target_length) {
                dictionary_word_list_add_word(dict_list,
                                            dictionary_word_get_word(dw),
                                            dictionary_word_get_length(dw));
            }
        }

        dictionary_word_list_destroy(temp_list);
    }

    // Sort and get unique words
    dictionary_word_list_sort(dict_list);
    DictionaryWordList *unique_list = dictionary_word_list_create();
    dictionary_word_list_unique(dict_list, unique_list);
    dictionary_word_list_destroy(dict_list);

    // Convert dictionary words to alphagrams
    DictionaryWordList *alphagram_list = dictionary_word_list_create();
    int count = dictionary_word_list_get_count(unique_list);

    for (int i = 0; i < count; i++) {
        const DictionaryWord *dw = dictionary_word_list_get_word(unique_list, i);
        int len = dictionary_word_get_length(dw);

        // Create sorted version (alphagram)
        MachineLetter alphagram[BOARD_DIM];
        memcpy(alphagram, dictionary_word_get_word(dw), len);

        // Sort the letters
        for (int j = 0; j < len - 1; j++) {
            for (int k = j + 1; k < len; k++) {
                if (alphagram[j] > alphagram[k]) {
                    MachineLetter temp = alphagram[j];
                    alphagram[j] = alphagram[k];
                    alphagram[k] = temp;
                }
            }
        }

        dictionary_word_list_add_word(alphagram_list, alphagram, len);
    }

    // Sort and get unique alphagrams
    dictionary_word_list_sort(alphagram_list);
    DictionaryWordList *unique_alphagrams = dictionary_word_list_create();
    dictionary_word_list_unique(alphagram_list, unique_alphagrams);
    dictionary_word_list_destroy(alphagram_list);
    dictionary_word_list_destroy(unique_list);

    // Convert to WordList format
    WordList *result = word_list_create();
    int alphagram_count = dictionary_word_list_get_count(unique_alphagrams);

    // fprintf(stderr, "[C] letterbox_find_anagrams_with_blank('%s'): found %d alphagrams\n",
    //         letters, alphagram_count);

    if (alphagram_count > 0) {
        result->words = malloc(alphagram_count * sizeof(char*));
        result->count = alphagram_count;

        StringBuilder *sb = string_builder_create();
        for (int i = 0; i < alphagram_count; i++) {
            const DictionaryWord *dw = dictionary_word_list_get_word(unique_alphagrams, i);
            string_builder_clear(sb);
            for (int j = 0; j < dictionary_word_get_length(dw); j++) {
                MachineLetter ml = dictionary_word_get_word(dw)[j];
                string_builder_add_user_visible_letter(sb, ld, ml);
            }
            result->words[i] = string_builder_dump(sb, NULL);
            // if (i < 5) {  // Print first few for debugging
            //     fprintf(stderr, "[C]   alphagram[%d]: %s\n", i, result->words[i]);
            // }
        }
        string_builder_destroy(sb);
    }

    dictionary_word_list_destroy(unique_alphagrams);
    return result;
}

// Optimized function to find words exactly two letters longer than input
WordList* letterbox_find_anagrams_with_two_blanks(const KWG *kwg, const LetterDistribution *ld,
                                                   const char *letters) {
    if (!kwg || !ld || !letters) {
        return word_list_create();
    }

    int base_length = strlen(letters);
    int target_length = base_length + 2;

    // Safety check
    if (target_length > BOARD_DIM) {
        return word_list_create();
    }

    // Create a rack from the input letters
    Rack base_rack;
    rack_set_dist_size_and_reset(&base_rack, ld_get_size(ld));
    int num_letters = rack_set_to_string(ld, &base_rack, letters);
    if (num_letters < 0) {
        return word_list_create();
    }

    // Dictionary list to collect all matching words
    DictionaryWordList *dict_list = dictionary_word_list_create();
    MachineLetter word[BOARD_DIM];

    // Try adding each pair of letters (including duplicates like AA, BB, etc.)
    for (MachineLetter blank1 = 1; blank1 < 27; blank1++) {
        for (MachineLetter blank2 = 1; blank2 < 27; blank2++) {
            Rack search_rack = base_rack;
            rack_add_letter(&search_rack, blank1);
            rack_add_letter(&search_rack, blank2);

            // Find all anagrams using this rack, but only keep those of exact target length
            DictionaryWordList *temp_list = dictionary_word_list_create();
            find_anagrams_recursive(kwg, kwg_get_dawg_root_node_index(kwg),
                                   &search_rack, ld, word, 0, false, temp_list);

            // Filter to only words of target length
            int temp_count = dictionary_word_list_get_count(temp_list);
            for (int i = 0; i < temp_count; i++) {
                const DictionaryWord *dw = dictionary_word_list_get_word(temp_list, i);
                if (dictionary_word_get_length(dw) == target_length) {
                    dictionary_word_list_add_word(dict_list,
                                                dictionary_word_get_word(dw),
                                                dictionary_word_get_length(dw));
                }
            }

            dictionary_word_list_destroy(temp_list);
        }
    }

    // Sort and get unique words
    dictionary_word_list_sort(dict_list);
    DictionaryWordList *unique_list = dictionary_word_list_create();
    dictionary_word_list_unique(dict_list, unique_list);
    dictionary_word_list_destroy(dict_list);

    // Convert dictionary words to alphagrams
    DictionaryWordList *alphagram_list = dictionary_word_list_create();
    int count = dictionary_word_list_get_count(unique_list);

    for (int i = 0; i < count; i++) {
        const DictionaryWord *dw = dictionary_word_list_get_word(unique_list, i);
        int len = dictionary_word_get_length(dw);

        // Create sorted version (alphagram)
        MachineLetter alphagram[BOARD_DIM];
        memcpy(alphagram, dictionary_word_get_word(dw), len);

        // Sort the letters
        for (int j = 0; j < len - 1; j++) {
            for (int k = j + 1; k < len; k++) {
                if (alphagram[j] > alphagram[k]) {
                    MachineLetter temp = alphagram[j];
                    alphagram[j] = alphagram[k];
                    alphagram[k] = temp;
                }
            }
        }

        dictionary_word_list_add_word(alphagram_list, alphagram, len);
    }

    // Sort and get unique alphagrams
    dictionary_word_list_sort(alphagram_list);
    DictionaryWordList *unique_alphagrams = dictionary_word_list_create();
    dictionary_word_list_unique(alphagram_list, unique_alphagrams);
    dictionary_word_list_destroy(alphagram_list);
    dictionary_word_list_destroy(unique_list);

    // Convert to WordList format
    WordList *result = word_list_create();
    int alphagram_count = dictionary_word_list_get_count(unique_alphagrams);

    if (alphagram_count > 0) {
        result->words = malloc(alphagram_count * sizeof(char*));
        result->count = alphagram_count;

        StringBuilder *sb = string_builder_create();
        for (int i = 0; i < alphagram_count; i++) {
            const DictionaryWord *dw = dictionary_word_list_get_word(unique_alphagrams, i);
            string_builder_clear(sb);
            for (int j = 0; j < dictionary_word_get_length(dw); j++) {
                MachineLetter ml = dictionary_word_get_word(dw)[j];
                string_builder_add_user_visible_letter(sb, ld, ml);
            }
            result->words[i] = string_builder_dump(sb, NULL);
        }
        string_builder_destroy(sb);
    }

    dictionary_word_list_destroy(unique_alphagrams);
    return result;
}
