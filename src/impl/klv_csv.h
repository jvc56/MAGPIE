#ifndef KLV_CSV_H
#define KLV_CSV_H

#include "../ent/klv.h"
#include "../ent/letter_distribution.h"

#include "../util/error_stack.h"

void klv_write_to_csv(KLV *klv, const LetterDistribution *ld,
                      const char *csv_filename, ErrorStack *error_stack);
KLV *klv_read_from_csv(const LetterDistribution *ld, const char *data_paths,
                       const char *leaves_name, ErrorStack *error_stack);
KLV *klv_create_empty(const LetterDistribution *ld, const char *name);

#endif