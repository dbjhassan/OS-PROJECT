#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

#include "logger.h"

#define LOG_CAP 1024
#define LOG_LEN 256

typedef struct {
    struct timespec ts;
    char text[LOG_LEN];
} log_entry;

static log_entry entries[LOG_CAP];
static int head=0;
static int tail=0;
static int count=0;
static int lost=0;
static int stopping=0;
static int running=0;

static FILE *log_file=NULL;
static pthread_t log_tid;
static pthread_mutex_t log_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_cond=PTHREAD_COND_INITIALIZER;

void write_line(const struct timespec *ts, const char *text)
{
    struct tm tm;
    char stamp[32];

    time_t secs=ts->tv_sec;

    if (localtime_r(&secs, &tm) == NULL) 
    { 
      fprintf(log_file, "%s\n", text);
        return;
    }

    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(log_file, "%s.%03ld %s\n", stamp, ts->tv_nsec/1000000, text);
}

void *logger_thread(void *arg)
{
    (void)arg;

    while (1) 
    {
        log_entry e;
        int have=0;
        int lost_now;
        int empty;

        pthread_mutex_lock(&log_lock);

        while (count==0 && lost==0 && !stopping) 
            pthread_cond_wait(&log_cond, &log_lock);

        if (count==0 && lost==0)
        {
            pthread_mutex_unlock(&log_lock);
            break;
        }
        
        lost_now=lost;
        lost=0;

        if (count>0) 
        {
            e=entries[head];
            head=(head+1)%LOG_CAP;
            count--;
            have=1;
        }

        empty=(count==0);

        pthread_mutex_unlock(&log_lock);

        if (lost_now>0) 
        {
            struct timespec now;
            char text[LOG_LEN];

            if (clock_gettime(CLOCK_REALTIME, &now) == -1) 
                memset(&now, 0, sizeof(now));

            snprintf(text, sizeof(text), "log queue full, %d messages lost", lost_now);
            write_line(&now, text);
        }

        if (have) 
            write_line(&e.ts, e.text);

        if (empty)
            fflush(log_file);
    }

    fflush(log_file);

    return NULL;
}

int logger_start(const char *path)
{
    log_file=fopen(path, "a");

    if (log_file==NULL) 
    {
        perror("fopen log");
        return -1;
    }

    stopping=0;
    running=1;

    int rc=pthread_create(&log_tid, NULL, logger_thread, NULL);

    if (rc!=0) 
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(rc));
        running=0;
    
        fclose(log_file);
        log_file=NULL;
        return -1;
    }

    return 0;
}

void logger_stop(void)
{
    if (log_file==NULL) 
        return;

    pthread_mutex_lock(&log_lock);
    stopping=1;
    pthread_cond_broadcast(&log_cond);
    pthread_mutex_unlock(&log_lock);

    pthread_join(log_tid, NULL);

    pthread_mutex_lock(&log_lock);
    running = 0;
    pthread_mutex_unlock(&log_lock);

    if (fclose(log_file)==EOF) 
        perror("fclose log");

    log_file = NULL;
}

void log_msg(const char *fmt, ...)
{
    log_entry e;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(e.text, LOG_LEN, fmt, ap);
    va_end(ap);

    if (clock_gettime(CLOCK_REALTIME, &e.ts) == -1) 
        memset(&e.ts, 0, sizeof(e.ts));

    pthread_mutex_lock(&log_lock);

    if (!running) 
    {
        pthread_mutex_unlock(&log_lock);
        fprintf(stderr, "%s\n", e.text);
        return;
    }

    if (count==LOG_CAP) 
    {
        lost++;
        pthread_cond_signal(&log_cond);
        pthread_mutex_unlock(&log_lock);
        return;
    }

    entries[tail]=e;
    tail=(tail+1)%LOG_CAP;
    count++;

    pthread_cond_signal(&log_cond);
    pthread_mutex_unlock(&log_lock);
}
