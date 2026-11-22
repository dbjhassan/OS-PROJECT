#ifndef LOGGER_H
#define LOGGER_H

int logger_start(const char *path);

void logger_stop(void);

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
