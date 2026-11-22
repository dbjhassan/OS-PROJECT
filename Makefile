CC      = gcc
CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -O0 -pthread -MMD -MP $(EXTRA)
LDFLAGS = -pthread $(EXTRA)
LDLIBS  = -lrt

PROGS = switch host swtcl

all: $(PROGS)

switch: main.o switch.o protocol.o frame.o queue.o mactable.o logger.o
	$(CC) $(LDFLAGS) -o switch main.o switch.o protocol.o frame.o queue.o mactable.o logger.o $(LDLIBS)

host: host.o protocol.o
	$(CC) $(LDFLAGS) -o host host.o protocol.o $(LDLIBS)

swtcl: swtcl.o shmstats.o protocol.o
	$(CC) $(LDFLAGS) -o swtcl swtcl.o shmstats.o protocol.o $(LDLIBS)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

switch.o: switch.c
	$(CC) $(CFLAGS) -c switch.c -o switch.o

protocol.o: protocol.c
	$(CC) $(CFLAGS) -c protocol.c -o protocol.o

frame.o: frame.c
	$(CC) $(CFLAGS) -c frame.c -o frame.o

queue.o: queue.c
	$(CC) $(CFLAGS) -c queue.c -o queue.o

mactable.o: mactable.c
	$(CC) $(CFLAGS) -c mactable.c -o mactable.o

logger.o: logger.c
	$(CC) $(CFLAGS) -c logger.c -o logger.o

host.o: host.c
	$(CC) $(CFLAGS) -c host.c -o host.o

swtcl.o: swtcl.c
	$(CC) $(CFLAGS) -c swtcl.c -o swtcl.o

shmstats.o: shmstats.c
	$(CC) $(CFLAGS) -c shmstats.c -o shmstats.o

# make tsan : clean rebuild with ThreadSanitizer
tsan:
	$(MAKE) clean
	$(MAKE) EXTRA=-fsanitize=thread

clean:
	rm -f $(PROGS) *.o *.d

-include $(wildcard *.d)

.PHONY: all clean tsan
