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

WordList* word_list_create() {
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
    MachineLetter word[RACK_SIZE];
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
static void find_extensions_recursive(const KWG *kwg, const LetterDistribution *ld,
                                     uint32_t node_index, MachineLetter *extension,
                                     int depth, int max_depth,
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
            if (kwg_node_accepts(node)) {
                dictionary_word_list_add_word(result_list, extension, depth + 1);
            }

            // Continue searching deeper
            find_extensions_recursive(kwg, ld, next_node_index, extension,
                                    depth + 1, max_depth, result_list);
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
                             max_extension_length, ext_list);

    // Sort alphabetically
    dictionary_word_list_sort(ext_list);

    // Group extensions by length and format, limiting to 40 chars per line
    StringBuilder *sb = string_builder_create();
    int count = dictionary_word_list_get_count(ext_list);

    const int MAX_CHARS_PER_LINE = 40;

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
                             max_extension_length, ext_list);

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

    const int MAX_CHARS_PER_LINE = 40;

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
