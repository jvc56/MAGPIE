#include "error_stack.h"

#include <stdbool.h>
#include <stdio.h>

#include "../util/io.h"
#include "../util/string_util.h"
#include "../util/util.h"

#define ERROR_STACK_CAPACITY 100

struct ErrorStack {
  int size;
  error_code_t error_codes[ERROR_STACK_CAPACITY];
  char *msgs[ERROR_STACK_CAPACITY];
};

ErrorStack *error_stack_create(void) {
  ErrorStack *error_stack = malloc_or_die(sizeof(ErrorStack));
  error_stack->size = 0;
  for (int i = 0; i < ERROR_STACK_CAPACITY; i++) {
    error_stack->error_codes[i] = ERROR_STATUS_SUCCESS;
    error_stack->msgs[i] = NULL;
  }
  return error_stack;
}

void error_stack_reset(ErrorStack *error_stack) {
  error_stack->size = 0;
  for (int i = 0; i < ERROR_STACK_CAPACITY; i++) {
    error_stack->error_codes[i] = ERROR_STATUS_SUCCESS;
    free(error_stack->msgs[i]);
    error_stack->msgs[i] = NULL;
  }
}

void error_stack_destroy(ErrorStack *error_stack) {
  if (!error_stack) {
    return;
  }
  for (int i = 0; i < ERROR_STACK_CAPACITY; i++) {
    free(error_stack->msgs[i]);
  }
  free(error_stack);
}

// Takes ownership of the msg
void error_stack_push(ErrorStack *error_stack, error_code_t error_code,
                      char *msg) {
  if (error_stack->size == ERROR_STACK_CAPACITY) {
    log_fatal("error stack is full");
  }
  error_stack->error_codes[error_stack->size] = error_code;
  error_stack->msgs[error_stack->size] = msg;
  error_stack->size++;
}

bool error_stack_is_empty(ErrorStack *error_stack) {
  return error_stack->size == 0;
}

error_code_t error_stack_top(ErrorStack *error_stack) {
  if (error_stack_is_empty(error_stack)) {
    return ERROR_STATUS_SUCCESS;
  }
  return error_stack->error_codes[error_stack->size - 1];
}

char *error_stack_get_string_and_reset(ErrorStack *error_stack) {
  if (error_stack_is_empty(error_stack)) {
    return NULL;
  }
  StringBuilder *string_builder = string_builder_create();
  for (int i = error_stack->size - 1; i >= 0; i--) {
    string_builder_add_formatted_string(string_builder, "(error %d) %s\n",
                                        error_stack->error_codes[i],
                                        error_stack->msgs[i]);
  }
  char *error_string = string_builder_dump(string_builder, NULL);
  string_builder_destroy(string_builder);
  error_stack_reset(error_stack);
  return error_string;
}

void error_stack_print_and_reset(ErrorStack *error_stack) {
  char *error_string = error_stack_get_string_and_reset(error_stack);
  if (!error_string) {
    return;
  }
  write_to_stream_err(error_string);
  free(error_string);
}