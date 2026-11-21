CC     = gcc
CFLAGS = CFLAGS = -std=c11 -D -Wall -Wextra -Werror -g -O0 -pthread
PROGS  =

all: $(PROGS)

clean:
	rm -f $(PROGS) 

.PHONY: all clean