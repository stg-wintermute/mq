CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
PREFIX  ?= /usr/local/bin

SRCS = src/parser.c src/filter.c src/format.c src/main.c
HDR  = src/mq.h

mq: $(SRCS) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

install: mq
	install -Dm755 mq $(DESTDIR)$(PREFIX)/mq

clean:
	rm -f mq

.PHONY: install clean
