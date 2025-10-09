#include "players_data.h"

#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../impl/kwg_maker.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "dictionary_word.h"
#include "klv.h"
#include "kwg.h"
#include "wmp.h"
#include <stdlib.h>
#include <time.h>

static const char *const players_data_type_names[] = {"kwg", "klv", "wordmap",
                                                      "words"};

// The PlayersData struct holds all of the
// information that can be set during configuration.
// This is then copied to the Player struct in the
// game before any operations on the game are performed.
struct PlayersData {
  bool data_is_shared[NUMBER_OF_DATA];
  void *data[(NUMBER_OF_DATA * 2)];
  move_sort_t move_sort_types[2];
  move_record_t move_record_types[2];
  char *player_names[2];
};

#define DEFAULT_MOVE_SORT_TYPE MOVE_SORT_EQUITY
#define DEFAULT_MOVE_RECORD_TYPE MOVE_RECORD_ALL

int players_data_get_player_data_index(players_data_t players_data_type,
                                       int player_index) {
  return (int)players_data_type * 2 + player_index;
}

void players_data_set_name(PlayersData *players_data, int player_index,
                           const char *player_name) {
  free(players_data->player_names[player_index]);
  players_data->player_names[player_index] = string_duplicate(player_name);
}

const char *players_data_get_name(const PlayersData *players_data,
                                  int player_index) {
  return players_data->player_names[player_index];
}

void players_data_set_move_sort_type(PlayersData *players_data,
                                     int player_index,
                                     move_sort_t move_sort_type) {
  players_data->move_sort_types[player_index] = move_sort_type;
}

move_sort_t players_data_get_move_sort_type(const PlayersData *players_data,
                                            int player_index) {
  return players_data->move_sort_types[player_index];
}

void players_data_set_move_record_type(PlayersData *players_data,
                                       int player_index,
                                       move_record_t move_record_type) {
  players_data->move_record_types[player_index] = move_record_type;
}

move_record_t players_data_get_move_record_type(const PlayersData *players_data,
                                                int player_index) {
  return players_data->move_record_types[player_index];
}

bool players_data_get_is_shared(const PlayersData *players_data,
                                players_data_t players_data_type) {
  return players_data->data_is_shared[(int)players_data_type];
}

void players_data_set_is_shared(PlayersData *players_data,
                                players_data_t players_data_type,
                                bool is_shared) {
  players_data->data_is_shared[(int)players_data_type] = is_shared;
}

void *players_data_get_data(const PlayersData *players_data,
                            players_data_t players_data_type,
                            int player_index) {
  // Data must be allocated by the caller
  int data_index =
      players_data_get_player_data_index(players_data_type, player_index);
  return players_data->data[data_index];
}

KWG *players_data_get_kwg(const PlayersData *players_data, int player_index) {
  return (KWG *)players_data_get_data(players_data, PLAYERS_DATA_TYPE_KWG,
                                      player_index);
}

KLV *players_data_get_klv(const PlayersData *players_data, int player_index) {
  return (KLV *)players_data_get_data(players_data, PLAYERS_DATA_TYPE_KLV,
                                      player_index);
}

WMP *players_data_get_wmp(const PlayersData *players_data, int player_index) {
  return (WMP *)players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP,
                                      player_index);
}

DictionaryWordList *
players_data_get_unsorted_words(const PlayersData *players_data,
                                int player_index) {
  return (DictionaryWordList *)players_data_get_data(
      players_data, PLAYERS_DATA_TYPE_UNSORTED_WORDS, player_index);
}

void players_data_set_data(PlayersData *players_data,
                           players_data_t players_data_type, int player_index,
                           void *data) {
  // Data must be allocated by the caller
  int data_index =
      players_data_get_player_data_index(players_data_type, player_index);
  players_data->data[data_index] = data;
}

