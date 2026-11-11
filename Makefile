CC ?= gcc
CFLAGS ?= -O2 -Wall
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall

all: powerd

powerd: src/powerd.c
$(CC) $(CFLAGS) $< -o $@

install: powerd
sudo ./scripts/install.sh

clean:
rm -f powerd
