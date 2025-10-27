#include "magpie_wrapper.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void test_antipole_hooks() {
    printf("Testing ANTIPOLE hooks...\n");

    Config* config = letterbox_create_config("../data", "CSW24");
    if (!config) {
        printf("FAILED: Could not create config\n");
        return;
    }

    KWG* kwg = letterbox_get_kwg(config);
    LetterDistribution* ld = letterbox_get_ld(config);

    if (!kwg || !ld) {
        printf("FAILED: Could not get KWG or LD\n");
        letterbox_destroy_config(config);
        return;
    }

    // Test front hooks
    char* front_hooks = letterbox_find_front_hooks(kwg, ld, "ANTIPOLE");
    printf("Front hooks for ANTIPOLE: '%s'\n", front_hooks);

    if (strchr(front_hooks, 'R') == NULL) {
        printf("FAILED: Expected 'R' in front hooks, got '%s'\n", front_hooks);
    } else {
        printf("PASSED: Found 'R' in front hooks\n");
    }

    free(front_hooks);

    // Test back hooks
    char* back_hooks = letterbox_find_back_hooks(kwg, ld, "ANTIPOLE");
    printf("Back hooks for ANTIPOLE: '%s'\n", back_hooks);

    if (strchr(back_hooks, 'S') == NULL) {
        printf("FAILED: Expected 'S' in back hooks, got '%s'\n", back_hooks);
    } else {
        printf("PASSED: Found 'S' in back hooks\n");
    }

    free(back_hooks);

    letterbox_destroy_config(config);
}

void test_simple_words() {
    printf("\nTesting simple word hooks...\n");

    Config* config = letterbox_create_config("../data", "CSW24");
    if (!config) {
        printf("FAILED: Could not create config\n");
        return;
    }

    KWG* kwg = letterbox_get_kwg(config);
    LetterDistribution* ld = letterbox_get_ld(config);

    // Test CAT (SCAT, CATS)
    char* front_hooks = letterbox_find_front_hooks(kwg, ld, "CAT");
    char* back_hooks = letterbox_find_back_hooks(kwg, ld, "CAT");
    printf("CAT: front='%s' back='%s'\n", front_hooks, back_hooks);

    if (strchr(front_hooks, 'S') == NULL) {
        printf("FAILED: Expected 'S' in front hooks for CAT\n");
    } else {
        printf("PASSED: Found 'S' in front hooks for CAT\n");
    }

    if (strchr(back_hooks, 'S') == NULL) {
        printf("FAILED: Expected 'S' in back hooks for CAT\n");
    } else {
        printf("PASSED: Found 'S' in back hooks for CAT\n");
    }

    free(front_hooks);
    free(back_hooks);

    letterbox_destroy_config(config);
}

void test_more_words() {
    printf("\nTesting more word hooks...\n");

    Config* config = letterbox_create_config("../data", "CSW24");
    if (!config) {
        printf("FAILED: Could not create config\n");
        return;
    }

    KWG* kwg = letterbox_get_kwg(config);
    LetterDistribution* ld = letterbox_get_ld(config);

    // Test RAIN (BRAIN, RAINS/RAINY)
    char* front_hooks = letterbox_find_front_hooks(kwg, ld, "RAIN");
    char* back_hooks = letterbox_find_back_hooks(kwg, ld, "RAIN");
    printf("RAIN: front='%s' back='%s'\n", front_hooks, back_hooks);
    free(front_hooks);
    free(back_hooks);

    // Test TABLE (STABLE, TABLES)
    front_hooks = letterbox_find_front_hooks(kwg, ld, "TABLE");
    back_hooks = letterbox_find_back_hooks(kwg, ld, "TABLE");
    printf("TABLE: front='%s' back='%s'\n", front_hooks, back_hooks);
    free(front_hooks);
    free(back_hooks);

    letterbox_destroy_config(config);
}

int main() {
    test_antipole_hooks();
    test_simple_words();
    test_more_words();
    printf("\nAll tests completed!\n");
    return 0;
}
