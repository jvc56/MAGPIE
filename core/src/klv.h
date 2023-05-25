#ifndef KLV_H
#define KLV_H

#include <stdint.h>

#include "kwg.h"
#include "rack.h"

typedef struct KLV {
    KWG * kwg;
    int * word_counts;
    float * leave_values;
} KLV;

KLV * create_klv(const char* klv_filename);
void destroy_klv(KLV * klv);
float leave_value(KLV * klv, Rack * rack);

#endif