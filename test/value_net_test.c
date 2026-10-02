#include "value_net_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/endian_io.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/value_net.h"
#include "../src/impl/value_net_metal.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  VALUE_NET_PARITY_ROWS = 64,
  VALUE_NET_PARITY_REPEATS = 20,
};

// Reads count little-endian floats from path.
static float *vnt_read_floats(const char *path, size_t count) {
  FILE *stream = fopen_or_die(path, "rb");
  uint32_t *bits = malloc_or_die(sizeof(uint32_t) * count);
  if (!fread_le_uint32s(bits, count, stream)) {
    log_fatal("could not read %zu floats from %s", count, path);
  }
  (void)fclose(stream);
  float *values = malloc_or_die(sizeof(float) * count);
  memcpy(values, bits, sizeof(float) * count);
  free(bits);
  return values;
}

// Compares a backend's outputs with the parity set's reference outputs and
// prints the largest differences.
static void vnt_report(const char *backend, int rows, const float *value,
                       const float *spread, const float *ref_value,
                       const float *ref_spread, double seconds) {
  double max_value = 0.0;
  double max_spread = 0.0;
  for (int row = 0; row < rows; row++) {
    max_value = fmax(max_value, fabs((double)value[row] - ref_value[row]));
    max_spread = fmax(max_spread, fabs((double)spread[row] - ref_spread[row]));
  }
  printf("value_net_parity backend=%s rows=%d max_abs_value_diff=%.3g "
         "max_abs_spread_diff=%.3g seconds=%.3f\n",
         backend, rows, max_value, max_spread, seconds);
}

// "parity:<dir>:<parity_dir>[:<rows>]": the CPU forward pass on the
// parity set's input rows against its reference outputs.
static void vnt_parity(const char *dir, const char *parity_dir, int rows) {
  ErrorStack *error_stack = error_stack_create();
  ValueNet *net = value_net_create(dir, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not load the value net from %s", dir);
  }
  char *path = get_formatted_string("%s/board.f32", parity_dir);
  float *board = vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS *
                                           VALUE_NET_BOARD_FLOATS);
  free(path);
  path = get_formatted_string("%s/scalars.f32", parity_dir);
  float *scalars =
      vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS * VALUE_NET_SCALARS);
  free(path);
  path = get_formatted_string("%s/value.f32", parity_dir);
  float *ref_value = vnt_read_floats(path, VALUE_NET_PARITY_ROWS);
  free(path);
  path = get_formatted_string("%s/spread.f32", parity_dir);
  float *ref_spread = vnt_read_floats(path, VALUE_NET_PARITY_ROWS);
  free(path);
  float value[VALUE_NET_PARITY_ROWS];
  float spread[VALUE_NET_PARITY_ROWS];
  int64_t start = ctimer_monotonic_ns();
  value_net_evaluate_cpu(net, rows, board, scalars, value, spread);
  vnt_report("cpu", rows, value, spread, ref_value, ref_spread,
             (double)(ctimer_monotonic_ns() - start) / 1e9);
  for (int half = 0; half <= 1; half++) {
    ValueNetMetal *metal = value_net_metal_create(net, half, error_stack);
    if (metal == NULL) {
      error_stack_print_and_reset(error_stack);
      continue;
    }
    // The first run compiles the graph for this batch size.
    start = ctimer_monotonic_ns();
    value_net_metal_evaluate(metal, rows, board, scalars, value, spread);
    const double first = (double)(ctimer_monotonic_ns() - start) / 1e9;
    start = ctimer_monotonic_ns();
    for (int repeat = 0; repeat < VALUE_NET_PARITY_REPEATS; repeat++) {
      value_net_metal_evaluate(metal, rows, board, scalars, value, spread);
    }
    const double seconds = (double)(ctimer_monotonic_ns() - start) / 1e9 /
                           VALUE_NET_PARITY_REPEATS;
    vnt_report(half ? "metal_fp16" : "metal_fp32", rows, value, spread,
               ref_value, ref_spread, seconds);
    printf("value_net_metal first_call_seconds=%.3f\n", first);
    value_net_metal_destroy(metal);
  }
  free(board);
  free(scalars);
  free(ref_value);
  free(ref_spread);
  value_net_destroy(net);
  error_stack_destroy(error_stack);
}

void value_net_test_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  const int num_fields = string_splitter_get_number_of_items(fields);
  const char *mode = string_splitter_get_item(fields, 0);
  if (strings_equal(mode, "parity") && num_fields >= 3) {
    int rows = VALUE_NET_PARITY_ROWS;
    if (num_fields >= 4) {
      rows = (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
    }
    if (rows < 1 || rows > VALUE_NET_PARITY_ROWS) {
      log_fatal("parity rows must be 1..%d", VALUE_NET_PARITY_ROWS);
    }
    vnt_parity(string_splitter_get_item(fields, 1),
               string_splitter_get_item(fields, 2), rows);
  } else {
    log_fatal("unknown valuenet spec: %s", spec);
  }
  string_splitter_destroy(fields);
}
