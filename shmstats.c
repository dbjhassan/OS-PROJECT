#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "shm_stats.h"
#include "common.h"

int shm_stats_create(shm_stats_t **stats, int num_ports)
{
    int fd=shm_open(VSW_SHM_NAME, O_CREAT | O_RDWR, 0666);

    if (fd==-1) 
    {
        perror("shm_open");
        return -1;
    }

    if (ftruncate(fd, (off_t)sizeof(shm_stats_t))==-1) 
    {
        perror("ftruncate");
        close(fd);
    
      return -1;
    }

    void *p = mmap(NULL, sizeof(shm_stats_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (p==MAP_FAILED) 
    {
        perror("mmap");
        close(fd);
        
        return -1;
    }

    if (close(fd)==-1) 
    {
        perror("close");
    }

    shm_stats_t *s=p;

    memset(s, 0, sizeof(*s));

    pthread_mutexattr_t attr;

    int rc=pthread_mutexattr_init(&attr);

    if (rc==0)
    {
        rc=pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    }

    if (rc== 0) 
    {
        rc = pthread_mutex_init(&s->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    if (rc!=0) 
    {
        fprintf(stderr, "shared mutex: %s\n", strerror(rc));
        munmap(s, sizeof(*s));
        return -1;
    }

    s->num_ports=num_ports;
    *stats=s;

    return 0;
}

void shm_stats_destroy(shm_stats_t *stats)
{
    pthread_mutex_destroy(&stats->mutex);

    if (munmap(stats, sizeof(*stats))==-1) 
        perror("munmap");
    
    if (shm_unlink(VSW_SHM_NAME) ==-1)
        perror("shm_unlink");
}

int shm_stats_attach(shm_stats_t **stats, int *fd)
{
    *fd = shm_open(VSW_SHM_NAME, O_RDWR, 0);

    if (*fd==-1) 
    {
        perror("shm_open (is the switch running?)");
        return -1;
    }

    void *p = mmap(NULL, sizeof(shm_stats_t), PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);

    if (p==MAP_FAILED) 
    {
        perror("mmap");
        close(*fd);
        return -1;
    }

    *stats=p;

    return 0;
}

void shm_stats_detach(shm_stats_t *stats, int fd)
{
    if (munmap(stats, sizeof(*stats))==-1) 
    {
        perror("munmap");
    }

    if (close(fd)==-1)
        perror("close");    
}

void shm_stats_add(shm_stats_t *stats, int port, int which)
{
    if (port<0 || port>= MAX_PORTS) 
        return;

    pthread_mutex_lock(&stats->mutex);

    switch (which) 
    {
    case STAT_IN:
        stats->ports[port].frames_in++;
        break;
    
    case STAT_OUT:
        stats->ports[port].frames_out++;
        break;
    
    case STAT_DROPPED:
        stats->ports[port].frames_dropped++;
        break;
    
    case STAT_MIRRORED:
        stats->ports[port].frames_mirrored++;
        break;
    }

    pthread_mutex_unlock(&stats->mutex);
}

int shm_stats_copy(shm_stats_t *stats, port_stats_t out[MAX_PORTS])
{
    pthread_mutex_lock(&stats->mutex);
    int n=stats->num_ports;

    memcpy(out, stats->ports, sizeof(stats->ports));

    pthread_mutex_unlock(&stats->mutex);

    return n;
}
