CC      ?= cc
CFLAGS  ?= -O2 -Wall
LDLIBS   = $(shell pkg-config --cflags --libs libevdev)
PREFIX  ?= /usr/local

all: tpfilter

tpfilter: src/tpfilter.c
	$(CC) $(CFLAGS) -o $@ src/tpfilter.c $(LDLIBS)

install: tpfilter
	install -Dm755 tpfilter $(DESTDIR)$(PREFIX)/bin/tpfilter
	install -Dm644 systemd/tpfilter.service $(DESTDIR)$(PREFIX)/lib/systemd/system/tpfilter.service

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tpfilter
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/system/tpfilter.service

clean:
	rm -f tpfilter

.PHONY: all install uninstall clean
