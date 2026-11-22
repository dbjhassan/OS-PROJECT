#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "protocol.h"

static unsigned char my_mac[6];

void *rx_thread(void *arg)
{
    int fd= *(int *)arg;
    frame_hdr h;
    unsigned char buf[MAX_PAYLOAD + 1];
    int r;

    while ((r=frame_recv(fd, &h, buf))==1) 
    {
        char s[18];
        mac_fmt(h.src, s);
        buf[h.length] = 0;

        printf("[rx] from %s vlan=%d prio=%d: %s\n", s, h.vlan, h.priority, (char *)buf);
        fflush(stdout);
    }

    printf("[host] connection closed\n");

    if (r==0) 
      exit(0);
    
    else exit(1);
}

int main(int argc, char *argv[])
{
    if (argc!=4) 
    {
        fprintf(stderr, "usage: %s <switch_ip> <tcp_port> <mac>\n", argv[0]);
        return 1;
    }

    int port =parse_port(argv[2]);

    if (port==-1) 
    {
        fprintf(stderr, "bad port\n");
        return 1;
    }

    if (mac_parse(argv[3], my_mac)==-1) 
    {
        fprintf(stderr, "bad mac\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd==-1) 
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family=AF_INET;
    address.sin_port=htons((unsigned short)port);

    if (inet_pton(AF_INET, argv[1], &address.sin_addr)!=1) 
    {
        fprintf(stderr, "bad ip\n");
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&address, sizeof(address))==-1) 
    {
        perror("connect");
        close(fd);
        return 1;
    }

    pthread_t tid;

    int rc = pthread_create(&tid, NULL, rx_thread, &fd);

    if (rc!=0) 
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(rc));
        close(fd);
        return 1;
    }

    rc = pthread_detach(tid);

    if (rc!=0) 
    {
        fprintf(stderr, "pthread_detach: %s\n", strerror(rc));
        close(fd);
        return 1;
    }

    char line[2048];

    while (fgets(line, sizeof(line), stdin) != NULL) 
    {
        line[strcspn(line, "\n")] = '\0';
        char *space = strchr(line, ' ');

        if (space == NULL) 
        {
            fprintf(stderr, "format: <dst_mac|bcast> <text>\n");
            continue;
        }

        *space = '\0';
        space++;

        frame_hdr h;
        memset(&h, 0, sizeof(h));

        h.vlan = 1;
        h.priority = NP;
        memcpy(h.src, my_mac, 6);

        if (strcmp(line, "bcast")==0) 
            memset(h.dst, 0xFF, 6);
        else if (mac_parse(line, h.dst)==-1) 
        {
            fprintf(stderr, "bad dst mac\n");
            continue;
        }

        size_t n = strlen(space);

        if (n>MAX_PAYLOAD) 
            n=MAX_PAYLOAD;

        h.length = (int)n;

        if (frame_send(fd, &h, (unsigned char *)space) == -1) 
        {
            perror("send");
            break;
        }
    }

    if (close(fd)==-1) 
    {
        perror("close");
        return 1;
    }

    return 0;
}
