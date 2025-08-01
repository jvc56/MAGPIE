#ifndef IO_UTIL_H
#define IO_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
  ERROR_STATUS_SUCCESS,
  // Config load errors
  ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
  ERROR_STATUS_CONFIG_LOAD_DUPLICATE_ARG,
  ERROR_STATUS_CONFIG_LOAD_MISPLACED_COMMAND,
  ERROR_STATUS_CONFIG_LOAD_AMBIGUOUS_COMMAND,
  ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
  ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
  ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
  ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
  ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING,
  ERROR_STATUS_CONFIG_LOAD_INSUFFICIENT_NUMBER_OF_VALUES,
  ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_GAME_VARIANT,
  ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_EXEC_MODE,
  ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_CREATE_DATA_TYPE,
  ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_SORT_TYPE,
  ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_RECORD_TYPE,
  ERROR_STATUS_CONFIG_LOAD_MALFORMED_SAMPLING_RULE,
  ERROR_STATUS_CONFIG_LOAD_MALFORMED_THRESHOLD,
  ERROR_STATUS_CONFIG_LOAD_MALFORMED_BOOL_ARG,
  ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG,
  ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG,
  ERROR_STATUS_CONFIG_LOAD_MISSING_ARG,
  ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LEXICONS,
  ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LETTER_DISTRIBUTION,
  ERROR_STATUS_CONFIG_LOAD_BOARD_LAYOUT_ERROR,
  ERROR_STATUS_CONFIG_LOAD_WIN_PCT_ERROR,
  ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
  // CGP load errors
  ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS,
  ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_ROWS,
  ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_COLUMNS,
  ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_PLAYER_RACKS,
  ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_PLAYER_SCORES,
  ERROR_STATUS_CGP_PARSE_MALFORMED_BOARD_LETTERS,
  ERROR_STATUS_CGP_PARSE_MALFORMED_SCORES,
  ERROR_STATUS_CGP_PARSE_MALFORMED_RACK_LETTERS,
  ERROR_STATUS_CGP_PARSE_MALFORMED_CONSECUTIVE_ZEROS,
  ERROR_STATUS_CGP_PARSE_MALFORMED_CGP_OPCODE_BINGO_BONUS,
  ERROR_STATUS_CGP_PARSE_MALFORMED_CGP_OPCODE_BOARD_NAME,
  ERROR_STATUS_CGP_PARSE_MALFORMED_CGP_OPCODE_GAME_VARIANT,
  ERROR_STATUS_CGP_PARSE_BOARD_LETTERS_NOT_IN_BAG,
  ERROR_STATUS_CGP_PARSE_RACK_LETTERS_NOT_IN_BAG,
  // Leavegen error
  ERROR_STATUS_LEAVE_GEN_DIFFERENT_LEXICA_OR_LEAVES,
  // Inference errors
  ERROR_STATUS_INFERENCE_NO_TILES_PLAYED,
  ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
  ERROR_STATUS_INFERENCE_TILES_PLAYED_NOT_IN_BAG,
  ERROR_STATUS_INFERENCE_BOTH_PLAY_AND_EXCHANGE,
  ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO,
  ERROR_STATUS_INFERENCE_EXCHANGE_NOT_ALLOWED,
  // Autoplay errors
  ERROR_STATUS_AUTOPLAY_EMPTY_OPTIONS,
  ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS,
  ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS,
  ERROR_STATUS_AUTOPLAY_MALFORMED_NUM_GAMES,
  // Board layout errors
  ERROR_STATUS_BOARD_LAYOUT_MALFORMED_START_COORDS,
  ERROR_STATUS_BOARD_LAYOUT_OUT_OF_BOUNDS_START_COORDS,
  ERROR_STATUS_BOARD_LAYOUT_INVALID_NUMBER_OF_ROWS,
  ERROR_STATUS_BOARD_LAYOUT_INVALID_NUMBER_OF_COLS,
  ERROR_STATUS_BOARD_LAYOUT_INVALID_BONUS_SQUARE,
  // Convert errors
  ERROR_STATUS_CONVERT_INPUT_FILE_ERROR,
  ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
  ERROR_STATUS_CONVERT_KWG_TOO_LARGE_FOR_FORMAT,
  ERROR_STATUS_CONVERT_TEXT_CONTAINS_INVALID_LETTER,
  ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_LONG,
  ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_SHORT,
  ERROR_STATUS_CONVERT_MALFORMED_KWG,
  ERROR_STATUS_CONVERT_UNRECOGNIZED_CONVERSION_TYPE,
  ERROR_STATUS_CONVERT_UNIMPLEMENTED_CONVERSION_TYPE,
  // GCG Parse errors
  ERROR_STATUS_GCG_PARSE_LEXICON_NOT_SPECIFIED,
  ERROR_STATUS_GCG_PARSE_DUPLICATE_NAMES,
  ERROR_STATUS_GCG_PARSE_DUPLICATE_NICKNAMES,
  ERROR_STATUS_GCG_PARSE_PRAGMA_SUCCEEDED_EVENT,
  ERROR_STATUS_GCG_PARSE_MISPLACED_ENCODING,
  ERROR_STATUS_GCG_PARSE_PLAYER_DOES_NOT_EXIST,
  ERROR_STATUS_GCG_PARSE_PLAYER_NUMBER_REDUNDANT,
  ERROR_STATUS_GCG_PARSE_UNSUPPORTED_CHARACTER_ENCODING,
  ERROR_STATUS_GCG_PARSE_GCG_EMPTY,
  ERROR_STATUS_GCG_PARSE_NOTE_PRECEDENT_EVENT,
  ERROR_STATUS_GCG_PARSE_NO_MATCHING_TOKEN,
  ERROR_STATUS_GCG_PARSE_PHONY_TILES_RETURNED_WITHOUT_PLAY,
  ERROR_STATUS_GCG_PARSE_PHONY_TILES_RETURNED_MISMATCH,
  ERROR_STATUS_GCG_PARSE_CHALLENGE_BONUS_WITHOUT_PLAY,
  ERROR_STATUS_GCG_PARSE_INVALID_CHALLENGE_BONUS_PLAYER_INDEX,
  ERROR_STATUS_GCG_PARSE_INVALID_PHONY_TILES_PLAYER_INDEX,
  ERROR_STATUS_GCG_PARSE_PLAYED_LETTERS_NOT_IN_RACK,
  ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
  ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
  ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
  ERROR_STATUS_GCG_PARSE_RACK_END_POINTS_INCORRECT,
  ERROR_STATUS_GCG_PARSE_END_RACK_PENALTY_INCORRECT,
  ERROR_STATUS_GCG_PARSE_UNRECOGNIZED_GAME_VARIANT,
  ERROR_STATUS_GCG_PARSE_MOVE_BEFORE_PLAYER,
  ERROR_STATUS_GCG_PARSE_REDUNDANT_PRAGMA,
  ERROR_STATUS_GCG_PARSE_EVENT_AFTER_LAST_RACK,
  ERROR_STATUS_GCG_PARSE_GAME_EVENTS_OVERFLOW,
  ERROR_STATUS_GCG_PARSE_GAME_EVENT_OFF_TURN,
  ERROR_STATUS_GCG_PARSE_MOVE_EVENT_AFTER_GAME_END,
  ERROR_STATUS_GCG_PARSE_END_GAME_EVENT_BEFORE_GAME_END,
  ERROR_STATUS_GCG_PARSE_MOVE_SCORING_ERROR,
  ERROR_STATUS_GCG_PARSE_CUMULATIVE_SCORING_ERROR,
  ERROR_STATUS_GCG_PARSE_CONFIG_LOAD_ERROR,
  ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR,
  ERROR_STATUS_GCG_PARSE_GAME_EVENT_OVERFLOW,
  // Simmer errors
  ERROR_STATUS_SIM_NO_MOVES,
  // Move validation errors
  ERROR_STATUS_MOVE_VALIDATION_GAME_NOT_LOADED,
  ERROR_STATUS_MOVE_VALIDATION_EMPTY_MOVE,
  ERROR_STATUS_MOVE_VALIDATION_INVALID_PLAYER_INDEX,
  ERROR_STATUS_MOVE_VALIDATION_EMPTY_MOVE_TYPE_OR_POSITION,
  ERROR_STATUS_MOVE_VALIDATION_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED,
  ERROR_STATUS_MOVE_VALIDATION_EMPTY_RACK,
  ERROR_STATUS_MOVE_VALIDATION_EMPTY_CHALLENGE_POINTS,
  ERROR_STATUS_MOVE_VALIDATION_EMPTY_CHALLENGE_TURN_LOSS,
  ERROR_STATUS_MOVE_VALIDATION_NONEXCHANGE_NUMERIC_TILES,
  ERROR_STATUS_MOVE_VALIDATION_EXCHANGE_INSUFFICIENT_TILES,
  ERROR_STATUS_MOVE_VALIDATION_INVALID_NUMBER_EXCHANGED,
  ERROR_STATUS_MOVE_VALIDATION_INVALID_TILES_PLAYED,
  ERROR_STATUS_MOVE_VALIDATION_INVALID_RACK,
  ERROR_STATUS_MOVE_VALIDATION_INVALID_CHALLENGE_POINTS,
  ERROR_STATUS_MOVE_VALIDATION_INVALID_CHALLENGE_TURN_LOSS,
  ERROR_STATUS_MOVE_VALIDATION_RACK_NOT_IN_BAG,
  ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_NOT_IN_RACK,
  ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OUT_OF_BOUNDS,
  ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OVER_BRICK,
  ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OVERFLOW,
  ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_BOARD_MISMATCH,
  ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_DISCONNECTED,
  ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION,
  ERROR_STATUS_MOVE_VALIDATION_EXCESS_EXCHANGE_FIELDS,
  ERROR_STATUS_MOVE_VALIDATION_EXCESS_FIELDS,
  ERROR_STATUS_MOVE_VALIDATION_MISSING_FIELDS,
  ERROR_STATUS_MOVE_VALIDATION_EXCESS_PASS_FIELDS,
  ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED,
  ERROR_STATUS_MOVE_VALIDATION_UNKNOWN_EXCHANGE_DISALLOWED,
  // Command
  ERROR_STATUS_COMMAND_STILL_RUNNING,
  ERROR_STATUS_COMMAND_NOTHING_TO_STOP,
  // Data filepaths
  ERROR_STATUS_FILEPATH_NULL_PATH,
  ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
  ERROR_STATUS_FILEPATH_NULL_FILENAME,
  ERROR_STATUS_FILEPATH_FILE_NOT_WRITABLE,
  ERROR_STATUS_FILEPATH_DIRECTORY_NOT_WRITABLE,
  ERROR_STATUS_FILEPATH_NO_MATCHING_FILES,
  ERROR_STATUS_FILEPATH_FAILED_TO_CREATE_DIRECTORY,
  // Letter distribution
  ERROR_STATUS_LD_INVALID_ROW,
  ERROR_STATUS_LD_LEXICON_DEFAULT_NOT_FOUND,
  ERROR_STATUS_LD_NAME_NOT_FOUND,
  ERROR_STATUS_LD_UNSUPPORTED_BOARD_DIM_DEFAULT,
  // Read Write errors
  ERROR_STATUS_RW_FAILED_TO_OPEN_STREAM,
  ERROR_STATUS_RW_MEMORY_ALLOCATION_ERROR,
  ERROR_STATUS_RW_READ_ERROR,
  ERROR_STATUS_RW_WRITE_ERROR,
  // Win Percentage
  ERROR_STATUS_WIN_PCT_NO_DATA_FOUND,
  ERROR_STATUS_WIN_PCT_INVALID_NUMBER_OF_COLUMNS,
  ERROR_STATUS_WIN_PCT_INVALID_SPREAD,
  ERROR_STATUS_WIN_PCT_INVALID_TOTAL_GAMES,
  ERROR_STATUS_WIN_PCT_INVALID_TOTAL_WINS,
  // WMP errors
  ERROR_STATUS_WMP_UNSUPPORTED_VERSION,
  ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM,
  // KLV errors
  ERROR_STATUS_KLV_LINE_EXCEEDS_MAX_LENGTH,
  ERROR_STATUS_KLV_DUPLICATE_LEAVE,
  ERROR_STATUS_KLV_INVALID_LEAVE,
  ERROR_STATUS_KLV_INVALID_ROW,
  // String conversion errors
  ERROR_STATUS_STRING_TO_INT_CONVERSION_FAILED,
  ERROR_STATUS_STRING_TO_DOUBLE_CONVERSION_FAILED,
  ERROR_STATUS_FOUND_SIGN_FOR_UNSIGNED_INT,
} error_code_t;

