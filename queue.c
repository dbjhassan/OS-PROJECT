#include <string.h>
#include <pthread.h>

#include "queue.h"

int queue_init(queue *q)
{
    memset(q, 0, sizeof(queue));
    q->closing=1;

    if (pthread_mutex_init(&q->lock, NULL)!=0) 
        return -1;

    if (pthread_cond_init(&q->not_empty, NULL)!=0) 
    {
        pthread_mutex_destroy(&q->lock);
        return -1;
    }

    return 0;
}

void queue_destroy(queue *q)
{
    queue_flush(q);

    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}

void queue_open(queue *q)
{
    pthread_mutex_lock(&q->lock);

    q->head=0;
    q->tail=0;
    q->count=0;
    q->dropped=0;
    q->closing=0;

    pthread_mutex_unlock(&q->lock);
}

int queue_put(queue *q, frame *f)
{
    pthread_mutex_lock(&q->lock);

    if (q->closing) 
    {
        pthread_mutex_unlock(&q->lock);
        return -2;
    }

    if (q->count==QUEUE_CAP) 
    {
        q->dropped++;
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    q->item[q->tail]=f;
    q->tail=(q->tail + 1)%QUEUE_CAP;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);

    return 0;
}

frame *queue_get(queue *q)
{
    pthread_mutex_lock(&q->lock);

    while (q->count== 0 && !q->closing) 
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->closing) 
    {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    frame *f= q->item[q->head];

    q->head=(q->head + 1)%QUEUE_CAP;
    q->count--;

    pthread_mutex_unlock(&q->lock);

    return f;
}

void queue_close(queue *q)
{
    pthread_mutex_lock(&q->lock);

    q->closing=1;

    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

void queue_flush(queue *q)
{
    pthread_mutex_lock(&q->lock);

    while (q->count> 0) 
    {
        frame_unref(q->item[q->head]);

        q->head=(q->head + 1) % QUEUE_CAP;
        q->count--;
    }

    pthread_mutex_unlock(&q->lock);
}

int queue_dropped(queue *q)
{
    pthread_mutex_lock(&q->lock);

    int d=q->dropped;

    pthread_mutex_unlock(&q->lock);

    return d;
}
