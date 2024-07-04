#include "../../src/ent/klv.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/leave_list.h"

#include "../../src/impl/config.h"

#include "test_util.h"

void test_leave_list(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const KLV *klv = players_data_get_data(config_get_players_data(config),
                                         PLAYERS_DATA_TYPE_KLV, 0);
  LeaveList *leave_list =
      leave_list_create(kwg_get_number_of_nodes(klv_get_kwg(klv)));
  leave_list_destroy(leave_list);
  config_destroy(config);
}