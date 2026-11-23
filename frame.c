#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "frame.h"

frame *frame_new(const frame_hdr *h, const unsigned char *data)
{
    frame *f = malloc(sizeof(frame));
    
    if (f == NULL)
        return NULL;

    if (pthread_mutex_init(&f->lock, NULL) != 0) 
    {
        free(f);
        return NULL;
    }

    f->h = *h;
    f->refs = 1;

    if (h->length > 0) 
        memcpy(f->data, data, (size_t)h->length);

    return f;
}

void frame_ref(frame *f)
{
    pthread_mutex_lock(&f->lock);
    f->refs++;
    pthread_mutex_unlock(&f->lock);
}

void frame_unref(frame *f)
{
    pthread_mutex_lock(&f->lock);
    f->refs--;
    int left = f->refs;
    pthread_mutex_unlock(&f->lock);

    if (left == 0) {
        pthread_mutex_destroy(&f->lock);
        free(f);
    }
}
