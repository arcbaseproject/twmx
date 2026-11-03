# twmx - see README.md for details

VERSION = 2.1

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
XSESSIONDIR ?= $(PREFIX)/share/xsessions

PKG_CONFIG ?= pkg-config
XINC != $(PKG_CONFIG) --cflags x11 xrandr 2>/dev/null || echo -I/usr/X11R6/include
XLIB != $(PKG_CONFIG) --libs x11 xrandr 2>/dev/null || echo -L/usr/X11R6/lib -lX11 -lXrandr

CPPFLAGS += -D_DEFAULT_SOURCE -DVERSION=\"$(VERSION)\"
CFLAGS ?= -Os
CFLAGS += -std=c99 -pedantic -Wall -Wextra

all: twmx

config.h:
	cp config.def.h $@

twmx: twmx.c config.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(XINC) twmx.c -o $@ $(LDFLAGS) $(XLIB)

test: config.h
	@set -e; testbin=$$(mktemp /tmp/twmx-test.XXXXXX); \
	trap 'rm -f "$$testbin"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(XINC) tests/regression.c -o "$$testbin" $(LDFLAGS) $(XLIB); \
	sh tests/run.sh "$$testbin"

clean:
	rm -f twmx

install: twmx
	mkdir -p $(DESTDIR)$(BINDIR)
	cp -f twmx $(DESTDIR)$(BINDIR)/twmx
	chmod 755 $(DESTDIR)$(BINDIR)/twmx
	mkdir -p $(DESTDIR)$(XSESSIONDIR)
	printf '[Desktop Entry]\nName=twmx\nComment=Small tiling window manager\nExec=%s\nType=Application\n' \
		$(BINDIR)/twmx >$(DESTDIR)$(XSESSIONDIR)/twmx.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/twmx $(DESTDIR)$(XSESSIONDIR)/twmx.desktop

.PHONY: all test clean install uninstall