typedef enum {
  LOG_TRACE,
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_FATAL,
} log_level_t;

#define log_trace(...) log_with_info(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_with_info(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...) log_with_info(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...) log_with_info(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_with_info(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

void log_with_info(log_level_t log_level, const char *caller_filename,
                   int caller_line, const char *format, ...);

void write_to_stream_out(const char *fmt, ...);
void write_to_stream_err(const char *fmt, ...);
void write_to_stream(FILE *stream, const char *fmt, ...);
char *read_line_from_stream_in(void);

// WARNING: This function should only be called once at startup or for testing
void log_set_level(log_level_t new_log_level);

// WARNING: This function should only be used for testing
void io_set_stream_out(FILE *stream);

// WARNING: This function should only be used for testing
void io_reset_stream_out(void);

// WARNING: This function should only be used for testing
void io_set_stream_err(FILE *stream);

// WARNING: This function should only be used for testing
void io_reset_stream_err(void);

// WARNING: This function should only be used for testing
void io_set_stream_in(FILE *stream);

// WARNING: This function should only be used for testing
void io_reset_stream_in(void);

char *format_string_with_va_list(const char *format, va_list *args);
char *get_formatted_string(const char *format, ...);
void fflush_or_die(FILE *stream);
void *malloc_or_die(size_t size);
void *calloc_or_die(size_t num, size_t size);
void *realloc_or_die(void *realloc_target, size_t size);

