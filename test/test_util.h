#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include "../src/ent/anchor.h"
#include "../src/ent/bag.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/board.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include <stdbool.h>

#define TRIVIAL_CROSS_SET_STRING "*"
#define DEFAULT_TEST_DATA_PATH "./testdata:./data"

typedef struct SortedMoveList {
  int count;
  Move **moves;
} SortedMoveList;

uint64_t string_to_cross_set(const LetterDistribution *ld, const char *letters);
char *cross_set_to_string(const LetterDistribution *ld, uint64_t input);
void play_top_n_equity_move(Game *game, int n);
SortedMoveList *sorted_move_list_create(MoveList *ml);
void sorted_move_list_destroy(SortedMoveList *sorted_move_list);
void print_move_list(const Board *board, const LetterDistribution *ld,
                     const SortedMoveList *sml, int move_list_length);
void sort_and_print_move_list(const Board *board, const LetterDistribution *ld,
                              MoveList *ml);
void resort_sorted_move_list_by_score(SortedMoveList *sml);
void assert_equal_at_equity_resolution(double a, double b);
bool within_epsilon(double a, double b);
int count_newlines(const char *str);
bool equal_rack(const Rack *expected_rack, const Rack *actual_rack);
void assert_strings_equal(const char *str1, const char *str2);
void assert_racks_equal(const LetterDistribution *ld, const Rack *r1,
                        const Rack *r2);
void assert_rack_equals_string(const LetterDistribution *ld, const Rack *r1,
                               const char *r2_str);
void assert_move(const Game *game, const MoveList *move_list,
                 const SortedMoveList *sml, int move_index,
                 const char *expected_move_string);
void assert_bags_are_equal(const Bag *b1, const Bag *b2, int rack_array_size);
void assert_boards_are_equal(Board *b1, Board *b2);
void assert_games_are_equal(const Game *g1, const Game *g2, bool check_scores);
void print_game(const Game *game, const MoveList *move_list);
void print_cgp(const Game *game);
void print_english_rack(const Rack *rack);
void print_rack(const Rack *rack, const LetterDistribution *ld);
void print_inference(const LetterDistribution *ld,
                     const Rack *target_played_tiles,
                     InferenceResults *inference_results);
void set_thread_control_status_to_start(ThreadControl *thread_control);
void load_cgp_or_die(Game *game, const char *cgp);
void load_and_exec_config_or_die(Config *config, const char *cmd);
bool load_and_exec_config_or_die_timed(Config *config, const char *cmd,
                                       int seconds);
char *get_test_filename(const char *filename);
void delete_file(const char *filename);
void delete_fifo(const char *fifo_name);
Config *config_create_or_die(const char *cmd);
Config *config_create_default_test(void);
WMP *wmp_create_or_die(const char *data_paths, const char *wmp_name);
KLV *klv_create_or_die(const char *data_paths, const char *klv_name);
void klv_write_or_die(const KLV *klv, const char *data_paths,
                      const char *klv_name);
KLV *klv_read_from_csv_or_die(const LetterDistribution *ld,
                              const char *data_paths, const char *leaves_name);
void klv_write_to_csv_or_die(KLV *klv, const LetterDistribution *ld,
                             const char *data_paths, const char *csv_name);
char *get_string_from_file_or_die(const char *filename);
void set_row(const Game *game, int row, const char *row_content);
void assert_board_layout_error(const char *data_paths,
                               const char *board_layout_name,
                               error_code_t expected_status);
void load_game_with_test_board(Game *game, const char *data_paths,
                               const char *board_layout_name);
ValidatedMoves *validated_moves_create_and_assert_status(
    const Game *game, int player_index, const char *ucgi_moves_string,
    bool allow_phonies, bool allow_unknown_exchanges, bool allow_playthrough,
    error_code_t expected_status);
error_code_t config_simulate_and_return_status(const Config *config,
                                               Rack *known_opp_rack,
                                               SimResults *sim_results);
void game_play_to_turn_or_die(GameHistory *game_history, Game *game,
                              int turn_index);
void game_play_to_end_or_die(GameHistory *game_history, Game *game);
void assert_validated_and_generated_moves(Game *game, const char *rack_string,
                                          const char *move_position,
                                          const char *move_tiles,
                                          int move_score,
                                          bool play_move_on_board);
ValidatedMoves *assert_validated_move_success(Game *game, const char *cgp_str,
                                              const char *move_str,
                                              int player_index,
                                              bool allow_phonies,
                                              bool allow_playthrough);
void assert_game_matches_cgp(const Game *game, const char *expected_cgp,
                             bool write_player_on_turn_first);
void assert_stats_are_equal(const Stat *s1, const Stat *s2);
void assert_simmed_plays_stats_are_equal(const SimmedPlay *sp1,
                                         const SimmedPlay *sp2, int max_plies);
void assert_sim_results_equal(SimResults *sr1, SimResults *sr2);
void assert_klvs_equal(const KLV *klv1, const KLV *klv2);
void assert_word_count(const LetterDistribution *ld,
                       const DictionaryWordList *words,
                       const char *human_readable_word, int expected_count);

BitRack string_to_bit_rack(const LetterDistribution *ld,
                           const char *rack_string);

void assert_word_in_buffer(const MachineLetter *buffer,
                           const char *expected_word,
                           const LetterDistribution *ld, int word_idx,
                           int length);

void assert_move_score(const Move *move, int expected_score);
void assert_move_equity_int(const Move *move, int expected_equity);
void assert_move_equity_exact(const Move *move, Equity expected_equity);
void assert_rack_score(const LetterDistribution *ld, const Rack *rack,
                       int expected_score);
void assert_validated_moves_challenge_points(const ValidatedMoves *vms, int i,
                                             int expected_challenge_points);
void assert_anchor_equity_int(const AnchorHeap *ah, int i, int expected);
void assert_anchor_equity_exact(const AnchorHeap *ah, int i, Equity expected);
void generate_anchors_for_test(Game *game);
void extract_sorted_anchors_for_test(AnchorHeap *sorted_anchors);
void set_klv_leave_value(const KLV *klv, const LetterDistribution *ld,
                         const char *rack_str, Equity equity);
#endif
