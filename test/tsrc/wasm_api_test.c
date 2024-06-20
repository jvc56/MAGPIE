#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/def/game_history_defs.h"

#include "../../src/impl/wasm_api.h"

#include "../../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

const char *cgp1 =
    "4AUREOLED3/11O3/11Z3/10FY3/10A4/10C4/10I4/7THANX3/10GUV2/"
    "15/15/15/15/15/15 AHMPRTU/ 177/44 0 -lex CSW21; -ld english;";

void test_wasm_api() {
  // Attempt to load a malformed cgp
  char *retstr = wasm_score_move("malformed_cgp" VS_ED, "6G.DIPETAZ.ADEIRSZ");
  assert_strings_equal(retstr, "wasm cgp load failed with type 2, code 6");
  free(retstr);

  // play a phony 6G DI(PET)AZ keeping ERS.
  retstr = wasm_score_move(VS_ED, "6G.DIPETAZ.ADEIRSZ");
  // score is 57
  // equity of ERS is 15.497
  // -> total equity is 72.497
  assert_strings_equal(
      retstr, "currmove 6g.DIPETAZ result scored valid false invalid_words "
              "WIFAY,ZGENUINE,DIPETAZ sc 57 eq 72.497");
  free(retstr);

  // Score an exchange keeping AEINR (equity is 12.277)
  retstr = wasm_score_move(VS_ED, "ex.QV.QVAEINR");
  assert_strings_equal(
      retstr, "currmove ex.QV result scored valid true sc 0 eq 12.277");
  free(retstr);

  // Score another play
  // No leave specified so leave equity is 0
  retstr = wasm_score_move(cgp1, "2b.THUMP");
  assert_strings_equal(
      retstr, "currmove 2b.THUMP result scored valid true sc 50 eq 50.000");
  free(retstr);

  wasm_destroy_configs();
}
