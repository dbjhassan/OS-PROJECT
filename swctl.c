#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include "shmstats.h"
#include "common.h"

#define BUFFER_SIZE 512

int send_all(int fd, const char *data, size_t length)
{
    size_t sent=0;

    while (sent<length)
    {
        ssize_t n=send(fd, data+sent, length-sent, 0);

        if (n<=0) 
            return -1;

        sent+=(size_t)n;
    }

    return 0;
}

int socket_command(const char *command)
{
    int fd=socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd==-1) 
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_un address;

    memset(&address, 0, sizeof(address));

    address.sun_family=AF_UNIX;

    strncpy(address.sun_path, VSW_SOCK_PATH,
            sizeof(address.sun_path)-1);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address))==-1) 
    {
        perror("connect (is the switch running?)");
        close(fd);
        
        return 1;
    }

    char message[BUFFER_SIZE];

    snprintf(message, sizeof(message), "%s\n", command);

    if (send_all(fd, message, strlen(message))==-1) 
    {
        perror("send");
        close(fd);
    
      return 1;
    }

    char buffer[BUFFER_SIZE];

    ssize_t n;

    while ((n=recv(fd, buffer, sizeof(buffer), 0))>0)
        fwrite(buffer, 1, (size_t)n, stdout);

    if (n==-1) 
    {
        perror("recv");
        close(fd);
        return 1;
    }

    if (close(fd)==-1) 
    {
        perror("close");
        return 1;
    }

    return 0;
}

void show_stats(shm_stats_t *stats, int clear)
{
    port_stats_t ports[MAX_PORTS];
    int num_ports = shm_stats_copy(stats, ports);
    if (clear) 
        printf("\033[2J\033[H");

    printf("Virtual Switch Live Statistics\n\n");
    printf("Port  Frames In  Frames Out  Dropped  Mirrored\n");

    for (int i=0; i<num_ports; i++)
        printf("%4d  %10lu  %10lu  %7lu  %8lu\n", i, ports[i].frames_in, ports[i].frames_out, ports[i].frames_dropped, ports[i].frames_mirrored);

    if (clear)
        printf("\nRefreshing every second. Press Ctrl+C to stop.\n");

    fflush(stdout);
}

int stats_once(void)
{
    shm_stats_t *stats;
    int fd;

    if (shm_stats_attach(&stats, &fd)==-1) 
        return 1;

    show_stats(stats, 0);
    shm_stats_detach(stats, fd);

    return 0;
}

int stats_watch(void)
{
    shm_stats_t *stats;
    int fd;

    if (shm_stats_attach(&stats, &fd)==-1) 
        return 1;

    while (1) 
    {
        show_stats(stats, 1);
        sleep(1);
    }

    shm_stats_detach(stats, fd);

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc<2) 
    {
        printf("Usage:\n");
        printf("./swctl show mac\n");
        printf("./swctl show ports\n");
        printf("./swctl clear mac\n");
        printf("./swctl stats\n");
        printf("./swctl stats --watch\n");

        return 1;
    }

    if (strcmp(argv[1], "stats")==0) 
    {
        if (argc==2) 
            return stats_once();

        if (argc==3 && strcmp(argv[2], "--watch")==0) 
            return stats_watch();

        printf("Usage: ./swctl stats [--watch]\n");

        return 1;
    }

    char command[BUFFER_SIZE]="";

    for (int i=1; i<argc; i++) 
    {
        size_t extra = strlen(argv[i]);
        if (i > 1)
            extra += 1;
      
        if (strlen(command)+extra>=sizeof(command)) 
        {
            printf("Command too long\n");
            return 1;
        }

        if (i>1) 
            strcat(command, " ");
      
        strcat(command, argv[i]);
    }

    return socket_command(command);
}
