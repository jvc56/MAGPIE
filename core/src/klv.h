#ifndef KLV_H
#define KLV_H

#include <stdint.h>

#include "kwg.h"
#include "rack.h"

typedef struct KLV {
  KWG *kwg;
  int *word_counts;
  float *leave_values;
} KLV;

KLV *create_klv(const char *klv_filename);
void destroy_klv(KLV *klv);
double get_leave_value(KLV *klv, Rack *rack);
int32_t increment_node_to_ml(KLV *klv, int32_t node_index, int32_t word_index,
                             int *next_word_index, uint8_t ml);
int32_t follow_arc(KLV *klv, int32_t node_index, int32_t word_index,
                   int32_t *next_word_index);

#endif