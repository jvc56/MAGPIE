#include <assert.h>
#include <stdio.h>

#include "../src/config.h"

#include "test_util.h"
#include "test_config.h"

void test_english_letter_distribution(TestConfig * test_config) {
    Config * config = get_america_config(test_config);
    uint32_t dist[] = {9, 2, 2, 4, 12, 2, 3, 2, 9, 1, 1, 4, 2, 6, 8, 2, 1, 6, 4, 6, 4, 2, 2, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    uint32_t scores[] = {1, 3, 3, 2, 1, 4, 2, 4, 1, 8, 5, 1, 3, 1, 1, 3, 10, 1, 1, 1, 1, 4, 4, 8, 4, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t is_vowel[] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(sizeof(dist) / sizeof(int) == (RACK_ARRAY_SIZE));
    assert(sizeof(scores) / sizeof(int) == (RACK_ARRAY_SIZE));
    assert(sizeof(is_vowel) / sizeof(int) == (RACK_ARRAY_SIZE));
    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        assert(config->letter_distribution->distribution[i] == dist[i]);
        assert(config->letter_distribution->scores[i] == scores[i]);
        assert(config->letter_distribution->is_vowel[i] == is_vowel[i]);
    }
    for (int i = (RACK_ARRAY_SIZE); i < (MACHINE_LETTER_MAX_VALUE + 1); i++) {
        assert(config->letter_distribution->distribution[i] == 0);
        assert(config->letter_distribution->scores[i] == 0);
        assert(config->letter_distribution->is_vowel[i] == 0);
    }

    
}

void test_letter_distribution(TestConfig * test_config) {
    test_english_letter_distribution(test_config);
}
