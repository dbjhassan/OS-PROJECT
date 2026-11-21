#include "mactable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned hash_mac(const uint8_t m[6])
{
    uint32_t h = 2166136261u;

    for (int i = 0; i < 6; i++)
    {
        h ^= m[i];
        h *= 16777619u;
    }
    return h % MAC_BUCKETS;
}

void mac_table_init(mac_table_t *t, int max_entries, int max_per_port)
{
    t->max_entries = max_entries;
    t->max_per_port = max_per_port;
    atomic_init(&t->total, 0);
    
    for (int i = 0; i < MAC_MAX_PORTS; i++)
        atomic_init(&t->per_port[i], 0);
    
        for (unsigned i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_init(&t->bucket[i].lock, NULL);
        t->bucket[i].head = NULL;
    }
}

void mac_table_destroy(mac_table_t *t)
{
    mac_table_clear(t);
    for (unsigned i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_destroy(&t->bucket[i].lock);
}

static void evict_oldest(mac_table_t *t, mac_bucket_t *b)
{
    mac_entry_t **pp;
    mac_entry_t **victim = NULL;
    mac_entry_t *e;

    for (pp = &b->head; *pp; pp = &(*pp)->next)
        if (!victim or (*pp)->last_seen < (*victim)->last_seen)
            victim = pp;
    
    if (!victim)
        return;

    e = *victim;
    *victim = e->next;

    atomic_fetch_sub(&t->per_port[e->port], 1);
    atomic_fetch_sub(&t->total, 1);
    free(e);
}

int mac_table_learn(mac_table_t *t, const uint8_t mac[6], int port)
{
    mac_bucket_t *b;
    mac_entry_t *e;
    int rc = LEARN_OK;
    time_t now = time(NULL);

    if (mac[0] & 1)
        return LEARN_OK;

    if (port < 0 || port >= MAC_MAX_PORTS)
        return LEARN_OK;

    b = &t->bucket[hash_mac(mac)];
    
    pthread_mutex_lock(&b->lock);
    
    for (e = b->head; e; e = e->next)
        if (memcmp(e->mac, mac, 6) == 0)
            break;

    if (e)
    {
        if (e->port!= port)
        {
            if (atomic_load(&t->per_port[port]) >= t->max_per_port)
            {
                rc = LEARN_PORT_FULL;
                goto out;
            }

            atomic_fetch_sub(&t->per_port[e->port], 1);
            atomic_fetch_add(&t->per_port[port], 1);
            
            e->port = port;
            rc = LEARN_MOVED;
        }
        e->last_seen = now;
    }


    else
    {
        if (atomic_load(&t->per_port[port]) >= t->max_per_port)
        {
            rc = LEARN_PORT_FULL;
            goto out;
        }
    
        if (atomic_load(&t->total) >= t->max_entries)
            evict_oldest(t, b);
    
        if (atomic_load(&t->total) >= t->max_entries)
            goto out; /* empty bucket, nothing to evict: skip */

        e = calloc(1, sizeof *e);

        if (!e)
            goto out;

        memcpy(e->mac, mac, 6);

        e->port = port;
        e->last_seen = now;
        e->next = b->head;
        b->head = e;

        atomic_fetch_add(&t->per_port[port], 1);
        atomic_fetch_add(&t->total, 1);
    }

out:
    pthread_mutex_unlock(&b->lock);
    return rc;
}

int mac_table_lookup(mac_table_t *t, const uint8_t mac[6])
{
    mac_bucket_t *b = &t->bucket[hash_mac(mac)];
    int port = -1;

    pthread_mutex_lock(&b->lock);
    for (mac_entry_t *e = b->head; e; e = e->next)
    {
        if (memcmp(e->mac, mac, 6) == 0)
        {
            port = e->port;
            break;
        }
    }
    pthread_mutex_unlock(&b->lock);
    return port;
}

size_t mac_table_age(mac_table_t *t, time_t max_age)
{
    size_t n = 0;
    time_t now = time(NULL);

    for (unsigned i = 0; i < MAC_BUCKETS; i++)
    {
        mac_bucket_t *b = &t->bucket[i];
        mac_entry_t **pp;

        pthread_mutex_lock(&b->lock);
        pp = &b->head;
        
        while (*pp)
        {
            mac_entry_t *e = *pp;

            if (now - e->last_seen > max_age)
            {
                *pp = e->next;
                atomic_fetch_sub(&t->per_port[e->port], 1);
                atomic_fetch_sub(&t->total, 1);
                free(e);
                n++;
            }
        
            else
                pp = &e->next;
        }
        pthread_mutex_unlock(&b->lock);
    }
    return n;
}

size_t mac_table_clear(mac_table_t *t)
{
    size_t removed = 0;
    for (unsigned i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_lock(&t->bucket[i].lock);

    for (unsigned i = 0; i < MAC_BUCKETS; i++)
    {
        mac_entry_t *e = t->bucket[i].head;

        while (e)
        {
            mac_entry_t *next = e->next;

            atomic_fetch_sub(&t->per_port[e->port], 1);
            atomic_fetch_sub(&t->total, 1);
            free(e);
            removed++;
            e = next;
        }
        t->bucket[i].head = NULL;
    }

    for (unsigned i = MAC_BUCKETS; i-- > 0;)
        pthread_mutex_unlock(&t->bucket[i].lock);
    
    return removed;
}

mac_view_t *mac_table_snapshot(mac_table_t *t, size_t *n)
{
    size_t cap = 64;
    size_t len = 0;
    mac_view_t *v = malloc(cap * sizeof *v);

    *n = 0;
    if (!v)
        return NULL;

    for (unsigned i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&t->bucket[i].lock);
        for (mac_entry_t *e = t->bucket[i].head; e; e = e->next)
        {
            if (len == cap)
            {
                mac_view_t *nv = realloc(v, cap * 2 * sizeof *v);

                if (!nv)
                {
                    pthread_mutex_unlock(&t->bucket[i].lock);
                    free(v);
                    return NULL;
                }
                v = nv;
                cap *= 2;
            }

            memcpy(v[len].mac, e->mac, 6);
           
            v[len].port = e->port;
            v[len].last_seen = e->last_seen;
            v[len].bucket = i;
            len++;

        }
        pthread_mutex_unlock(&t->bucket[i].lock);
    }
    *n = len;
    return v;

}

void mac_format(const uint8_t m[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}
