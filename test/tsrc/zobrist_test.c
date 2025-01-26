
#include "../../src/impl/config.h"
#include <assert.h>

#include "../../src/def/validated_move_defs.h"
#include "../../src/ent/game.h"
#include "../../src/ent/validated_move.h"
#include "../../src/ent/zobrist.h"
#include "../../src/impl/gameplay.h"
#include "test_util.h"
#include "zobrist_test.h"

// void test_hash_after_making_play(void) {
//   Config *config = config_create_or_die("set -s1 score -s2 score");
//   load_and_exec_config_or_die(
//       config, "cgp "
//               "14C/13QI/12FIE/10VEE1R/9KIT2G/8CIG1IDE/8UTA2AS/7ST1SYPh1/6JA5A1/"
//               "5WOLD2BOBA/3PLOT1R1NU1EX/Y1VEIN1NOR1mOA1/UT1AT1N1L2FEH1/"
//               "GUR2WIRER5/SNEEZED8 ADENOOO/AHIILMM 353/236 0 -lex CSW21;");
//   Game *game = config_get_game(config);
//   Zobrist *z = zobrist_create(42);
//   uint64_t h = zobrist_calculate_hash(
//       z, game_get_board(game), player_get_rack(game_get_player(game, 0)),
//       player_get_rack(game_get_player(game, 1)), false, 0);

//   // play a move on the board.
//   ValidatedMoves *vms =
//       validated_moves_create(game, 0, "15J.END", false, false, false);
//   assert(validated_moves_get_validation_status(vms) ==
//          MOVE_VALIDATION_STATUS_SUCCESS);

//   const LetterDistribution *ld = game_get_ld(game);
//   uint32_t ld_size = ld_get_size(ld);

//   Rack *rack = rack_create(ld_size);
//   rack_set_to_string(
//       ld, rack,
//       "AOOO"); // rack always contains the post-play rack, when calling add_move

//   uint64_t h1 = zobrist_add_move(z, h, validated_moves_get_move(vms, 0), rack,
//                                  true, 0, 0);

//   // actually play the move:
//   play_move_status_t play_status =
//       play_move(validated_moves_get_move(vms, 0), game, NULL, NULL);
//   assert(play_status == PLAY_MOVE_STATUS_SUCCESS);

//   uint64_t h2 = zobrist_calculate_hash(
//       z, game_get_board(game), player_get_rack(game_get_player(game, 0)),
//       player_get_rack(game_get_player(game, 1)), true, 0);

//   assert(h1 == h2);

//   validated_moves_destroy(vms);
//   rack_destroy(rack);
//   zobrist_destroy(z);
//   config_destroy(config);
// }

// void test_hash_after_making_blank_play(void) {
//   Config *config = config_create_or_die("set -s1 score -s2 score");
//   load_and_exec_config_or_die(
//       config, "cgp "
//               "IBADAT1B7/2CAFE1OD1TRANQ/2TUT2RENIED2/3REV2YOMIM2/4RAFT1NISI2/"
//               "5COR2N1x2/6LA1AGEE2/6LIAISED2/5POKY2W3/4JOWS7/V2LUZ9/ORPIN10/"
//               "L1OE11/TUX12/I14 EEEEGH?/AGHNOSU 308/265 0 -lex CSW21;");

//   Game *game = config_get_game(config);
//   Zobrist *z = zobrist_create(42);
//   uint64_t h = zobrist_calculate_hash(
//       z, game_get_board(game), player_get_rack(game_get_player(game, 0)),
//       player_get_rack(game_get_player(game, 1)), false, 0);

//   // play 6m (x)u
//   ValidatedMoves *vms =
//       validated_moves_create(game, 0, "6m.xu", false, false, false);
//   assert(validated_moves_get_validation_status(vms) ==
//          MOVE_VALIDATION_STATUS_SUCCESS);

//   const LetterDistribution *ld = game_get_ld(game);
//   uint32_t ld_size = ld_get_size(ld);

//   Rack *rack = rack_create(ld_size);
//   rack_set_to_string(ld, rack,
//                      "EEEEGH"); // rack always contains the post-play rack, when
//                                 // calling add_move

//   uint64_t h1 = zobrist_add_move(z, h, validated_moves_get_move(vms, 0), rack,
//                                  true, 0, 0);

//   // actually play the move:
//   play_move_status_t play_status =
//       play_move(validated_moves_get_move(vms, 0), game, NULL, NULL);
//   assert(play_status == PLAY_MOVE_STATUS_SUCCESS);

//   uint64_t h2 = zobrist_calculate_hash(
//       z, game_get_board(game), player_get_rack(game_get_player(game, 0)),
//       player_get_rack(game_get_player(game, 1)), true, 0);

//   assert(h1 == h2);

//   validated_moves_destroy(vms);
//   rack_destroy(rack);
//   zobrist_destroy(z);
//   config_destroy(config);
// }

// void test_hash_after_passing(void) {
//   Config *config = config_create_or_die("set -s1 score -s2 score");
//   load_and_exec_config_or_die(
//       config, "cgp "
//               "14C/13QI/12FIE/10VEE1R/9KIT2G/8CIG1IDE/8UTA2AS/7ST1SYPh1/6JA5A1/"
//               "5WOLD2BOBA/3PLOT1R1NU1EX/Y1VEIN1NOR1mOA1/UT1AT1N1L2FEH1/"
//               "GUR2WIRER5/SNEEZED8 ADENOOO/AHIILMM 353/236 0 -lex CSW21;");
//   Game *game = config_get_game(config);
//   Zobrist *z = zobrist_create(42);
//   uint64_t h = zobrist_calculate_hash(
//       z, game_get_board(game), player_get_rack(game_get_player(game, 0)),
//       player_get_rack(game_get_player(game, 1)), false, 0);

//   // play a move on the board.
//   ValidatedMoves *vms =
//       validated_moves_create(game, 0, "pass", false, false, false);
//   assert(validated_moves_get_validation_status(vms) ==
//          MOVE_VALIDATION_STATUS_SUCCESS);

//   const LetterDistribution *ld = game_get_ld(game);
//   uint32_t ld_size = ld_get_size(ld);

//   Rack *rack = rack_create(ld_size);
//   rack_set_to_string(ld, rack, "ADENOOO");

//   uint64_t h1 = zobrist_add_move(z, h, validated_moves_get_move(vms, 0), rack,
//                                  true, 1, 0);

//   // actually play the move:
//   play_move_status_t play_status =
//       play_move(validated_moves_get_move(vms, 0), game, NULL, NULL);
//   assert(play_status == PLAY_MOVE_STATUS_SUCCESS);

//   uint64_t h2 = zobrist_calculate_hash(
//       z, game_get_board(game), player_get_rack(game_get_player(game, 0)),
//       player_get_rack(game_get_player(game, 1)), true, 1);

//   assert(h1 == h2);

//   // play another pass
//   validated_moves_destroy(vms);
//   vms = validated_moves_create(game, 1, "pass", false, false, false);

//   rack_set_to_string(ld, rack, "AHIILMM");

//   uint64_t h3 = zobrist_add_move(z, h2, validated_moves_get_move(vms, 0), rack,
//                                  false, 2, 1);
//   play_status = play_move(validated_moves_get_move(vms, 0), game, NULL, NULL);

//   // should not be equal to the very first hash
//   assert(h != h3);

//   validated_moves_destroy(vms);
//   rack_destroy(rack);
//   zobrist_destroy(z);
//   config_destroy(config);
// }

void test_zobrist(void) {
//   test_hash_after_making_play();
//   test_hash_after_making_blank_play();
//   test_hash_after_passing();
}