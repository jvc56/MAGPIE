#include "../src/ort.h"

#include <assert.h>

#include "../src/board.h"
#include "superconfig.h"

void test_ort(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  ORT *ort = config->player_1_strategy_params->ort;
  LetterDistribution *ld = config->letter_distribution;
  Rack *rack = create_rack(ld->size);
  uint8_t word_sizes[BOARD_DIM + 1];

  set_rack_to_string(rack, "BFHHIO?", ld);
  get_word_sizes(ort, rack, ld, word_sizes);
  assert(word_sizes[0] == 5);  // BOdHI
  assert(word_sizes[1] == 6);  // HOB(B)IsH
  assert(word_sizes[2] == 7);  //
  assert(word_sizes[3] == 7);  //
  assert(word_sizes[4] == 6);  //
  assert(word_sizes[5] == 6);  //
  for (int i = 6; i <= BOARD_DIM; i++) {
    assert(word_sizes[i] == rack->number_of_letters);
  }
}