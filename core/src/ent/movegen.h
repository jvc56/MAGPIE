#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "../def/movegen_defs.h"

#include "anchor.h"
#include "bag.h"
#include "board.h"
#include "config.h"
#include "constants.h"
#include "klv.h"
#include "kwg.h"
#include "leave_map.h"
#include "letter_distribution.h"
#include "move.h"
#include "player.h"
#include "rack.h"

struct Generator;
typedef struct Generator Generator;

Generator *create_generator(const Config *config, int move_list_capacity);
Generator *generate_duplicate(const Generator *gen, int move_list_capacity);
void destroy_generator(Generator *gen);
void update_generator(const Config *config, Generator *gen);
void generate_moves(const Rack *opp_rack, Generator *gen, Player *player,
                    bool add_exchange, move_record_t move_record_type,
                    move_sort_t move_sort_type,
                    bool apply_placement_adjustment);

#endif