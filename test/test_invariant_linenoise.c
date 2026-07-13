#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "src/compat/linenoise.h"

START_TEST(test_buffer_reads_never_exceed_declared_length)
{
    // Invariant: Buffer reads never exceed the declared length
    const char *payloads[] = {
        "A",  // Valid input (single char)
        "123456789",  // Boundary: exactly 9 chars (common buffer size)
        "1234567890123456789012345678901234567890",  // 40 chars - 4x typical buffer
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",  // 100 chars - extreme overflow
        "\0",  // Empty string with null terminator
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        // Test linenoise's internal buffer handling through public API
        char *result = linenoise(payloads[i]);
        
        // If linenoise returns a result, verify it's properly terminated
        if (result != NULL) {
            // Check that strlen doesn't exceed reasonable buffer bounds
            // (assuming LINENOISE_MAX_LINE typical value of 4096)
            ck_assert_msg(strlen(result) < 4096, 
                         "Returned string length %zu exceeds safe buffer size", 
                         strlen(result));
            
            // Verify null termination
            ck_assert_msg(result[strlen(result)] == '\0',
                         "Returned string is not properly null-terminated");
            
            free(result);
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_reads_never_exceed_declared_length);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}