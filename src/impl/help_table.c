#include "help_table.h"

#include "../def/config_defs.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Options are global and any command accepts any of them, so nothing in the
// parser says which command reads which option. This table records it, from
// the config_fill_*_args functions and the impl_* commands in config.c.

#define HELP_ARRAY_LENGTH(array) ((int)(sizeof(array) / sizeof((array)[0])))
#define HELP_NO_TOKEN NUMBER_OF_ARG_TOKENS
#define HELP_ROW(token) {ARG_TOKEN_##token, HELP_NO_TOKEN, HELP_NO_TOKEN}
#define HELP_PLAYER_ROW(both_players, player)                                  \
  {ARG_TOKEN_##both_players, ARG_TOKEN_P1_##player, ARG_TOKEN_P2_##player}
#define HELP_PLAYERS_ONLY_ROW(token)                                           \
  {HELP_NO_TOKEN, ARG_TOKEN_P1_##token, ARG_TOKEN_P2_##token}
#define HELP_GROUP(title, help_subject, is_shared, rows)                       \
  {title, help_subject, is_shared, rows, HELP_ARRAY_LENGTH(rows)}
#define HELP_SECTION(title, commands)                                          \
  {title, commands, HELP_ARRAY_LENGTH(commands)}
#define HELP_GROUP_BIT(group) (1U << (group))

static const HelpOptionRow setup_rows[] = {
    HELP_PLAYER_ROW(LEXICON, LEXICON),
    HELP_PLAYER_ROW(LEAVES, LEAVES),
    HELP_ROW(LETTER_DISTRIBUTION),
    HELP_ROW(BOARD_LAYOUT),
    HELP_ROW(GAME_VARIANT),
    HELP_ROW(BINGO_BONUS),
    HELP_ROW(CHALLENGE_BONUS),
};

static const HelpOptionRow tables_rows[] = {
    HELP_PLAYER_ROW(USE_WMP, USE_WMP),
    HELP_PLAYER_ROW(USE_RIT, USE_RIT),
    HELP_PLAYER_ROW(USE_WIT, USE_WIT),
    HELP_ROW(USE_MMAP_FOR_RIT),
};

static const HelpOptionRow movegen_rows[] = {
    HELP_PLAYER_ROW(NUMBER_OF_PLAYS, NUM_PLAYS),
    HELP_PLAYERS_ONLY_ROW(MOVE_SORT_TYPE),
    HELP_PLAYERS_ONLY_ROW(MOVE_RECORD_TYPE),
    HELP_ROW(MOVEGEN_MARGIN),
    HELP_ROW(USE_SMALL_PLAYS),
    HELP_ROW(NUMBER_OF_SMALL_PLAYS),
};

static const HelpOptionRow sim_rows[] = {
    HELP_PLAYER_ROW(PLIES, SIM_PLIES),
    HELP_PLAYER_ROW(MAX_ITERATIONS, MAX_ITERATIONS),
    HELP_PLAYER_ROW(MIN_PLAY_ITERATIONS, MIN_PLAY_ITERATIONS),
    HELP_PLAYER_ROW(STOP_COND_PCT, STOP_COND_PCT),
    HELP_PLAYER_ROW(TIME_LIMIT, TIME_LIMIT),
    HELP_PLAYER_ROW(MOVE_BUDGET, MOVE_BUDGET),
    HELP_PLAYER_ROW(THRESHOLD, THRESHOLD),
    HELP_PLAYER_ROW(SAMPLING_RULE, SAMPLING_RULE),
    HELP_PLAYER_ROW(SIM_WITH_INFERENCE, SIM_WITH_INFERENCE),
    HELP_PLAYER_ROW(SIM_MARGIN_FORECAST, SIM_MARGIN_FORECAST),
    HELP_PLAYER_ROW(SIM_BAG_CYCLE, SIM_BAG_CYCLE),
    HELP_PLAYER_ROW(UTILITY_W_WINPCT, UTILITY_W_WINPCT),
    HELP_PLAYER_ROW(UTILITY_W_SPREAD, UTILITY_W_SPREAD),
    HELP_PLAYER_ROW(UTILITY_SPREAD_SCALE, UTILITY_SPREAD_SCALE),
    HELP_ROW(CUTOFF),
    HELP_ROW(WIN_PCT),
    HELP_ROW(USE_HEAT_MAP),
};

static const HelpOptionRow inference_rows[] = {
    HELP_PLAYER_ROW(INFERENCE_MARGIN, INFERENCE_MARGIN),
};

static const HelpOptionRow endgame_rows[] = {
    HELP_PLAYER_ROW(ENDGAME_PLIES, ENDGAME_PLIES),
    HELP_ROW(ENDGAME_TIME_LIMIT),
    HELP_ROW(ENDGAME_TOP_K),
};

static const HelpOptionRow solver_rows[] = {
    HELP_ROW(ENDGAME_BEST_REPLY),
    HELP_ROW(TT_FRACTION_OF_MEM),
};

static const HelpOptionRow peg_rows[] = {
    HELP_ROW(PEG_TIME_LIMIT),
    HELP_ROW(PEG_PLIES),
    HELP_PLAYER_ROW(PEG_TOP_K, PEG_TOP_K),
    HELP_PLAYER_ROW(PEG_STRIDE, PEG_STRIDE),
    HELP_PLAYER_ROW(PEG_PESSIMISTIC, PEG_PESSIMISTIC),
    HELP_PLAYER_ROW(PEG_NESTED, PEG_NESTED),
    HELP_ROW(PEG_NESTED_CAND_CAPS),
    HELP_ROW(PEG_NESTED_DEPTH),
    HELP_ROW(PEG_NESTED_STRIDES),
    HELP_PLAYERS_ONLY_ROW(PEG_MAX_BAG),
    HELP_ROW(PEG_OUTCOMES),
    HELP_ROW(PEG_JSON),
    HELP_ROW(PEG_OUT_LINES),
    HELP_ROW(PEG_OUT_WIDTH),
    HELP_ROW(PEG_NOPRUNE),
};

static const HelpOptionRow pat_rows[] = {
    HELP_PLAYER_ROW(PAT, PAT),
    HELP_PLAYER_ROW(PAT_CLASSES, PAT_CLASSES),
    HELP_PLAYER_ROW(PAT_CANDIDATES, PAT_CANDIDATES),
    HELP_PLAYER_ROW(PAT_ROLLOUT, PAT_ROLLOUT),
    HELP_PLAYER_ROW(PAT_ROLLOUT_CLASSES, PAT_ROLLOUT_CLASSES),
    HELP_ROW(PAT_LABEL_PLIES),
};

static const HelpOptionRow analyze_rows[] = {
    HELP_ROW(ANALYZE_PEG_MAX_BAG),
    HELP_ROW(ANALYZE_ENDGAME_SPREAD),
    HELP_ROW(SHOW_MISTAKES),
};

static const HelpOptionRow autoplay_rows[] = {
    HELP_ROW(USE_GAME_PAIRS),
    HELP_ROW(MULTI_THREADING_MODE),
    HELP_PLAYERS_ONLY_ROW(PLAY_CHOOSER_TIME),
    HELP_ROW(OVERTIME_PENALTY_POINTS),
    HELP_ROW(OVERTIME_PERIOD),
    HELP_ROW(WRITE_BUFFER_SIZE),
    HELP_ROW(PRINT_BOARDS),
};

static const HelpOptionRow display_rows[] = {
    HELP_ROW(MAX_NUMBER_OF_DISPLAY_PLAYS),
    HELP_ROW(SHPLIES),
    HELP_ROW(SHOW_BU),
    HELP_ROW(SHOW_GAME_WITH_MOVES),
    HELP_ROW(HUMAN_READABLE),
    HELP_ROW(PRETTY),
    HELP_ROW(BOARD_COLOR),
    HELP_ROW(BOARD_TILE_GLYPHS),
    HELP_ROW(BOARD_BORDER),
    HELP_ROW(BOARD_COLUMN_LABEL),
    HELP_ROW(ON_TURN_MARKER),
    HELP_ROW(ON_TURN_COLOR),
    HELP_ROW(ON_TURN_SCORE_STYLE),
};

static const HelpOptionRow session_rows[] = {
    HELP_ROW(NUMBER_OF_THREADS), HELP_ROW(RANDOM_SEED),
    HELP_ROW(MOVEGEN_LIMIT),     HELP_ROW(DATA_PATH),
    HELP_ROW(PRINT_INTERVAL),    HELP_ROW(PRINT_ON_FINISH),
    HELP_ROW(EXEC_MODE),         HELP_ROW(SHOW_PROMPT),
    HELP_ROW(AUTOSAVE_GCG),      HELP_ROW(FG_REQUIRED),
    HELP_ROW(SAVE_SETTINGS),     HELP_ROW(SETTINGS_VERSION),
};

static const HelpOptionGroup option_groups[NUMBER_OF_HELP_OPTION_GROUPS] = {
    [HELP_OPTIONS_SETUP] =
        HELP_GROUP("Game setup options", "setup", true, setup_rows),
    [HELP_OPTIONS_TABLES] =
        HELP_GROUP("Lookup table options", "tables", true, tables_rows),
    [HELP_OPTIONS_MOVEGEN] =
        HELP_GROUP("Move generation options", "generate", false, movegen_rows),
    [HELP_OPTIONS_SIM] =
        HELP_GROUP("Simulation options", "simulate", false, sim_rows),
    [HELP_OPTIONS_INFERENCE] =
        HELP_GROUP("Inference options", "infer", false, inference_rows),
    [HELP_OPTIONS_ENDGAME] =
        HELP_GROUP("Endgame options", "endgame", false, endgame_rows),
    [HELP_OPTIONS_SOLVER] =
        HELP_GROUP("Solver options", "endgame", false, solver_rows),
    [HELP_OPTIONS_PEG] =
        HELP_GROUP("Pre-endgame options", "peg", false, peg_rows),
    [HELP_OPTIONS_PAT] = HELP_GROUP("PAT options", "patgen", false, pat_rows),
    [HELP_OPTIONS_ANALYZE] =
        HELP_GROUP("Analyze options", "analyze", false, analyze_rows),
    [HELP_OPTIONS_AUTOPLAY] =
        HELP_GROUP("Autoplay options", "autoplay", false, autoplay_rows),
    [HELP_OPTIONS_DISPLAY] =
        HELP_GROUP("Display options", "display", true, display_rows),
    [HELP_OPTIONS_SESSION] =
        HELP_GROUP("Session options", "session", true, session_rows),
};

static const arg_token_t setup_commands[] = {
    ARG_TOKEN_NEW_GAME,     ARG_TOKEN_LOAD,    ARG_TOKEN_CGP,
    ARG_TOKEN_EXPORT,       ARG_TOKEN_P1_NAME, ARG_TOKEN_P2_NAME,
    ARG_TOKEN_SWITCH_NAMES,
};

static const arg_token_t play_commands[] = {
    ARG_TOKEN_RACK,     ARG_TOKEN_RANDOM_RACK, ARG_TOKEN_MOVES,
    ARG_TOKEN_COMMIT,   ARG_TOKEN_TOP_COMMIT,  ARG_TOKEN_CNOTE,
    ARG_TOKEN_NOTE,     ARG_TOKEN_CHALLENGE,   ARG_TOKEN_UNCHALLENGE,
    ARG_TOKEN_OVERTIME, ARG_TOKEN_GOTO,        ARG_TOKEN_NEXT,
    ARG_TOKEN_PREVIOUS,
};

static const arg_token_t analyze_commands[] = {
    ARG_TOKEN_GEN,
    ARG_TOKEN_RACK_AND_GEN,
    ARG_TOKEN_SIM,
    ARG_TOKEN_GEN_AND_SIM,
    ARG_TOKEN_RACK_AND_GEN_AND_SIM,
    ARG_TOKEN_SNOPRUNE,
    ARG_TOKEN_INFER,
    ARG_TOKEN_ENDGAME,
    ARG_TOKEN_PEG,
    ARG_TOKEN_ANALYZE,
    ARG_TOKEN_SHOW_GAME,
    ARG_TOKEN_SHOW_MOVES,
    ARG_TOKEN_SHOW_INFERENCE,
    ARG_TOKEN_SHOW_ENDGAME,
    ARG_TOKEN_SHOW_PEG,
    ARG_TOKEN_SHOW_HEAT_MAP,
};

static const arg_token_t batch_commands[] = {
    ARG_TOKEN_AUTOPLAY,
    ARG_TOKEN_LEAVE_GEN,
    ARG_TOKEN_PAT_GEN,
    ARG_TOKEN_CONTRIBUTE,
};

static const arg_token_t program_commands[] = {
    ARG_TOKEN_SET,         ARG_TOKEN_DATA,     ARG_TOKEN_CONVERT,
    ARG_TOKEN_CREATE_DATA, ARG_TOKEN_BUILDERS, ARG_TOKEN_VERSION,
    ARG_TOKEN_HELP,
};

static const HelpCommandSection command_sections[] = {
    HELP_SECTION("Set up a game", setup_commands),
    HELP_SECTION("Play and annotate", play_commands),
    HELP_SECTION("Analyze a position", analyze_commands),
    HELP_SECTION("Run batches", batch_commands),
    HELP_SECTION("Data and program", program_commands),
};

// What the help for a command shows of each group: its options, or a pointer
// to where they are.
typedef struct HelpCommandGroups {
  arg_token_t command;
  uint32_t listed_groups;
  uint32_t pointed_to_groups;
} HelpCommandGroups;

enum {
  HELP_SETUP_BIT = HELP_GROUP_BIT(HELP_OPTIONS_SETUP),
  HELP_TABLES_BIT = HELP_GROUP_BIT(HELP_OPTIONS_TABLES),
  HELP_MOVEGEN_BIT = HELP_GROUP_BIT(HELP_OPTIONS_MOVEGEN),
  HELP_SIM_BIT = HELP_GROUP_BIT(HELP_OPTIONS_SIM),
  HELP_INFERENCE_BIT = HELP_GROUP_BIT(HELP_OPTIONS_INFERENCE),
  HELP_ENDGAME_BIT = HELP_GROUP_BIT(HELP_OPTIONS_ENDGAME),
  HELP_SOLVER_BIT = HELP_GROUP_BIT(HELP_OPTIONS_SOLVER),
  HELP_PEG_BIT = HELP_GROUP_BIT(HELP_OPTIONS_PEG),
  HELP_PAT_BIT = HELP_GROUP_BIT(HELP_OPTIONS_PAT),
  HELP_ANALYZE_BIT = HELP_GROUP_BIT(HELP_OPTIONS_ANALYZE),
  HELP_AUTOPLAY_BIT = HELP_GROUP_BIT(HELP_OPTIONS_AUTOPLAY),
  HELP_DISPLAY_BIT = HELP_GROUP_BIT(HELP_OPTIONS_DISPLAY),
  HELP_SESSION_BIT = HELP_GROUP_BIT(HELP_OPTIONS_SESSION),
  // What every command that reads a position and finds plays works from.
  HELP_POSITION_BITS =
      HELP_SETUP_BIT | HELP_TABLES_BIT | HELP_DISPLAY_BIT | HELP_SESSION_BIT,
  // A simulation's candidates come from move generation, and it can infer.
  HELP_SIM_POINTER_BITS =
      HELP_MOVEGEN_BIT | HELP_INFERENCE_BIT | HELP_PAT_BIT | HELP_POSITION_BITS,
  // Every player in a batch can generate, simulate, infer and solve.
  HELP_BATCH_POINTER_BITS = HELP_MOVEGEN_BIT | HELP_SIM_BIT |
                            HELP_INFERENCE_BIT | HELP_ENDGAME_BIT |
                            HELP_SOLVER_BIT | HELP_PEG_BIT | HELP_POSITION_BITS,
  HELP_ALL_GROUP_BITS = HELP_GROUP_BIT(NUMBER_OF_HELP_OPTION_GROUPS) - 1,
};

static const HelpCommandGroups command_groups[] = {
    {ARG_TOKEN_NEW_GAME, 0, HELP_SETUP_BIT | HELP_SESSION_BIT},
    {ARG_TOKEN_LOAD, 0, HELP_SETUP_BIT | HELP_SESSION_BIT},
    {ARG_TOKEN_CGP, 0, HELP_SETUP_BIT | HELP_SESSION_BIT},
    {ARG_TOKEN_EXPORT, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_P1_NAME, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_P2_NAME, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_SWITCH_NAMES, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_RACK, 0, 0},
    {ARG_TOKEN_RANDOM_RACK, 0, 0},
    {ARG_TOKEN_MOVES, 0, 0},
    {ARG_TOKEN_COMMIT, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_TOP_COMMIT, 0, HELP_MOVEGEN_BIT | HELP_SESSION_BIT},
    {ARG_TOKEN_CNOTE, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_NOTE, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_CHALLENGE, 0, HELP_SETUP_BIT | HELP_SESSION_BIT},
    {ARG_TOKEN_UNCHALLENGE, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_OVERTIME, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_GOTO, 0, 0},
    {ARG_TOKEN_NEXT, 0, 0},
    {ARG_TOKEN_PREVIOUS, 0, 0},
    {ARG_TOKEN_GEN, HELP_MOVEGEN_BIT, HELP_PAT_BIT | HELP_POSITION_BITS},
    {ARG_TOKEN_RACK_AND_GEN, HELP_MOVEGEN_BIT,
     HELP_PAT_BIT | HELP_POSITION_BITS},
    {ARG_TOKEN_SIM, HELP_SIM_BIT, HELP_SIM_POINTER_BITS},
    {ARG_TOKEN_GEN_AND_SIM, HELP_SIM_BIT, HELP_SIM_POINTER_BITS},
    {ARG_TOKEN_RACK_AND_GEN_AND_SIM, HELP_SIM_BIT, HELP_SIM_POINTER_BITS},
    {ARG_TOKEN_SNOPRUNE, HELP_SIM_BIT, HELP_SIM_POINTER_BITS},
    {ARG_TOKEN_INFER, HELP_INFERENCE_BIT, HELP_POSITION_BITS},
    {ARG_TOKEN_ENDGAME, HELP_ENDGAME_BIT | HELP_SOLVER_BIT, HELP_POSITION_BITS},
    {ARG_TOKEN_PEG, HELP_PEG_BIT | HELP_SOLVER_BIT, HELP_POSITION_BITS},
    {ARG_TOKEN_ANALYZE, HELP_ANALYZE_BIT,
     HELP_SIM_BIT | HELP_ENDGAME_BIT | HELP_SOLVER_BIT | HELP_PEG_BIT |
         HELP_SIM_POINTER_BITS},
    {ARG_TOKEN_SHOW_GAME, 0, HELP_DISPLAY_BIT},
    {ARG_TOKEN_SHOW_MOVES, 0, HELP_DISPLAY_BIT},
    {ARG_TOKEN_SHOW_INFERENCE, 0, HELP_DISPLAY_BIT},
    {ARG_TOKEN_SHOW_ENDGAME, 0, HELP_DISPLAY_BIT},
    {ARG_TOKEN_SHOW_PEG, 0, HELP_PEG_BIT | HELP_DISPLAY_BIT},
    {ARG_TOKEN_SHOW_HEAT_MAP, 0, HELP_DISPLAY_BIT},
    {ARG_TOKEN_AUTOPLAY, HELP_AUTOPLAY_BIT,
     HELP_PAT_BIT | HELP_BATCH_POINTER_BITS},
    {ARG_TOKEN_LEAVE_GEN, 0,
     HELP_AUTOPLAY_BIT | HELP_PAT_BIT | HELP_BATCH_POINTER_BITS},
    {ARG_TOKEN_PAT_GEN, HELP_PAT_BIT,
     HELP_AUTOPLAY_BIT | HELP_BATCH_POINTER_BITS},
    {ARG_TOKEN_CONTRIBUTE, 0, HELP_TABLES_BIT},
    {ARG_TOKEN_SET, 0, HELP_ALL_GROUP_BITS},
    {ARG_TOKEN_DATA, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_CONVERT, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_CREATE_DATA, 0, HELP_SESSION_BIT},
    {ARG_TOKEN_BUILDERS, 0, 0},
    {ARG_TOKEN_VERSION, 0, 0},
    {ARG_TOKEN_HELP, 0, 0},
};

const HelpOptionGroup *help_table_get_option_group(help_option_group_t group) {
  return &option_groups[group];
}

int help_table_get_num_command_sections(void) {
  return HELP_ARRAY_LENGTH(command_sections);
}

const HelpCommandSection *help_table_get_command_section(int section_idx) {
  return &command_sections[section_idx];
}

help_option_group_t help_table_find_option_group(arg_token_t option) {
  for (int group_idx = 0; group_idx < NUMBER_OF_HELP_OPTION_GROUPS;
       group_idx++) {
    const HelpOptionGroup *group = &option_groups[group_idx];
    for (int row_idx = 0; row_idx < group->num_rows; row_idx++) {
      const HelpOptionRow *row = &group->rows[row_idx];
      if (row->both_players == option || row->player_one == option ||
          row->player_two == option) {
        return (help_option_group_t)group_idx;
      }
    }
  }
  return NUMBER_OF_HELP_OPTION_GROUPS;
}

static const HelpCommandGroups *get_command_groups(arg_token_t command) {
  for (int command_idx = 0; command_idx < HELP_ARRAY_LENGTH(command_groups);
       command_idx++) {
    if (command_groups[command_idx].command == command) {
      return &command_groups[command_idx];
    }
  }
  log_fatal("command token %d has no help table row", command);
  return NULL;
}

bool help_table_command_lists_group(arg_token_t command,
                                    help_option_group_t group) {
  return (get_command_groups(command)->listed_groups & HELP_GROUP_BIT(group)) !=
         0;
}

bool help_table_command_points_to_group(arg_token_t command,
                                        help_option_group_t group) {
  return (get_command_groups(command)->pointed_to_groups &
          HELP_GROUP_BIT(group)) != 0;
}
