#ifndef MACTABLE_H
#define MACTABLE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MAC_BUCKETS 64
#define MAC_MAX_PORTS 64
#define MAC_DEFAULT_ENTRIES 1024
#define MAC_DEFAULT_PER_PORT 16
#define MAC_DEFAULT_AGE 30


enum {
    LEARN_OK = 0,
    LEARN_MOVED = 1,
    LEARN_PORT_FULL = -1
};

typedef struct mac_entry {
    uint8_t mac[6];
    int port;
    time_t last_seen;
    struct mac_entry *next;
} mac_entry_t;

typedef struct {
    pthread_mutex_t lock;
    mac_entry_t *head;
} mac_bucket_t;

typedef struct {
    mac_bucket_t bucket[MAC_BUCKETS];
    atomic_int total;
    atomic_int per_port[MAC_MAX_PORTS];
    int max_entries;
    int max_per_port;
} mac_table_t;

/* Copy of an entry, safe to use after the bucket lock is released. */
typedef struct {
    uint8_t mac[6];
    int port;
    time_t last_seen;
    unsigned bucket;
} mac_view_t;

void mac_table_init(mac_table_t *t, int max_entries, int max_per_port);
void mac_table_destroy(mac_table_t *t);

int mac_table_learn(mac_table_t *t, const uint8_t mac[6], int port);

int mac_table_lookup(mac_table_t *t, const uint8_t mac[6]);

size_t mac_table_age(mac_table_t *t, time_t max_age);

size_t mac_table_clear(mac_table_t *t);

mac_view_t *mac_table_snapshot(mac_table_t *t, size_t *n);

void mac_format(const uint8_t mac[6], char out[18]);


int mac_table_start_aging(mac_table_t *t, time_t max_age);

void mac_table_stop_aging(void);

void mac_table_remove_port(mac_table_t *t, int port);

int mactable_init(void);

void mactable_destroy(void);

void mactable_set_limits(int entries, int per_port, int age);

int mactable_learn(const unsigned char mac[6], int port);

int mactable_lookup(const unsigned char mac[6]);

void mactable_remove_port(int port);

size_t mactable_clear(void);

mac_view_t *mactable_snapshot(size_t *n);

int mactable_start_aging(void);

void mactable_stop_aging(void);

#endif
