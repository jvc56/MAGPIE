#include <assert.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/alphabet.h"

#include "test_util.h"
#include "superconfig.h"

void test_alphabet(SuperConfig * superconfig) {
    Config * config = get_america_config(superconfig);
    // Test blank
    assert(get_blanked_machine_letter(1) == (1 | BLANK_MASK));
    assert(get_blanked_machine_letter(5) == (5 | BLANK_MASK));

    // Test unblank
    assert(get_unblanked_machine_letter(1) == (1 & UNBLANK_MASK));
    assert(get_unblanked_machine_letter(5) == (5 & UNBLANK_MASK));

    // Test val
    // separation token
    assert(val(config->kwg->alphabet, SEPARATION_TOKEN) == SEPARATION_MACHINE_LETTER);
    // blank
    assert(val(config->kwg->alphabet, BLANK_TOKEN) == BLANK_MACHINE_LETTER);
    // played through
    assert(val(config->kwg->alphabet, ASCII_PLAYED_THROUGH) == PLAYED_THROUGH_MARKER);
    // blank
    assert(val(config->kwg->alphabet, 'a') == get_blanked_machine_letter(1));
    assert(val(config->kwg->alphabet, 'b') == get_blanked_machine_letter(2));
    // not blank
    assert(val(config->kwg->alphabet, 'C') == 3);
    assert(val(config->kwg->alphabet, 'D') == 4);

    // Test user visible
    // separation token
    // The separation letter and machine letter should be the only machine
    // letters that map to the same value, since
    // SEPARATION_MACHINE_LETTER == BLANK_MACHINE_LETTER
    assert(user_visible_letter(config->kwg->alphabet, SEPARATION_MACHINE_LETTER) == BLANK_TOKEN);
    // blank
    assert(user_visible_letter(config->kwg->alphabet, BLANK_MACHINE_LETTER) == BLANK_TOKEN);
    // played through
    assert(user_visible_letter(config->kwg->alphabet, PLAYED_THROUGH_MARKER) == BLANK_TOKEN);
    // blank
    assert(user_visible_letter(config->kwg->alphabet, get_blanked_machine_letter(1)) == 'a');
    assert(user_visible_letter(config->kwg->alphabet, get_blanked_machine_letter(2)) == 'b');
    // not blank
    assert(user_visible_letter(config->kwg->alphabet, 3) == 'C');
    assert(user_visible_letter(config->kwg->alphabet, 4) == 'D');

}
