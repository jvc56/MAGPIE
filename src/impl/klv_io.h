#ifndef KLV_IO_H
#define KLV_IO_H

#include "../ent/klv.h"
#include "../ent/letter_distribution.h"

void klv_write(const KLV *klv, const LetterDistribution *ld,
               const char *filepath);

#endif