void *players_data_create_data(players_data_t players_data_type,
                               const char *data_paths, const char *data_name,
                               ErrorStack *error_stack) {
  if (!data_name) {
    return NULL;
  }
  void *data = NULL;
  switch (players_data_type) {
  case PLAYERS_DATA_TYPE_KWG:
    data = kwg_create(data_paths, data_name, error_stack);
    break;
  case PLAYERS_DATA_TYPE_KLV:
    data = klv_create(data_paths, data_name, error_stack);
    break;
  case PLAYERS_DATA_TYPE_WMP:
    data = wmp_create(data_paths, data_name, error_stack);
    break;
  case PLAYERS_DATA_TYPE_UNSORTED_WORDS: {
    // Create KWG, dump to word list, shuffle, and destroy KWG
    KWG *kwg = kwg_create(data_paths, data_name, error_stack);
    if (!error_stack_is_empty(error_stack) || !kwg) {
      return NULL;
    }
    DictionaryWordList *word_list = dictionary_word_list_create();
    kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), word_list, NULL);
    printf("Lexicon %s has %d words.\n", kwg->name,
           dictionary_word_list_get_count(word_list));
    kwg_destroy(kwg);
    // Use current time as seed for shuffle
    dictionary_word_list_shuffle(word_list, (uint64_t)time(NULL));
    data = word_list;
    break;
  }
  case NUMBER_OF_DATA:
    log_fatal("cannot create invalid players data type");
    break;
  }
  return data;
}

void players_data_destroy_data(PlayersData *players_data,
                               players_data_t players_data_type,
                               int player_index) {
  int data_index =
      players_data_get_player_data_index(players_data_type, player_index);
  if (players_data->data[data_index]) {
    switch (players_data_type) {
    case PLAYERS_DATA_TYPE_KWG:
      kwg_destroy(players_data->data[data_index]);
      break;
    case PLAYERS_DATA_TYPE_KLV:
      klv_destroy(players_data->data[data_index]);
      break;
    case PLAYERS_DATA_TYPE_WMP:
      wmp_destroy(players_data->data[data_index]);
      break;
    case PLAYERS_DATA_TYPE_UNSORTED_WORDS:
      dictionary_word_list_destroy(players_data->data[data_index]);
      break;
    case NUMBER_OF_DATA:
      log_fatal("cannot destroy invalid players data type");
      break;
    }
    players_data->data[data_index] = NULL;
  }
}

int get_index_of_existing_data(const PlayersData *players_data,
                               players_data_t players_data_type,
                               const char *data_name) {
  int index = -1;
  for (int player_index = 0; player_index < 2; player_index++) {
    if (strings_equal(players_data_get_data_name(
                          players_data, players_data_type, player_index),
                      data_name)) {
      index = player_index;
      break;
    }
  }
  return index;
}

const char *players_data_get_data_name(const PlayersData *players_data,
                                       players_data_t players_data_type,
                                       int player_index) {

  const char *data_name = NULL;
  int data_index =
      players_data_get_player_data_index(players_data_type, player_index);
  if (players_data->data[data_index]) {
    switch (players_data_type) {
    case PLAYERS_DATA_TYPE_KWG:
      data_name = kwg_get_name(players_data->data[data_index]);
      break;
    case PLAYERS_DATA_TYPE_KLV:
      data_name = klv_get_name(players_data->data[data_index]);
      break;
    case PLAYERS_DATA_TYPE_WMP:
      data_name = wmp_get_name(players_data->data[data_index]);
      break;
    case PLAYERS_DATA_TYPE_UNSORTED_WORDS:
      // UNSORTED_WORDS doesn't have a name - it's derived from KWG
      data_name = NULL;
      break;
    case NUMBER_OF_DATA:
      log_fatal("cannot destroy invalid players data type");
      break;
    }
  }
  return data_name;
}

PlayersData *players_data_create(void) {
  PlayersData *players_data = malloc_or_die(sizeof(PlayersData));
  for (int player_index = 0; player_index < 2; player_index++) {
    for (int data_index = 0; data_index < NUMBER_OF_DATA; data_index++) {
      int player_data_index = players_data_get_player_data_index(
          (players_data_t)data_index, player_index);
      players_data->data_is_shared[data_index] = false;
      players_data->data[player_data_index] = NULL;
    }
    players_data_set_move_sort_type(players_data, player_index,
                                    DEFAULT_MOVE_SORT_TYPE);
    players_data_set_move_record_type(players_data, player_index,
                                      DEFAULT_MOVE_RECORD_TYPE);
    players_data->player_names[player_index] = NULL;
  }
  return players_data;
}

