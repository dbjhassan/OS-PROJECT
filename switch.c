#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#include "protocol.h"
#include "frame.h"
#include "queue.h"
#include "common.h"
#include "mactable.h"
#include "shmstats.h"
#include "logger.h"
#include "switch.h"

#ifndef SEM_WAIT_SECONDS
#define SEM_WAIT_SECONDS 5
#endif

#define FULL_MSG "switch full"

#define PORT_FREE 0
#define PORT_UP 1
#define PORT_CLOSING 2

typedef struct {
    int idx;
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
static int admin_start(void);
static void admin_stop(void);
static shm_stats_t *stats;
static atomic_int stopping;

void *tx_thread(void *arg)
{
    port_t *p = arg;
    frame *f;

    while ((f = queue_get(&p->q))!=NULL) 
    {
        int rc= frame_send(p->fd, &f->h, f->data);
        frame_unref(f);

        if (rc ==0)
            shm_stats_add(stats, p->idx, STAT_OUT);

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

    if (atomic_load(&stopping))
    {
        pthread_mutex_unlock(&ports_lock);
        return -1;
    }

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
        ports[idx].idx = idx;
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

    pthread_mutex_lock(&ports_lock);
    p->state=PORT_CLOSING;
    pthread_mutex_unlock(&ports_lock);

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
    int rc;

    frame_ref(f);
    rc=queue_put(&ports[port].q, f);

    if (rc !=0)
    {
        frame_unref(f);

        if (rc==-1)
            shm_stats_add(stats, port, STAT_DROPPED);
    }
}

void forward(int in_port, const frame_hdr *h, const unsigned char *data)
{
    frame *f=frame_new(h, data);
    int out=-1;

    if (f==NULL)
        return;

    if (!mac_is_bcast(h->dst)) 
        out=mactable_lookup(h->dst);

    if (out==-1) 
    {
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

    if (atomic_load(&stopping))
        return -1;

    return 0;
}

void send_switch_full(int fd)
{
    frame_hdr h;

    memset(&h, 0, sizeof(h));

    memset(h.dst, 0xFF, 6);
    h.vlan = 1;
    h.priority = HP;
    h.length=(int)strlen(FULL_MSG);

    if (frame_send(fd, &h, (const unsigned char *)FULL_MSG)==-1) 
        perror("send");
}

void *session_thread(void *arg)
{
    int fd=(int)(long)arg;

    if (admit()==-1) 
    {
        if (!atomic_load(&stopping))
        {
            printf("[sw] switch full, connection refused\n");
            fflush(stdout);
            send_switch_full(fd);
        }

        if (close(fd)==-1) 
            perror("close");

        return NULL;
    }

    int idx=port_alloc(fd);

    if (idx==-1) 
    {
        if (!atomic_load(&stopping))
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
        shm_stats_add(stats, idx, STAT_IN);

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

    if (shm_stats_create(&stats, num_ports)==-1) 
    {
        fprintf(stderr, "shm_stats_create failed\n");
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

    if (admin_start()==-1) 
    {
        fprintf(stderr, "admin_start failed\n");
        return -1;
    }

    return 0;
}


static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t active_cond = PTHREAD_COND_INITIALIZER;
static int active_sessions = 0;

static void *session_entry(void *arg)
{
    session_thread(arg);

    pthread_mutex_lock(&active_lock);
    active_sessions--;
    pthread_cond_broadcast(&active_cond);
    pthread_mutex_unlock(&active_lock);

    return NULL;
}

int session_spawn(int fd)
{
    pthread_t tid;
    int rc;

    pthread_mutex_lock(&active_lock);
    active_sessions++;
    pthread_mutex_unlock(&active_lock);

    rc = pthread_create(&tid, NULL, session_entry, (void *)(long)fd);

    if (rc != 0)
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(rc));
        pthread_mutex_lock(&active_lock);
        active_sessions--;
        pthread_mutex_unlock(&active_lock);
        return -1;
    }

    pthread_detach(tid);
    return 0;
}

int switch_dump_mac(const char *path)
{
    size_t n;
    mac_view_t *v = mactable_snapshot(&n);
    FILE *f;

    if (v == NULL)
        return -1;

    f = fopen(path, "w");

    if (f == NULL)
    {
        free(v);
        return -1;
    }

    fprintf(f, "MAC                PORT  LAST_SEEN\n");

    for (size_t i = 0; i < n; i++)
    {
        char s[18];

        mac_format(v[i].mac, s);
        fprintf(f, "%s  %4d  %ld\n", s, v[i].port, (long)v[i].last_seen);
    }

    free(v);

    if (fclose(f) == EOF)
        return -1;

    return 0;
}

/* ---- admin socket (talks to swctl) ---- */

static int admin_fd = -1;
static int admin_pipe[2] = {-1, -1};
static pthread_t admin_tid;
static int admin_running = 0;

static void admin_say(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void admin_say(int fd, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    write_full(fd, buf, strlen(buf));
}

static void admin_handle(int fd, const char *cmd)
{
    if (strcmp(cmd, "show mac") == 0)
    {
        size_t n;
        mac_view_t *v = mactable_snapshot(&n);

        if (v == NULL)
        {
            admin_say(fd, "error: out of memory\n");
            return;
        }

        admin_say(fd, "MAC                PORT  AGE(s)\n");

        for (size_t i = 0; i < n; i++)
        {
            char s[18];

            mac_format(v[i].mac, s);
            admin_say(fd, "%s  %4d  %ld\n", s, v[i].port, (long)(time(NULL) - v[i].last_seen));
        }

        free(v);
    }
    else if (strcmp(cmd, "show ports") == 0)
    {
        admin_say(fd, "PORT  STATE    DROPPED\n");

        for (int i = 0; i < num_ports; i++)
        {
            int st;
            const char *name;

            pthread_mutex_lock(&ports_lock);
            st = ports[i].state;
            pthread_mutex_unlock(&ports_lock);

            if (st == PORT_UP)
                name = "up";
            else if (st == PORT_CLOSING)
                name = "closing";
            else
                name = "free";

            admin_say(fd, "%4d  %-7s  %d\n", i, name, queue_dropped(&ports[i].q));
        }
    }
    else if (strcmp(cmd, "clear mac") == 0)
        admin_say(fd, "cleared %zu entries\n", mactable_clear());
    else
        admin_say(fd, "unsupported command: %s\n", cmd);
}

static void *admin_thread(void *arg)
{
    struct pollfd pfd[2];

    (void)arg;

    pfd[0].fd = admin_fd;
    pfd[0].events = POLLIN;
    pfd[1].fd = admin_pipe[0];
    pfd[1].events = POLLIN;

    while (1)
    {
        int c;
        char line[256];
        size_t len = 0;
        struct timeval tv;

        if (poll(pfd, 2, -1) == -1)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pfd[1].revents & POLLIN)
            break;

        if (!(pfd[0].revents & POLLIN))
            continue;

        c = accept(admin_fd, NULL, NULL);

        if (c == -1)
            continue;

        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (len < sizeof(line) - 1)
        {
            char ch;
            ssize_t r = recv(c, &ch, 1, 0);

            if (r <= 0 || ch == '\n')
                break;

            line[len++] = ch;
        }

        line[len] = '\0';

        if (len > 0)
            admin_handle(c, line);

        close(c);
    }

    return NULL;
}

static int admin_start(void)
{
    struct sockaddr_un a;

    admin_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (admin_fd == -1)
    {
        perror("socket");
        return -1;
    }

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, VSW_SOCK_PATH, sizeof(a.sun_path) - 1);
    unlink(VSW_SOCK_PATH);

    if (bind(admin_fd, (struct sockaddr *)&a, sizeof(a)) == -1 || listen(admin_fd, 8) == -1)
    {
        perror("admin bind/listen");
        close(admin_fd);
        admin_fd = -1;
        return -1;
    }

    if (pipe(admin_pipe) == -1)
    {
        perror("pipe");
        close(admin_fd);
        admin_fd = -1;
        return -1;
    }

    if (pthread_create(&admin_tid, NULL, admin_thread, NULL) != 0)
    {
        fprintf(stderr, "admin pthread_create failed\n");
        return -1;
    }

    admin_running = 1;
    return 0;
}

static void admin_stop(void)
{
    if (!admin_running)
        return;

    if (write(admin_pipe[1], "x", 1) == -1)
        perror("write");

    pthread_join(admin_tid, NULL);
    admin_running = 0;

    close(admin_fd);
    close(admin_pipe[0]);
    close(admin_pipe[1]);
    unlink(VSW_SOCK_PATH);
}

/* ---- shutdown: every thread joined, every fd closed, all memory freed ---- */

void switch_shutdown(void)
{
    int waiting;

    pthread_mutex_lock(&ports_lock);
    atomic_store(&stopping, 1);

    for (int i = 0; i < num_ports; i++)
    {
        if (ports[i].state == PORT_UP)
        {
            if (shutdown(ports[i].fd, SHUT_RDWR) == -1 && errno != ENOTCONN)
                perror("shutdown");
        }
    }
    pthread_mutex_unlock(&ports_lock);

    /* wake sessions that are still waiting for a free port */
    pthread_mutex_lock(&active_lock);
    waiting = active_sessions;
    pthread_mutex_unlock(&active_lock);

    for (int i = 0; i < waiting; i++)
        sem_post(&free_ports);

    pthread_mutex_lock(&active_lock);
    while (active_sessions > 0)
        pthread_cond_wait(&active_cond, &active_lock);
    pthread_mutex_unlock(&active_lock);

    admin_stop();
    mactable_stop_aging();

    for (int i = 0; i < num_ports; i++)
        queue_destroy(&ports[i].q);

    mactable_destroy();
    sem_destroy(&free_ports);

    if (stats != NULL)
        shm_stats_destroy(stats);
}
