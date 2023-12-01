#ifndef SIMMER_H
#define SIMMER_H

struct Simmer;
typedef struct Simmer Simmer;

Simmer *create_simmer(const Config *config);
void destroy_simmer(Simmer *simmer);

#endif