void players_data_destroy(PlayersData *players_data) {
  if (!players_data) {
    return;
  }
  for (int i = 0; i < 2; i++) {
    free(players_data->player_names[i]);
  }
  for (int data_index = 0; data_index < NUMBER_OF_DATA; data_index++) {
    bool is_shared =
        players_data_get_is_shared(players_data, (players_data_t)data_index);
    for (int player_index = 0; player_index < 2; player_index++) {
      if (!is_shared || player_index == 0) {
        players_data_destroy_data(players_data, (players_data_t)data_index,
                                  player_index);
      }
    }
  }
  free(players_data);
}

bool players_data_type_is_nullable(players_data_t players_data_type) {
  return players_data_type == PLAYERS_DATA_TYPE_WMP;
}

void players_data_set(PlayersData *players_data,
                      players_data_t players_data_type, const char *data_paths,
                      const char *p1_data_name, const char *p2_data_name,
                      ErrorStack *error_stack) {
  // WMP is optional, KWG and KLV are required for every player.
  if (!players_data_type_is_nullable(players_data_type)) {
    if (is_string_empty_or_null(p1_data_name)) {
      log_fatal("cannot set data type '%s' to null for player one",
                players_data_type_names[players_data_type]);
    }
    if (is_string_empty_or_null(p2_data_name)) {
      log_fatal("cannot set data type '%s' to null for player two",
                players_data_type_names[players_data_type]);
    }
  }

  const char *input_data_names[2];
  input_data_names[0] = p1_data_name;
  input_data_names[1] = p2_data_name;

  bool new_data_is_shared =
      strings_equal(input_data_names[0], input_data_names[1]);
  bool old_data_is_shared =
      players_data_get_is_shared(players_data, players_data_type);
  void *data_pointers[2];

  for (int player_index = 0; player_index < 2; player_index++) {
    data_pointers[player_index] = NULL;
  }

  int existing_data_indexes[2];

  for (int player_index = 0; player_index < 2; player_index++) {
    existing_data_indexes[player_index] = get_index_of_existing_data(
        players_data, players_data_type, input_data_names[player_index]);
  }

  for (int player_index = 0; player_index < 2; player_index++) {
    if (existing_data_indexes[player_index] < 0) {
      if (player_index == 1 && new_data_is_shared) {
        data_pointers[1] = data_pointers[0];
      } else {
        void *generic_players_data = players_data_create_data(
            players_data_type, data_paths, input_data_names[player_index],
            error_stack);
        if (!error_stack_is_empty(error_stack)) {
          return;
        }
        data_pointers[player_index] = generic_players_data;
      }
    } else {
      data_pointers[player_index] = players_data_get_data(
          players_data, players_data_type, existing_data_indexes[player_index]);
    }
  }

  // Possibly destroy existing data
  // and set new data
  for (int player_index = 0; player_index < 2; player_index++) {
    const void *existing_data =
        players_data_get_data(players_data, players_data_type, player_index);
    if (existing_data != data_pointers[0] &&
        existing_data != data_pointers[1] &&
        (player_index == 0 || !old_data_is_shared)) {
      players_data_destroy_data(players_data, players_data_type, player_index);
    }
    players_data_set_data(players_data, players_data_type, player_index,
                          data_pointers[player_index]);
  }
  players_data_set_is_shared(players_data, players_data_type,
                             new_data_is_shared);
}

// Destroys and recreates the existing data for both players.
void players_data_reload(PlayersData *players_data,
                         players_data_t players_data_type,
                         const char *data_paths, ErrorStack *error_stack) {
  const bool data_is_shared =
      players_data_get_is_shared(players_data, players_data_type);
  void *recreated_data[2];
  for (int player_index = 0; player_index < 2; player_index++) {
    if (player_index == 0 || !data_is_shared) {
      recreated_data[player_index] = players_data_create_data(
          players_data_type, data_paths,
          players_data_get_data_name(players_data, players_data_type,
                                     player_index),
          error_stack);
    }
  }

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  for (int player_index = 0; player_index < 2; player_index++) {
    if (player_index == 1 && data_is_shared) {
      players_data_set_data(
          players_data, players_data_type, player_index,
          players_data_get_data(players_data, players_data_type, 0));
    } else {
      players_data_destroy_data(players_data, players_data_type, player_index);
      players_data_set_data(players_data, players_data_type, player_index,
                            recreated_data[player_index]);
    }
  }
}