#ifndef FRAME_H
#define FRAME_H

#include <pthread.h>

#include "protocol.h"

typedef struct {
    frame_hdr h;
    unsigned char data[MAX_PAYLOAD];
    int refs;
    pthread_mutex_t lock;
} frame;

frame *newframe(const frame_hdr *h, const unsigned char *data);

void frame_ref(frame *f);

void frame_unref(frame *f);

#endif
