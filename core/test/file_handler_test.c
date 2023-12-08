#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/ent/thread_control.h"
#include "../src/util/string_util.h"
#include "../src/util/log.h"
#include "../src/util/util.h"

#include "test_constants.h"
#include "test_util.h"

typedef struct TestFIFOArgs {
  bool done_reading;
  bool done_writing;
  const char *fifo_filename;
} TestFIFOArgs;

TestFIFOArgs *create_test_fifo_args(const char *fifo_filename) {
  TestFIFOArgs *test_fifo_args = malloc_or_die(sizeof(TestFIFOArgs));
  test_fifo_args->done_reading = false;
  test_fifo_args->done_writing = false;
  test_fifo_args->fifo_filename = fifo_filename;
  return test_fifo_args;
}

void destroy_test_fifo_args(TestFIFOArgs *test_fifo_args) {
  free(test_fifo_args);
}

void test_regular_file() {
  char *test_output_filename1 = get_test_filename("output");
  char *test_output_filename2 = get_test_filename("output2");

  reset_file(test_output_filename1);
  reset_file(test_output_filename2);

  FileHandler *fh = create_file_handler_from_filename(test_output_filename1,
                                                      FILE_HANDLER_MODE_WRITE);

  write_to_file(fh, "line 1\n");
  char *file_content1 = get_string_from_file(test_output_filename1);

  assert_strings_equal(file_content1, "line 1\n");
  free(file_content1);

  write_to_file(fh, "line 2\n");
  char *file_content2 = get_string_from_file(test_output_filename1);

  assert_strings_equal(file_content2, "line 1\nline 2\n");
  free(file_content2);

  // Set to a different file
  set_file_handler(fh, test_output_filename2, FILE_HANDLER_MODE_WRITE);

  write_to_file(fh, "line 3\n");
  char *file_content3 = get_string_from_file(test_output_filename2);

  assert_strings_equal(file_content3, "line 3\n");
  free(file_content3);

  write_to_file(fh, "line 4\n");
  char *file_content4 = get_string_from_file(test_output_filename2);

  assert_strings_equal(file_content4, "line 3\nline 4\n");
  free(file_content4);

  // Set back to file 1
  set_file_handler(fh, test_output_filename1, FILE_HANDLER_MODE_WRITE);
  write_to_file(fh, "line 5");
  char *file_content5 = get_string_from_file(test_output_filename1);

  assert_strings_equal(file_content5, "line 1\nline 2\nline 5");
  free(file_content5);

  // Set to read file 1
  set_file_handler(fh, test_output_filename1, FILE_HANDLER_MODE_READ);

  char *file_1_line_1 = getline_from_file(fh);
  assert_strings_equal("line 1", file_1_line_1);
  free(file_1_line_1);

  char *file_1_line_2 = getline_from_file(fh);
  assert_strings_equal("line 2", file_1_line_2);
  free(file_1_line_2);

  char *file_1_line_3 = getline_from_file(fh);
  assert_strings_equal("line 5", file_1_line_3);
  free(file_1_line_3);

  // File should be empty
  char *file_1_eof_result = getline_from_file(fh);
  if (file_1_eof_result) {
  }
  assert(!file_1_eof_result);

  destroy_file_handler(fh);
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
      create_file_handler_from_filename(fifo_filename, FILE_HANDLER_MODE_READ);

  StringBuilder *result_builder = create_string_builder();
  while (1) {
    char *line = getline_from_file(fifo_reader);
    if (!line) {
      break;
    }
    string_builder_add_string(result_builder, line);
    free(line);
  }
  assert_strings_equal("abc", string_builder_peek(result_builder));
  destroy_string_builder(result_builder);
  destroy_file_handler(fifo_reader);
  test_fifo_args->done_reading = true;
  return NULL;
}

void *write_fifo_thread(void *uncasted_test_fifo_args) {
  TestFIFOArgs *test_fifo_args = (TestFIFOArgs *)uncasted_test_fifo_args;

  const char *fifo_filename = test_fifo_args->fifo_filename;

  FileHandler *fifo_writer =
      create_file_handler_from_filename(fifo_filename, FILE_HANDLER_MODE_WRITE);

  sleep(1);
  write_to_file(fifo_writer, "a\n");
  sleep(1);
  write_to_file(fifo_writer, "b\n");
  sleep(1);
  write_to_file(fifo_writer, "c\n");
  sleep(1);
  destroy_file_handler(fifo_writer);

  test_fifo_args->done_writing = true;

  return NULL;
}

void test_fifo() {
  char *test_fifo_filename = get_test_filename("fifo_test");
  create_fifo(test_fifo_filename);

  TestFIFOArgs *test_fifo_args = create_test_fifo_args(test_fifo_filename);

  pthread_t reader_thread_id;
  pthread_create(&reader_thread_id, NULL, read_fifo_thread, test_fifo_args);
  pthread_detach(reader_thread_id);

  pthread_t writer_thread_id;
  pthread_create(&writer_thread_id, NULL, write_fifo_thread, test_fifo_args);
  pthread_detach(writer_thread_id);

  block_for_fifo_finish(&test_fifo_args->done_reading,
                        &test_fifo_args->done_writing, 8);

  destroy_test_fifo_args(test_fifo_args);
  free(test_fifo_filename);
}

void test_file_handler() {
  // test_regular_file();
  test_fifo();
}
