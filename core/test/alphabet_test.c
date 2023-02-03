#include <assert.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/alphabet.h"

#include "test_util.h"

void test_alphabet() {
    Config * config = create_america_sort_by_score_config();
    // Test blank
    assert(get_blanked_machine_letter(0) == BLANK_OFFSET);
    assert(get_blanked_machine_letter(1) == BLANK_OFFSET + 1);
    assert(get_blanked_machine_letter(BLANK_OFFSET-1) == BLANK_OFFSET + (BLANK_OFFSET-1));
    assert(get_blanked_machine_letter(BLANK_OFFSET) == BLANK_OFFSET);
    assert(get_blanked_machine_letter(BLANK_OFFSET+1) == BLANK_OFFSET+1);

    // Test unblank
    assert(get_unblanked_machine_letter(0) == 0);
    assert(get_unblanked_machine_letter(1) == 1);
    assert(get_unblanked_machine_letter(BLANK_OFFSET-1) == BLANK_OFFSET-1);
    assert(get_unblanked_machine_letter(BLANK_OFFSET) == 0);
    assert(get_unblanked_machine_letter(BLANK_OFFSET+1) == 1);

    // Test val
    // separation token
    assert(val(config->gaddag->alphabet, SEPARATION_TOKEN) == SEPARATION_MACHINE_LETTER);
    // blank
    assert(val(config->gaddag->alphabet, BLANK_TOKEN) == BLANK_MACHINE_LETTER);
    // played through
    assert(val(config->gaddag->alphabet, ASCII_PLAYED_THROUGH) == PLAYED_THROUGH_MARKER);
    // blank offset
    assert(val(config->gaddag->alphabet, 'a') == 100);
    assert(val(config->gaddag->alphabet, 'b') == 101);
    // no blank offset
    assert(val(config->gaddag->alphabet, 'C') == 2);
    assert(val(config->gaddag->alphabet, 'D') == 3);

    // Test user visible
    // separation token
    // The separation letter and machine letter should be the only machine
    // letters that map to the same value, since
    // SEPARATION_MACHINE_LETTER == BLANK_MACHINE_LETTER
    assert(user_visible_letter(config->gaddag->alphabet, SEPARATION_MACHINE_LETTER) == BLANK_TOKEN);
    // blank
    assert(user_visible_letter(config->gaddag->alphabet, BLANK_MACHINE_LETTER) == BLANK_TOKEN);
    // played through
    assert(user_visible_letter(config->gaddag->alphabet, PLAYED_THROUGH_MARKER) == ASCII_PLAYED_THROUGH);
    // blank offset
    assert(user_visible_letter(config->gaddag->alphabet, 100) == 'a');
    assert(user_visible_letter(config->gaddag->alphabet, 101) == 'b');
    // no blank offset
    assert(user_visible_letter(config->gaddag->alphabet, 2) == 'C');
    assert(user_visible_letter(config->gaddag->alphabet, 3) == 'D');

    destroy_config(config);
}
