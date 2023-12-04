#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/rack.h"

int get_word_index_of(const KLV *klv, const Rack *leave, uint32_t node_index) {
  int idx = 0;
  int lidx = 0;
  int lidx_letter_count = get_number_of_letter(leave, lidx);
  int number_of_letters = get_number_of_letters(leave);

  // Advance lidx
  while (lidx_letter_count == 0) {
    lidx++;
    lidx_letter_count = get_number_of_letter(leave, lidx);
  }

  while (node_index != 0) {
    idx += klv_get_word_count(klv, node_index);
    while (kwg_tile(klv_get_kwg(klv), node_index) != (uint8_t)lidx) {
      if (kwg_is_end(klv_get_kwg(klv), node_index)) {
        return -1;
      }
      node_index++;
    }
    idx -= klv_get_word_count(klv, node_index);

    lidx_letter_count--;
    number_of_letters--;

    // Advance lidx
    while (lidx_letter_count == 0) {
      lidx++;
      if (lidx >= get_array_size(leave)) {
        break;
      }
      lidx_letter_count = get_number_of_letter(leave, lidx);
    }

    if (number_of_letters == 0) {
      if (kwg_accepts(klv_get_kwg(klv), node_index)) {
        return idx;
      }
      return -1;
    }
    if (kwg_accepts(klv_get_kwg(klv), node_index)) {
      idx += 1;
    }
    node_index = kwg_arc_index(klv_get_kwg(klv), node_index);
  }
  return -1;
}

double get_leave_value(const KLV *klv, const Rack *leave) {
  if (rack_is_empty(leave)) {
    return 0.0;
  }
  if (!klv) {
    return 0.0;
  }
  int index = get_word_index_of(klv, leave, kwg_arc_index(klv_get_kwg(klv), 0));
  if (index != -1) {
    return (double)klv_get_leave_value(klv, index);
  }
  return 0.0;
}
