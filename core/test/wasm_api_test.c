#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/game.h"
#include "../src/wasm_api.h"
#include "superconfig.h"
#include "test_constants.h"

void test_wasm_api() {
  // play a phony 6G DI(PET)AZ keeping ERS.
  char *retstr = score_play(VS_ED, MOVE_TYPE_PLAY, 5, 6, 0,
                            (uint8_t[]){4, 9, 0, 0, 0, 1, 26},
                            (uint8_t[]){5, 18, 19}, 7, 3);

  // score is 57
  // equity of ERS is 15.947
  // -> total equity is 72.947
  assert(strcmp(retstr,
                "currmove 6g.DIPETAZ result scored valid false invalid_words "
                "WIFAY,ZGENUINE,DIPETAZ sc 57 eq 72.947") == 0);

  free(retstr);

  // Score an exchange keeping AEINR (equity is 12.610)
  retstr = score_play(VS_ED, MOVE_TYPE_EXCHANGE, 0, 0, 0, (uint8_t[]){17, 23},
                      (uint8_t[]){1, 5, 9, 14, 18}, 2, 5);
  assert(strcmp(retstr,
                "currmove ex.QW result scored valid true sc 0 eq 12.610") == 0);

  free(retstr);
}
