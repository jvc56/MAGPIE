#ifndef LETTERBOX_MAGPIE_WRAPPER_H
#define LETTERBOX_MAGPIE_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct Config Config;
typedef struct KWG KWG;
typedef struct LetterDistribution LetterDistribution;

// Config and KWG creation
Config* letterbox_create_config(const char *data_path, const char *lexicon);
void letterbox_destroy_config(Config *config);
KWG* letterbox_get_kwg(Config *config);
LetterDistribution* letterbox_get_ld(Config *config);

// Word list result structure
typedef struct {
    char **words;
    int count;
} WordList;

// Create/destroy word lists
WordList* word_list_create();
void word_list_destroy(WordList *list);

// Find all anagrams of the given letters (uses DAWG root)
// Returns WordList with all valid words that can be formed
WordList* letterbox_find_anagrams(const KWG *kwg, const LetterDistribution *ld, const char *letters);

// Find front hooks for a word (letters that can be added to the start)
// Returns string of valid hook letters (e.g., "AES" means A, E, S can hook)
char* letterbox_find_front_hooks(const KWG *kwg, const LetterDistribution *ld, const char *word);

// Find back hooks for a word (letters that can be added to the end)
// Returns string of valid hook letters
char* letterbox_find_back_hooks(const KWG *kwg, const LetterDistribution *ld, const char *word);

// Find multi-letter back extensions (e.g., for "CENTER" finds "ED\nING")
// Returns string with one line per extension length, extensions separated by spaces
char* letterbox_find_back_extensions(const KWG *kwg, const LetterDistribution *ld,
                                     const char *word, int max_extension_length);

// Find multi-letter front extensions (e.g., for "CENTER" finds extensions that go before)
// Returns string with one line per extension length, extensions separated by spaces
char* letterbox_find_front_extensions(const KWG *kwg, const LetterDistribution *ld,
                                      const char *word, int max_extension_length);

// Find anagrams matching a pattern with wildcards and character classes
// Pattern syntax:
//   - Letters (A-Z): required letters
//   - . or ?: wildcard (any letter)
//   - [ABC]: character class (at least one of A, B, or C must be used)
// Examples:
//   - "......." = all 7-letter words
//   - "A??E???" = 7-letter words with A and E
//   - "[JQXZ]......" = 7-letter words with at least one of J, Q, X, Z
// Returns WordList with all alphagrams (sorted letter combinations) matching the pattern
WordList* letterbox_find_anagrams_by_pattern(const KWG *kwg, const LetterDistribution *ld,
                                             const char *pattern);

// Find words that are exactly one letter longer than the input letters
// This is an optimized version for finding blank extensions (word + one wild tile)
// Returns WordList with all alphagrams of length strlen(letters) + 1
WordList* letterbox_find_anagrams_with_blank(const KWG *kwg, const LetterDistribution *ld,
                                             const char *letters);

#ifdef __cplusplus
}
#endif

#endif // LETTERBOX_MAGPIE_WRAPPER_H
