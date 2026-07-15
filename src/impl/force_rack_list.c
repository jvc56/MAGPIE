#include "force_rack_list.h"

#include "../ent/encoded_rack.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  FORCE_RACK_LIST_MAX_LINE_LENGTH = 256,
  FORCE_RACK_LIST_INITIAL_CAPACITY = 64,
};

struct ForceRackList {
  EncodedRack *racks;
  int num_racks;
};

ForceRackList *force_rack_list_create(const LetterDistribution *ld,
                                      const char *filename,
                                      ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "r", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  ForceRackList *force_rack_list = malloc_or_die(sizeof(ForceRackList));
  int capacity = FORCE_RACK_LIST_INITIAL_CAPACITY;
  force_rack_list->racks = malloc_or_die(sizeof(EncodedRack) * capacity);
  force_rack_list->num_racks = 0;

  Rack rack;
  rack_set_dist_size(&rack, ld_get_size(ld));
  char line[FORCE_RACK_LIST_MAX_LINE_LENGTH];
  while (fgets(line, sizeof(line), stream)) {
    trim_whitespace(line);
    if (is_string_empty_or_whitespace(line)) {
      continue;
    }
    const int num_letters = rack_set_to_string(ld, &rack, line);
    if (num_letters <= 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_AUTOPLAY_FORCE_RACKS_MALFORMED_RACK,
          get_formatted_string("invalid rack in force racks file '%s': %s",
                               filename, line));
      break;
    }
    if (force_rack_list->num_racks == capacity) {
      capacity *= 2;
      force_rack_list->racks = realloc_or_die(
          force_rack_list->racks, sizeof(EncodedRack) * (size_t)capacity);
    }
    rack_encode(&rack, &force_rack_list->racks[force_rack_list->num_racks]);
    force_rack_list->num_racks++;
  }
  fclose_or_die(stream);

  if (!error_stack_is_empty(error_stack)) {
    force_rack_list_destroy(force_rack_list);
    return NULL;
  }

  if (force_rack_list->num_racks == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_AUTOPLAY_FORCE_RACKS_FILE_EMPTY,
        get_formatted_string("force racks file '%s' does not contain any racks",
                             filename));
    force_rack_list_destroy(force_rack_list);
    return NULL;
  }

  return force_rack_list;
}

void force_rack_list_destroy(ForceRackList *force_rack_list) {
  if (!force_rack_list) {
    return;
  }
  free(force_rack_list->racks);
  free(force_rack_list);
}

void force_rack_list_get_random_rack(const ForceRackList *force_rack_list,
                                     XoshiroPRNG *prng, Rack *rack_out) {
  const uint64_t random_index =
      prng_get_random_number(prng, (uint64_t)force_rack_list->num_racks);
  rack_decode(&force_rack_list->racks[random_index], rack_out);
}
