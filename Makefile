PREFIX ?= /usr/local
bindir ?= $(PREFIX)/bin
mandir ?= $(PREFIX)/share/man

CFLAGS ?= -Wall -O3 -g
VERSION?=$(shell (git describe --tags HEAD 2>/dev/null || echo "v0.1.0") | sed 's/^v//')

###############################################################################

ifeq ($(shell pkg-config --exists jack || echo no), no)
  $(warning *** libjack from http://jackaudio.org is required)
  $(error   Please install libjack-dev or libjack-jackd2-dev)
endif

ifeq ($(shell pkg-config --exists liblo || echo no), no)
  $(warning *** liblo from http://liblo.sourceforge.net is required)
  $(error   Please install liblo-dev)
endif

###############################################################################

override CFLAGS += -DVERSION="\"$(VERSION)\""
override CFLAGS += `pkg-config --cflags jack liblo`
LOADLIBES = `pkg-config --cflags --libs jack liblo` -lm -lpthread
man1dir   = $(mandir)/man1

###############################################################################

default: all

jackmidi2osc$(EXE_EXT): jackmidi2osc.c

install-bin: jackmidi2osc$(EXE_EXT)
	install -d $(DESTDIR)$(bindir)
	install -m755 jackmidi2osc $(DESTDIR)$(bindir)

install-man: jackmidi2osc.1
	install -d $(DESTDIR)$(man1dir)
	install -m644 jackmidi2osc.1 $(DESTDIR)$(man1dir)

uninstall-bin:
	rm -f $(DESTDIR)$(bindir)/jackmidi2osc$(EXE_EXT)
	-rmdir $(DESTDIR)$(bindir)

uninstall-man:
	rm -f $(DESTDIR)$(man1dir)/jackmidi2osc.1
	-rmdir $(DESTDIR)$(man1dir)
	-rmdir $(DESTDIR)$(mandir)

clean:
	rm -f jackmidi2osc$(EXE_EXT)

man: jackmidi2osc
	help2man -N -n 'JACK MIDI to OSC' -o jackmidi2osc.1 ./jackmidi2osc

all: jackmidi2osc$(EXE_EXT)

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

.PHONY: default all man clean install install-bin install-man uninstall uninstall-bin uninstall-man
