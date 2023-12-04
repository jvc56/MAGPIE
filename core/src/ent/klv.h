#ifndef KLV_H
#define KLV_H

struct KLV;
typedef struct KLV KLV;

KLV *create_klv(const char *klv_name);
void destroy_klv(KLV *klv);
int klv_get_word_count(const KLV *klv, int word_count_index);
float klv_get_leave_value(const KLV *klv, int leave_value_index);
const KWG *klv_get_kwg(const KLV *klv);

#endif