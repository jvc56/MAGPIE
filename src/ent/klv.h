#ifndef KLV_H
#define KLV_H

#include "rack.h"

typedef struct KLV KLV;

KLV *klv_create(const char *klv_name);
void klv_destroy(KLV *klv);
double klv_get_leave_value(const KLV *klv, const Rack *leave);

#endif