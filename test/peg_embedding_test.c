#include "peg_embedding_test.h"

#include "../src/impl/cmd_api.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>

void test_peg_embedding(void) {
  Magpie *mp = magpie_create(DEFAULT_TEST_DATA_PATH);
  assert(!magpie_has_error(mp));
  assert(magpie_run_sync(mp, "set -lex CSW24 -threads 1 -pegtlim 0.1") ==
         MAGPIE_SUCCESS);
  assert(
      magpie_run_sync(
          mp,
          "cgp "
          "4WILI3JANE/4A3AQUAE2/2C1N3K2Y3/2RUG3E2G3/2O1L3B1REV2/2U1EL2I1EEL2/"
          "2T1SI2A1S1Y2/R1O2PWNS1T4/E1N2I4A4/AH1MED4T4/GI1OPE4E4/IT1ZO2VOUDON2/"
          "N3D10/"
          "I3E10/COIFS10 BDFHMX?/AAORTT? 420/412 0 -lex CSW24") ==
      MAGPIE_SUCCESS);
  // The command-line protocol stays text-only until an embedder opts in.
  for (int pass = 0; pass < 3; pass++) {
    if (pass > 0) {
      magpie_set_peg_json(mp, pass == 1);
    }
    assert(magpie_run_sync(mp, "peg") == MAGPIE_SUCCESS);
    char *output = magpie_get_last_command_output(mp);
    assert(has_substring(output, "peginfo {") == (pass == 1));
    free(output);
    assert(magpie_run_sync(mp, "shpeg") == MAGPIE_SUCCESS);
    output = magpie_get_last_command_output(mp);
    assert(has_substring(output, "peginfo {") == (pass == 1));
    free(output);
  }
  magpie_destroy(mp);
}
