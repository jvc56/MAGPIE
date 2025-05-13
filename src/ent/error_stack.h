#ifndef ERROR_STACK_H
#define ERROR_STACK_H

#include <stdbool.h>

#include "../def/error_stack_defs.h"

typedef struct ErrorStack ErrorStack;

ErrorStack *error_stack_create(void);
void error_stack_reset(ErrorStack *error_status);
void error_stack_destroy(ErrorStack *error_status);
void error_stack_push(ErrorStack *error_status, error_code_t error_code,
                      char *msg);
error_code_t error_stack_top(ErrorStack *error_stack);
char *error_stack_string(ErrorStack *error_stack);
void error_stack_print(ErrorStack *error_stack);
bool error_stack_is_empty(ErrorStack *error_stack);

#endif
