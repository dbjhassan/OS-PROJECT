#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <pthread.h>
#include "common.h"
#include "protocol.h"

#define STAT_IN 0
#define STAT_OUT 1
#define STAT_DROPPED 2
#define STAT_MIRRORED 3

typedef struct {
    unsigned long frames_in;
    unsigned long frames_out;
    unsigned long frames_dropped;
    unsigned long frames_mirrored;
} port_stats_t;

typedef struct {
    pthread_mutex_t mutex;
    int num_ports;
    port_stats_t ports[MAX_PORTS];
} shm_stats_t;

int shm_stats_create(shm_stats_t **stats, int num_ports);

void shm_stats_destroy(shm_stats_t *stats);

int shm_stats_attach(shm_stats_t **stats, int *fd);

void shm_stats_detach(shm_stats_t *stats, int fd);

void shm_stats_add(shm_stats_t *stats, int port, int which);

int shm_stats_copy(shm_stats_t *stats, port_stats_t out[MAX_PORTS]);

#endif
