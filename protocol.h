#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define MAX_PAYLOAD 1500
#define HDR_LEN 17
#define HP 0
#define NP 1
#define LP 2

typedef struct {
    int length;
    unsigned char src[6];
    unsigned char dst[6];
    int vlan;
    int priority;
} frame_hdr;

int read_full(int fd, void *buf, size_t n);
int write_full(int fd, const void *buf, size_t n);

void hdr_pack(unsigned char out[HDR_LEN], const frame_hdr *h);
void hdr_unpack(const unsigned char in[HDR_LEN], frame_hdr *h);

int mac_parse(const char *s, unsigned char mac[6]);
void mac_fmt(const unsigned char mac[6], char out[18]);
int mac_is_bcast(const unsigned char mac[6]);

int frame_recv(int fd, frame_hdr *h, unsigned char *payload);
int frame_send(int fd, const frame_hdr *h, const unsigned char *payload);

int parse_port(const char *s);

#endif
