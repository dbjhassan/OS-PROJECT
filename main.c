#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "protocol.h"
#include "mac_table.h"
#include "logger.h"
#include "common.h"

static int stop_pipe[2]={-1, -1};
static pthread_t sig_tid;
static const char *config_path=NULL;

int config_load(const char *path)
{
    FILE *f = fopen(path, "r");

    if (f == NULL) 
    {
        log_msg("config: cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    int entries = 0;
    int per_port = 0;
    int age = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) != NULL) 
    {
        char key[64];
        int value;

        if (line[0] == '#' || line[0] == '\n') 
        {
            continue;
        }

        if (sscanf(line, "%63s %d", key, &value) != 2) 
        {
            log_msg("config: bad line ignored: %.60s", line);
            continue;
        }

        if (strcmp(key, "mac_max_entries") == 0) 
        {
            entries = value;
        } 
        else if (strcmp(key, "mac_max_per_port") == 0) 
        {
            per_port = value;
        } 
        else if (strcmp(key, "mac_age_seconds") == 0) 
        {
            age = value;
        } 
        else 
        {
            log_msg("config: unknown key ignored: %s", key);
        }
    }

    fclose(f);

    mactable_set_limits(entries, per_port, age);
    log_msg("config loaded from %s (entries=%d per_port=%d age=%d, 0 = unchanged)", path, entries, per_port, age);

    return 0;
}

void fill_sigset(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGHUP);
    sigaddset(set, SIGUSR1);
}

void *signal_thread(void *arg)
{
    (void)arg;
    sigset_t set;
    fill_sigset(&set);

    while (1) 
    {
        int sig;
        int rc = sigwait(&set, &sig);

        if (rc!=0) 
        {
            fprintf(stderr, "sigwait: %s\n", strerror(rc));
            break;
        }

        if (sig==SIGINT || sig==SIGTERM) 
        {
            log_msg("signal %d received, shutting down", sig);

            if (write(stop_pipe[1], "x", 1) == -1) 
                perror("write");

            break;
        }

        if (sig == SIGHUP) 
        {
            if (config_path != NULL) 
            {
                config_load(config_path);
            } 
            else 
            {
                log_msg("SIGHUP received, no config file given");
            }
        }

        if (sig == SIGUSR1) 
        {
            if (switch_dump_mac(VSW_DUMP_PATH) == 0) 
            {
                log_msg("MAC table dumped to %s", VSW_DUMP_PATH);
            } 
            else
            {
                log_msg("MAC table dump to %s failed", VSW_DUMP_PATH);
            }
        }
    }

    return NULL;
}

int serve(int port, int num_ports)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd == -1) 
    {
        perror("socket");
        return -1;
    }

    int one = 1;

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) 
    {
        perror("setsockopt");
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);

    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) == -1) 
    {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 64) == -1) 
    {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    log_msg("listening on %d with %d ports", port, num_ports);
    printf("switch running on port %d, log in %s\n", port, VSW_LOG_PATH);
    fflush(stdout);

    struct pollfd pfd[2];

    pfd[0].fd = listen_fd;
    pfd[0].events = POLLIN;
    pfd[1].fd = stop_pipe[0];
    pfd[1].events = POLLIN;

    int status = 0;

    while (1) 
    {
        int n = poll(pfd, 2, -1);

        if (n == -1) 
        {
            if (errno == EINTR) 
            {
                continue;
            }

            perror("poll");
            status = -1;
            break;
        }

        if (pfd[1].revents & POLLIN) 
        {
            break;
        }

        if (pfd[0].revents & POLLIN) 
        {
            int fd = accept(listen_fd, NULL, NULL);

            if (fd == -1) 
            {
                if (errno == EINTR || errno == ECONNABORTED) 
                {
                    continue;
                }

                perror("accept");
                status = -1;
                break;
            }

            if (session_spawn(fd) == -1) 
            {
                log_msg("could not start a session, connection dropped");

                if (close(fd) == -1) 
                {
                    perror("close");
                }
            }
        }
    }

    if (close(listen_fd) == -1) 
    {
        perror("close");
    }

    return status;
}

int main(int argc, char *argv[])
{
    if (argc != 3 && argc != 4) 
    {
        fprintf(stderr, "usage: %s <tcp_port> <num_ports> [config_file]\n", argv[0]);
        return 1;
    }

    int port = parse_port(argv[1]);

    if (port == -1) 
    {
        fprintf(stderr, "bad port\n");
        return 1;
    }

    int num_ports = parse_port(argv[2]);

    if (num_ports == -1 || num_ports > MAX_PORTS) 
    {
        fprintf(stderr, "num_ports must be 1 to %d\n", MAX_PORTS);
        return 1;
    }

    if (argc == 4) 
    {
        config_path = argv[3];
    }

    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = SIG_IGN;

    if (sigaction(SIGPIPE, &sa, NULL) == -1) 
    {
        perror("sigaction");
        return 1;
    }

    sigset_t set;

    fill_sigset(&set);

    int rc = pthread_sigmask(SIG_BLOCK, &set, NULL);

    if (rc != 0) 
    {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(rc));
        return 1;
    }

    if (logger_start(VSW_LOG_PATH) == -1) 
    {
        return 1;
    }

    if (pipe(stop_pipe) == -1) 
    {
        perror("pipe");
        logger_stop();
        return 1;
    }

    int status = 1;
    int inited = 0;

    if (switch_init(num_ports) == 0) 
    {
        inited = 1;

        if (config_path != NULL) 
        {
            config_load(config_path);
        }

        rc = pthread_create(&sig_tid, NULL, signal_thread, NULL);

        if (rc != 0) 
        {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
        } 
        else 
        {
            status = serve(port, num_ports) == 0 ? 0 : 1;

            if (status != 0) 
            {
                pthread_kill(sig_tid, SIGTERM);
            }

            pthread_join(sig_tid, NULL);
        }
    }

    log_msg("switch stopped");

    if (inited) 
    {
        switch_shutdown();
    }

    logger_stop();

    close(stop_pipe[0]);
    close(stop_pipe[1]);

    return status;
}
