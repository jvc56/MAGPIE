#include <stdlib.h>

#include "klv.h"
#include "kwg.h"
#include "log.h"
#include "players_data.h"

struct PlayersData {
  bool data_is_shared[NUMBER_OF_DATA];
  char *data_names[(NUMBER_OF_DATA * 2)];
  void *data[(NUMBER_OF_DATA * 2)];
  move_sort_t move_sort_types[2];
  move_record_t move_record_types[2];
  const char *player_names[2];
};

#define DEFAULT_MOVE_SORT_TYPE MOVE_SORT_EQUITY
#define DEFAULT_MOVE_RECORD_TYPE MOVE_RECORD_BEST

int players_data_get_player_data_index(players_data_t players_data_type,
                                       int player_index) {
  return players_data_type * 2 + player_index;
}

void players_data_set_name(PlayersData *players_data, int player_index,
                           const char *player_name) {
  players_data->player_names[player_index] = player_name;
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
  return players_data->data_is_shared[players_data_type];
}

void players_data_set_is_shared(PlayersData *players_data,
                                players_data_t players_data_type,
                                bool is_shared) {
  players_data->data_is_shared[players_data_type] = is_shared;
}

void players_data_destroy_data_name(PlayersData *players_data,
                                    players_data_t players_data_type,
                                    int player_index) {
  int data_name_index =
      players_data_get_player_data_index(players_data_type, player_index);
  if (players_data->data_names[data_name_index]) {
    free(players_data->data_names[data_name_index]);
    players_data->data_names[data_name_index] = NULL;
  }
}

void players_data_set_data_name(PlayersData *players_data,
                                players_data_t players_data_type,
                                int player_index, const char *data_name) {
  int data_name_index =
      players_data_get_player_data_index(players_data_type, player_index);
  if (strings_equal(players_data->data_names[data_name_index], data_name)) {
    return;
  }
  players_data_destroy_data_name(players_data, players_data_type, player_index);
  players_data->data_names[data_name_index] = string_duplicate(data_name);
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

void players_data_set_data(PlayersData *players_data,
                           players_data_t players_data_type, int player_index,
                           void *data) {
  // Data must be allocated by the caller
  int data_index =
      players_data_get_player_data_index(players_data_type, player_index);
  players_data->data[data_index] = data;
}

void *players_data_create_data(players_data_t players_data_type,
                               const char *data_name) {
  void *data = NULL;
  switch (players_data_type) {
  case PLAYERS_DATA_TYPE_KWG:
    data = create_kwg(data_name);
    break;
  case PLAYERS_DATA_TYPE_KLV:
    data = create_klv(data_name);
    break;
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
      destroy_kwg(players_data->data[data_index]);
      break;
    case PLAYERS_DATA_TYPE_KLV:
      destroy_klv(players_data->data[data_index]);
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

char *players_data_get_data_name(const PlayersData *players_data,
                                 players_data_t players_data_type,
                                 int player_index) {
  int data_name_index =
      players_data_get_player_data_index(players_data_type, player_index);
  return players_data->data_names[data_name_index];
}

PlayersData *create_players_data() {
  PlayersData *players_data = malloc_or_die(sizeof(PlayersData));
  for (int player_index = 0; player_index < 2; player_index++) {
    for (int data_index = 0; data_index < NUMBER_OF_DATA; data_index++) {
      int player_data_index = players_data_get_player_data_index(
          (players_data_t)data_index, player_index);
      players_data->data_is_shared[data_index] = false;
      players_data->data_names[player_data_index] = NULL;
      players_data->data[player_data_index] = NULL;
    }
    players_data_set_move_sort_type(players_data, player_index,
                                    DEFAULT_MOVE_SORT_TYPE);
    players_data_set_move_record_type(players_data, player_index,
                                      DEFAULT_MOVE_RECORD_TYPE);
    players_data_set_name(players_data, player_index, NULL);
  }
  return players_data;
}

void destroy_players_data(PlayersData *players_data) {
  for (int data_index = 0; data_index < NUMBER_OF_DATA; data_index++) {
    bool is_shared =
        players_data_get_is_shared(players_data, (players_data_t)data_index);
    for (int player_index = 0; player_index < 2; player_index++) {
      players_data_destroy_data_name(players_data, (players_data_t)data_index,
                                     player_index);
      if (!is_shared || player_index == 0) {
        players_data_destroy_data(players_data, (players_data_t)data_index,
                                  player_index);
      }
    }
  }
  free(players_data);
}

void set_players_data(PlayersData *players_data,
                      players_data_t players_data_type,
                      const char *p1_data_name, const char *p2_data_name) {

  if (is_string_empty_or_null(p1_data_name)) {
    log_fatal("cannot set player one data with null or empty name");
  }
  if (is_string_empty_or_null(p2_data_name)) {
    log_fatal("cannot set player two data with null or empty name");
  }

  const char *input_data_names[2];
  input_data_names[0] = p1_data_name;
  input_data_names[1] = p2_data_name;

  bool new_data_is_shared =
      strings_equal(input_data_names[0], input_data_names[1]);
  bool old_data_is_shared =
      players_data_get_is_shared(players_data, players_data_type);
  void *data_pointers[2];
  char *data_names[2];

  for (int player_index = 0; player_index < 2; player_index++) {
    data_pointers[player_index] = NULL;
    data_names[player_index] = NULL;
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
        data_names[1] = string_duplicate(data_names[0]);
      } else {
        data_pointers[player_index] = players_data_create_data(
            players_data_type, input_data_names[player_index]);
        data_names[player_index] =
            string_duplicate(input_data_names[player_index]);
      }
    } else {
      data_pointers[player_index] = players_data_get_data(
          players_data, players_data_type, existing_data_indexes[player_index]);
      data_names[player_index] = get_formatted_string(
          "%s",
          players_data_get_data_name(players_data, players_data_type,
                                     existing_data_indexes[player_index]));
    }
  }

  // Possibly destroy existing data
  // and set new data
  for (int player_index = 0; player_index < 2; player_index++) {
    void *existing_data =
        players_data_get_data(players_data, players_data_type, player_index);
    if (existing_data != data_pointers[0] &&
        existing_data != data_pointers[1] &&
        (player_index == 0 || !old_data_is_shared)) {
      players_data_destroy_data(players_data, players_data_type, player_index);
    }
    players_data_set_data(players_data, players_data_type, player_index,
                          data_pointers[player_index]);
    players_data_set_data_name(players_data, players_data_type, player_index,
                               data_names[player_index]);
  }
  players_data_set_is_shared(players_data, players_data_type,
                             new_data_is_shared);
  for (int player_index = 0; player_index < 2; player_index++) {
    free(data_names[player_index]);
  }
}