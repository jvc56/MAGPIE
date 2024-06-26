#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../src/def/file_handler_defs.h"

#include "../../src/ent/file_handler.h"

#include "../../src/util/log.h"
#include "../../src/util/string_util.h"
#include "../../src/util/util.h"

#include "test_util.h"

typedef struct TestFIFOArgs {
  bool done_reading;
  bool done_writing;
  const char *fifo_filename;
} TestFIFOArgs;

TestFIFOArgs *test_fifo_args_create(const char *fifo_filename) {
  TestFIFOArgs *test_fifo_args = malloc_or_die(sizeof(TestFIFOArgs));
  test_fifo_args->done_reading = false;
  test_fifo_args->done_writing = false;
  test_fifo_args->fifo_filename = fifo_filename;
  return test_fifo_args;
}

void test_fifo_args_destroy(TestFIFOArgs *test_fifo_args) {
  if (!test_fifo_args) {
    return;
  }
  free(test_fifo_args);
}

void test_regular_file(void) {
  char *test_output_filename1 = get_test_filename("output");
  char *test_output_filename2 = get_test_filename("output2");

  reset_file(test_output_filename1);
  reset_file(test_output_filename2);

  FileHandler *fh = file_handler_create_from_filename(test_output_filename1,
                                                      FILE_HANDLER_MODE_WRITE);

  file_handler_write(fh, "line 1\n");
  char *file_content1 = get_string_from_file(test_output_filename1);

  assert_strings_equal(file_content1, "line 1\n");
  free(file_content1);

  file_handler_write(fh, "line 2\n");
  char *file_content2 = get_string_from_file(test_output_filename1);

  assert_strings_equal(file_content2, "line 1\nline 2\n");
  free(file_content2);

  // Set to a different file
  file_handler_set_filename(fh, test_output_filename2, FILE_HANDLER_MODE_WRITE);

  file_handler_write(fh, "line 3\n");
  char *file_content3 = get_string_from_file(test_output_filename2);

  assert_strings_equal(file_content3, "line 3\n");
  free(file_content3);

  file_handler_write(fh, "line 4\n");
  char *file_content4 = get_string_from_file(test_output_filename2);

  assert_strings_equal(file_content4, "line 3\nline 4\n");
  free(file_content4);

  // Set back to file 1
  file_handler_set_filename(fh, test_output_filename1, FILE_HANDLER_MODE_WRITE);
  file_handler_write(fh, "line 5");
  char *file_content5 = get_string_from_file(test_output_filename1);

  assert_strings_equal(file_content5, "line 1\nline 2\nline 5");
  free(file_content5);

  // Set to read file 1
  file_handler_set_filename(fh, test_output_filename1, FILE_HANDLER_MODE_READ);

  char *file_1_line_1 = file_handler_get_line(fh);
  assert_strings_equal("line 1", file_1_line_1);
  free(file_1_line_1);

  char *file_1_line_2 = file_handler_get_line(fh);
  assert_strings_equal("line 2", file_1_line_2);
  free(file_1_line_2);

  char *file_1_line_3 = file_handler_get_line(fh);
  assert_strings_equal("line 5", file_1_line_3);
  free(file_1_line_3);

  // File should be empty
  char *file_1_eof_result = file_handler_get_line(fh);
  if (file_1_eof_result) {
  }
  assert(!file_1_eof_result);

  file_handler_destroy(fh);
  delete_file(test_output_filename1);
  delete_file(test_output_filename2);
  free(test_output_filename1);
  free(test_output_filename2);
}

void block_for_fifo_finish(bool *done_reading, bool *done_writing,
                           int max_seconds) {
  // Poll for the end of the command
  int seconds_elapsed = 0;
  while (1) {
    if (*done_reading && done_writing) {
      break;
    } else {
      sleep(1);
    }
    seconds_elapsed++;
    if (seconds_elapsed >= max_seconds) {
      log_fatal("Test FIFO aborted after %d seconds", max_seconds);
    }
  }
}

void *read_fifo_thread(void *uncasted_test_fifo_args) {
  TestFIFOArgs *test_fifo_args = (TestFIFOArgs *)uncasted_test_fifo_args;

  const char *fifo_filename = test_fifo_args->fifo_filename;

  FileHandler *fifo_reader =
      file_handler_create_from_filename(fifo_filename, FILE_HANDLER_MODE_READ);

  StringBuilder *result_builder = string_builder_create();
  while (1) {
    char *line = file_handler_get_line(fifo_reader);
    if (!line) {
      break;
    }
    string_builder_add_string(result_builder, line);
    free(line);
  }
  assert_strings_equal("abc", string_builder_peek(result_builder));
  string_builder_destroy(result_builder);
  file_handler_destroy(fifo_reader);
  test_fifo_args->done_reading = true;
  return NULL;
}

void *write_fifo_thread(void *uncasted_test_fifo_args) {
  TestFIFOArgs *test_fifo_args = (TestFIFOArgs *)uncasted_test_fifo_args;

  const char *fifo_filename = test_fifo_args->fifo_filename;

  FileHandler *fifo_writer =
      file_handler_create_from_filename(fifo_filename, FILE_HANDLER_MODE_WRITE);

  sleep(1);
  file_handler_write(fifo_writer, "a\n");
  sleep(1);
  file_handler_write(fifo_writer, "b\n");
  sleep(1);
  file_handler_write(fifo_writer, "c\n");
  sleep(1);
  file_handler_destroy(fifo_writer);

  test_fifo_args->done_writing = true;

  return NULL;
}

void test_fifo(void) {
  char *test_fifo_filename = get_test_filename("fifo_test");
  fifo_create(test_fifo_filename);

  TestFIFOArgs *test_fifo_args = test_fifo_args_create(test_fifo_filename);

  pthread_t reader_thread_id;
  pthread_create(&reader_thread_id, NULL, read_fifo_thread, test_fifo_args);
  pthread_detach(reader_thread_id);

  pthread_t writer_thread_id;
  pthread_create(&writer_thread_id, NULL, write_fifo_thread, test_fifo_args);
  pthread_detach(writer_thread_id);

  block_for_fifo_finish(&test_fifo_args->done_reading,
                        &test_fifo_args->done_writing, 8);

  test_fifo_args_destroy(test_fifo_args);
  free(test_fifo_filename);
}

void test_file_handler(void) {
  test_regular_file();
  test_fifo();
}
