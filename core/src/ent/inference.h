#ifndef INFERENCE_H
#define INFERENCE_H

struct Inference;
typedef struct Inference Inference;

Inference *create_inference();
void destroy_inference(Inference *inference);

#endif