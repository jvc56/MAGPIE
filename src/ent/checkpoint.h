#ifndef CHECKPOINT_H
#define CHECKPOINT_H

typedef struct Checkpoint Checkpoint;

typedef void (*prebroadcast_func_t)(void *);

Checkpoint *checkpoint_create(int num_threads,
                              prebroadcast_func_t prebroadcast_func);
void checkpoint_destroy(Checkpoint *checkpoint);
void checkpoint_wait(Checkpoint *checkpoint, void *data);

#endif
