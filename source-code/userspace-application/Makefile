
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Wextra -Werror
LDFLAGS ?= -pthread -lrt

all: aeldfinal

aeldfinal: rc522-poll.c
	$(CC) $(CFLAGS) rc522-poll.c -o aeldfinal $(LDFLAGS)

clean:
	rm -f aeldfinal

.PHONY: all default clean