typedef struct ErrorStack ErrorStack;

ErrorStack *error_stack_create(void);
void error_stack_destroy(ErrorStack *error_stack);
void error_stack_push(ErrorStack *error_stack, error_code_t error_code,
                      char *msg);
error_code_t error_stack_top(ErrorStack *error_stack);
char *error_stack_get_string_and_reset(ErrorStack *error_stack);
void error_stack_print_and_reset(ErrorStack *error_stack);
bool error_stack_is_empty(const ErrorStack *error_stack);

// WARNING: for testing only, production code should only reset the stack after
// printing or retrieving the error string
void error_stack_reset(ErrorStack *error_stack);

void fseek_or_die(FILE *stream, long offset, int whence);
char *get_string_from_file(const char *filename, ErrorStack *error_stack);
void write_string_to_file(const char *filename, const char *mode,
                          const char *string, ErrorStack *error_stack);
FILE *fopen_or_die(const char *filename, const char *mode);
FILE *fopen_safe(const char *filename, const char *mode,
                 ErrorStack *error_stack);
void fclose_or_die(FILE *stream);
void fwrite_or_die(const void *ptr, size_t size, size_t nmemb, FILE *stream,
                   const char *description);
void fprintf_or_die(FILE *stream, const char *format, ...);

#endif
