#ifndef KLV_H
#define KLV_H

struct KLV;
typedef struct KLV KLV;

KLV *create_klv(const char *klv_name);
void destroy_klv(KLV *klv);

#endif