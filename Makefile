CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall

all: powerd

powerd: src/powerd.c
	$(CC) $(CFLAGS) $< -o $@

install: powerd
	install -Dm755 powerd $(DESTDIR)$(BINDIR)/powerd
	@if [ -f bin/powerctl ]; then install -Dm755 bin/powerctl $(DESTDIR)$(BINDIR)/powerctl; fi
	@if command -v systemctl >/dev/null 2>&1 && systemctl is-enabled powerd >/dev/null 2>&1; then systemctl restart powerd; fi

uninstall:
	@if command -v systemctl >/dev/null 2>&1; then systemctl stop powerd 2>/dev/null || true; fi
	rm -f $(DESTDIR)$(BINDIR)/powerd $(DESTDIR)$(BINDIR)/powerctl

clean:
	rm -f powerd
