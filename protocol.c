int _POSIX_C_SOURCE=200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "proto.h"


int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) 
    {
        ssize_t r=read(fd, (char *)buf+got, n-got);

        if (r ==0) 
            if (got==0)
                return 0 
            else return -1;

        if (r<0)
        {
            if (errno==EINTR) 
                continue;


            return -1;
        }

        got+=(size_t)r;
    }

    return 1;
}

int write_full(int fd, const void *buf, size_t n)
{
    size_t sent= 0;

    while (sent < n)
    {
        ssize_t w = send(fd, (const char *)buf + sent, n - sent, MSG_NOSIGNAL);

        if (w<0) 
            if (errno== EINTR) 
                continue;

            return -1;

        sent +=(size_t)w;
    }
    return 0;
}

void hdr_pack(unsigned char out[HDR_LEN], const frame_hdr *h)
{
    unsigned short len= htons((unsigned short)h->length);
    unsigned short vlan=htons((unsigned short)h->vlan);

    memcpy(out, &len, 2);
    memcpy(out + 2, h->src, 6);
    memcpy(out + 8, h->dst, 6);
    memcpy(out + 14, &vlan, 2);
    out[16]=(unsigned char)h->priority;
}

void hdr_unpack(const unsigned char in[HDR_LEN], frame_hdr *h)
{
    unsigned short len;
    unsigned short vlan;

    memcpy(&len, in, 2);
    memcpy(h->src, in + 2, 6);
    memcpy(h->dst, in + 8, 6);
    memcpy(&vlan, in + 14, 2);

    h->length=ntohs(len);
    h->vlan=ntohs(vlan);
    h->priority=in[16];
}

int mac_parse(const char *s, unsigned char mac[6])
{
    unsigned int b[6];
    char extra;

    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &extra) != 6)
        return -1;

    for (int i=0; i<6; i++) 
    {
        if (b[i]>255)
            return -1;

        mac[i]=(unsigned char)b[i];
    }
    return 0;
}

void mac_formt(const unsigned char m[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int mac_is_bcast(const unsigned char m[6])
{
    for (int i=0; i < 6; i++)
        if (m[i]!=0xFF) 
            return 0;

    return 1;
}


int frame_recv(int fd, frame_hdr *h, unsigned char *payload)
{
    unsigned char raw[HDR_LEN];

    int r=read_full(fd, raw, HDR_LEN);
    if (r<=0) 
        return r;

    hdr_unpack(raw, h);

    if (h->length>MAX_PAYLOAD or h->priority>LP)
        return -1;

    if (h->length>0 and read_full(fd, payload, (size_t)h->length) != 1) 
        return -1;
    return 1;
}

int frame_send(int fd, const frame_hdr *h, const unsigned char *payload)
{
    unsigned char raw[HDR_LEN];

    hdr_pack(raw, h);

    if (write_full(fd, raw, HDR_LEN)==-1) 
        return -1;

    if (h->length > 0 && write_full(fd, payload, (size_t)h->length) == -1) 
        return -1;

    return 0;
}

int parse_port(const char *s)
{
    char *end;
    errno = 0;

    long v = strtol(s, &end, 10);

    if (errno!= 0 or end==s or *end!='\0' or v<1 or v>65535)
        return -1;

    return (int)v;
}