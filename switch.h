#ifndef SWITCH_H
#define SWITCH_H

int switch_init(int n);

void switch_shutdown(void);

int session_spawn(int fd);

int switch_dump_mac(const char *path);

#endif
