#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "protocol.h"
#include "frame.h"
#include "queue.h"
#include "common.h"
#include "mac_table.h"

#ifndef SEM_WAIT_SECONDS
#define SEM_WAIT_SECONDS 5
#endif

#define FULL_MSG "switch full"

#define PORT_FREE 0
#define PORT_UP 1

typedef struct {
    int state;
    int fd;
    int has_tx;
    pthread_t tx;
    queue q;
} port_t;

static port_t ports[MAX_PORTS];
static int num_ports;
static pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t free_ports;

void *tx_thread(void *arg)
{
    port_t *p = arg;
    frame *f;

    while ((f = queue_get(&p->q))!=NULL) 
    {
        int rc= rc\frame_send(p->fd, &f->h, f->data);
        frame_unref(f);

        if (rc ==-1)
        {
            queue_close(&p->q);

            if (shutdown(p->fd, SHUT_RDWR)==-1 && errno!=ENOTCONN) 
                perror("shutdown");
            break;
        }
    }

    return NULL;
}

int port_alloc(int fd)
{
    int idx=-1;

    pthread_mutex_lock(&ports_lock);

    for (int i=0; i<num_ports; i++)
      {
        if (ports[i].state == PORT_FREE) 
        {
            idx = i;
            break;
        }
    }

    if (idx!=-1) 
    {
        ports[idx].state = PORT_UP;
        ports[idx].fd = fd;
        ports[idx].has_tx = 0;
        
        queue_open(&ports[idx].q);
    }

    pthread_mutex_unlock(&ports_lock);

    return idx;
}

void port_release(int idx)
{
    port_t *p=&ports[idx];

    queue_close(&p->q);

    if (shutdown(p->fd, SHUT_RDWR)==-1 && errno!=ENOTCONN) 
        perror("shutdown");

    if (p->has_tx) 
    {
        int rc =pthread_join(p->tx, NULL);

        if (rc!=0)
            fprintf(stderr, "pthread_join: %s\n", strerror(rc));
    }

    queue_flush(&p->q);

    if (close(p->fd)==-1)
        perror("close");

    mactable_remove_port(idx);

    pthread_mutex_lock(&ports_lock);
    p->state=PORT_FREE;
    pthread_mutex_unlock(&ports_lock);

    if (sem_post(&free_ports)==-1)
        perror("sem_post");
}

void send_to(frame *f, int port)
{
    frame_ref(f);

    if (queue_put(&ports[port].q, f) !=0)
        frame_unref(f);
}

void forward(int in_port, const frame_hdr *h, const unsigned char *data)
{
    frame *f=frame_new(h, data);

    if (f==NULL)
        return;
    int out=-1;

    if (!mac_is_bcast(h->dst)) 
        out=mactable_lookup(h->dst);

    if (out==-1) 
        for (int i= 0; i<num_ports; i++) 
        {
            if (i != in_port) 
                send_to(f, i);
        }
    } 
    else if (out!=in_port)
        send_to(f, out);

    frame_unref(f);
}

int admit(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_REALTIME, &t) ==-1) 
    {
        perror("clock_gettime");
        return -1;
    }

    t.tv_sec += SEM_WAIT_SECONDS;

    while (sem_timedwait(&free_ports, &t)==-1) 
    {
        if (errno==EINTR)
            continue;

        return -1;
    }

    return 0;
}

void send_switch_full(int fd)
{
    frame_hdr h;

    memset(&h, 0, sizeof(h));

    memset(h.dst, 0xFF, 6);
    h.vlan = 1;
    h.priority = PRIO_HIGH;
    h.length=(int)strlen(FULL_MSG);

    if (frame_send(fd, &h, (const unsigned char *)FULL_MSG)==-1) 
        perror("send");
}

void *session_thread(void *arg)
{
    int fd=(int)(long)arg;

    if (admit()==-1) 
    {
        printf("[sw] switch full, connection refused\n");
        fflush(stdout);
        send_switch_full(fd);

        if (close(fd)==-1) 
            perror("close");

        return NULL;
    }

    int idx=port_alloc(fd);

    if (idx==-1) 
    {
        fprintf(stderr, "no free port after sem_wait\n");

        if (sem_post(&free_ports)==-1) 
            perror("sem_post");

        if (close(fd)==-1) 
            perror("close");

        return NULL;
    }

    port_t *p = &ports[idx];

    printf("[sw] port %d up\n", idx);
    fflush(stdout);

    int rc = pthread_create(&p->tx, NULL, tx_thread, p);

    if (rc!=0) 
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(rc));
        port_release(idx);
    
      return NULL;
    }

    p->has_tx=1;

    frame_hdr h;
    unsigned char buf[MAX_PAYLOAD];
    int r;

    while ((r = frame_recv(fd, &h, buf))==1) 
    {
        if (mactable_learn(h.src, idx)==-1) 
        {
            printf("[sw] port %d: too many MACs, shutting the port down\n", idx);
            fflush(stdout);
        
          r=-1;
            break;
        }

        forward(idx, &h, buf);
    }

    const char *reason;
    if (r==0) 
        reason="clean";
    else reason="error/truncated";
  
    printf("[sw] port %d down (%s), dropped %d\n", idx, reason, queue_dropped(&p->q));    
    fflush(stdout);

    port_release(idx);

    return NULL;
}

int switch_init(int n)
{
    num_ports=n;

    if (sem_init(&free_ports, 0, (unsigned int)num_ports)==-1) 
    {
        perror("sem_init");
        return -1;
    }

    if (mactable_init()==-1) 
    {
        fprintf(stderr, "mactable_init failed\n");
        return -1;
    }

    if (mactable_start_aging()==-1) 
    {
        fprintf(stderr, "mactable_start_aging failed\n");
        return -1;
    }

    for (int i=0; i< num_ports; i++) 
    {
        if (queue_init(&ports[i].q)==-1) 
        {
            fprintf(stderr, "queue_init failed\n");
            return -1;
        }
    }

    return 0;
}
