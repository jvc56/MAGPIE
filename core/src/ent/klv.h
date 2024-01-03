#ifndef KLV_H
#define KLV_H

#include "kwg.h"
#include "rack.h"

struct KLV;
typedef struct KLV KLV;

KLV *klv_create(const char *klv_name);
void klv_destroy(KLV *klv);
int klv_get_word_count(const KLV *klv, int word_count_index);
double klv_get_leave_value(const KLV *klv, const Rack *leave);
const KWG *klv_get_kwg(const KLV *klv);

#endif