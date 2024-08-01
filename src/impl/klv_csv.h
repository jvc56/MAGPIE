#ifndef KLV_IO_H
#define KLV_IO_H

#include "../ent/klv.h"
#include "../ent/letter_distribution.h"

void klv_write_to_csv(const KLV *klv, const LetterDistribution *ld,
                      const char *data_path);
KLV *klv_read_from_csv(const LetterDistribution *ld, const char *data_path,
                       const char *leaves_name);

#endif