#include "../ent/kwg.h"
#include "../ent/rack.h"

int get_word_index_of(const KLV *klv, const Rack *leave, uint32_t node_index) {
  int idx = 0;
  int lidx = 0;
  int lidx_letter_count = leave->array[lidx];
  int number_of_letters = leave->number_of_letters;

  // Advance lidx
  while (lidx_letter_count == 0) {
    lidx++;
    lidx_letter_count = leave->array[lidx];
  }

  while (node_index != 0) {
    idx += klv->word_counts[node_index];
    while (kwg_tile(klv->kwg, node_index) != (uint8_t)lidx) {
      if (kwg_is_end(klv->kwg, node_index)) {
        return -1;
      }
      node_index++;
    }
    idx -= klv->word_counts[node_index];

    lidx_letter_count--;
    number_of_letters--;

    // Advance lidx
    while (lidx_letter_count == 0) {
      lidx++;
      if (lidx >= leave->array_size) {
        break;
      }
      lidx_letter_count = leave->array[lidx];
    }

    if (number_of_letters == 0) {
      if (kwg_accepts(klv->kwg, node_index)) {
        return idx;
      }
      return -1;
    }
    if (kwg_accepts(klv->kwg, node_index)) {
      idx += 1;
    }
    node_index = kwg_arc_index(klv->kwg, node_index);
  }
  return -1;
}

double get_leave_value(const KLV *klv, const Rack *leave) {
  if (leave->empty) {
    return 0.0;
  }
  if (!klv) {
    return 0.0;
  }
  int index = get_word_index_of(klv, leave, kwg_arc_index(klv->kwg, 0));
  if (index != -1) {
    return (double)klv->leave_values[index];
  }
  return 0.0;
}
