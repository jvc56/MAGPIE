#ifndef PROD_CON_QUEUE_H
#define PROD_CON_QUEUE_H

typedef struct ProdConQueue ProdConQueue;

ProdConQueue *prod_con_queue_create(int size);
void prod_con_queue_destroy(ProdConQueue *pcq);
void prod_con_queue_produce(ProdConQueue *pcq, void *item);
void *prod_con_queue_consume(ProdConQueue *pcq);

#endif
