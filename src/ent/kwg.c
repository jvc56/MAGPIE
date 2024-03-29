#include "kwg.h"

#if defined(__APPLE__)
#include "../../compat/endian.h"
#else
#include <endian.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#include "../def/kwg_defs.h"

#include "../util/fileproxy.h"
#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

#include "letter_distribution.h"

// The KWG data structure was originally
// developed in wolges. For more details
// on how the KWG data structure works, see
// https://github.com/andy-k/wolges/blob/main/details.txt

char *get_kwg_filepath(const char *kwg_name) {
  // Check for invalid inputs
  if (!kwg_name) {
    log_fatal("kwg name is null");
  }
  return get_formatted_string("%s%s%s", KWG_FILEPATH, kwg_name,
                              KWG_FILE_EXTENSION);
}

void kwg_read_nodes_from_stream(KWG *kwg, size_t number_of_nodes,
                                FILE *stream) {
  kwg_allocate_nodes(kwg, number_of_nodes);                                  
  size_t result = fread(kwg->nodes, sizeof(uint32_t), number_of_nodes, stream);
  if (result != number_of_nodes) {
    log_fatal("kwg nodes fread failure: %zd != %zd", result, number_of_nodes);
  }
  for (uint32_t i = 0; i < number_of_nodes; i++) {
    kwg->nodes[i] = le32toh(kwg->nodes[i]);
  }
  kwg->number_of_nodes = number_of_nodes;
}

void kwg_allocate_nodes(KWG *kwg, size_t number_of_nodes) {
  kwg->nodes = (uint32_t *)malloc_or_die(number_of_nodes * sizeof(uint32_t));
  kwg->number_of_nodes = number_of_nodes;
}

uint32_t *kwg_get_mutable_nodes(KWG *kwg) { return kwg->nodes; }

void load_kwg(KWG *kwg, const char *kwg_name) {
  char *kwg_filename = get_kwg_filepath(kwg_name);

  FILE *stream = stream_from_filename(kwg_filename);
  if (!stream) {
    log_fatal("failed to open stream from filename: %s\n", kwg_filename);
  }
  free(kwg_filename);

  fseek(stream, 0, SEEK_END);         // seek to end of file
  long int kwg_size = ftell(stream);  // get current file pointer
  fseek(stream, 0, SEEK_SET);

  size_t number_of_nodes = kwg_size / sizeof(uint32_t);

  kwg_read_nodes_from_stream(kwg, number_of_nodes, stream);

  fclose(stream);
}

KWG *kwg_create(const char *kwg_name) {
  KWG *kwg = malloc_or_die(sizeof(KWG));
  load_kwg(kwg, kwg_name);
  return kwg;
}

KWG *kwg_create_empty() {
  KWG *kwg = malloc_or_die(sizeof(KWG));
  return kwg;
}

void kwg_destroy(KWG *kwg) {
  if (!kwg) {
    return;
  }
  free(kwg->nodes);
  free(kwg);
}

bool kwg_in_letter_set(const KWG *kwg, uint8_t letter, uint32_t node_index) {
  letter = get_unblanked_machine_letter(letter);
  uint32_t i = node_index;
  for (;;) {
    const uint32_t node = kwg_node(kwg, i);
    if (kwg_node_tile(node) == letter) {
      return kwg_node_accepts(node);
    }
    if (kwg_node_is_end(node)) {
      return false;
    }
    i++;
  }
}

int kwg_get_number_of_nodes(const KWG *kwg) { return kwg->number_of_nodes; }
