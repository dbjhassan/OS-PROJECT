#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

#include "frame.h"

#define QUEUE_CAP 64

typedef struct {
    frame *item[QUEUE_CAP];
    int head;
    int tail;
    int count;
    int closing;
    int dropped;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
} queue;

int queue_init(queue *q);

void queue_destroy(queue *q);

void queue_open(queue *q);

int queue_put(queue *q, frame *f);

frame *queue_get(queue *q);

void queue_close(queue *q);

void queue_flush(queue *q);

int queue_dropped(queue *q);

#endif
