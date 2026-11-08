# CUPS filter + backend for Matica XID8300 / XID8100 card printers

CC        ?= cc
CFLAGS    ?= -O2 -g
CFLAGS    += -Wall -Wextra -Wno-deprecated-declarations -std=c99 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CFLAGS    += $(shell cups-config --cflags 2>/dev/null)
CUPSLIBS  := $(shell cups-config --libs 2>/dev/null || echo -lcups)
IMAGELIBS := $(shell cups-config --image --libs 2>/dev/null || echo -lcupsimage -lcups)

SERVERBIN ?= $(shell cups-config --serverbin 2>/dev/null || echo /usr/lib/cups)
DATADIR   ?= $(shell cups-config --datadir 2>/dev/null || echo /usr/share/cups)
DESTDIR   ?=

all: rastertoxid8300 xid8300 ppd/xid8100.ppd

rastertoxid8300: src/rastertoxid8300.c src/xid8300.h src/xid8300_areas.h
	$(CC) $(CFLAGS) -o $@ src/rastertoxid8300.c $(IMAGELIBS) -lm

xid8300: src/xid8300.c src/xid8300.h
	$(CC) $(CFLAGS) -DXID8300_LIBDIR='"$(SERVERBIN)/xid8300"' -o $@ src/xid8300.c $(CUPSLIBS)

# The XID8100 is printed with the XID8300 Windows driver (same protocol), so
# its PPD is the XID8300 one with the model names changed.
ppd/xid8100.ppd: ppd/xid8300.ppd
	sed -e 's/XID8300\.PPD/XID8100.PPD/' -e 's/(XID8300)/(XID8100)/' \
	    -e 's/^\*ModelName: "Matica XID8300"/*ModelName: "Matica XID8100"/' \
	    -e 's/^\*ShortNickName: "Matica XID8300"/*ShortNickName: "Matica XID8100"/' \
	    -e 's/^\*NickName: .*/*NickName: "Matica XID8100 card printer (XID8300 protocol), rastertoxid8300"/' \
	    ppd/xid8300.ppd > $@

tests/mkraster: tests/mkraster.c
	$(CC) $(CFLAGS) -o $@ tests/mkraster.c $(IMAGELIBS)

tests/areacheck: tests/areacheck.c src/xid8300_areas.h src/xid8300.h
	$(CC) $(CFLAGS) -o $@ tests/areacheck.c

check: all tests/mkraster tests/areacheck
	sh tests/run.sh

install: all
	install -d $(DESTDIR)$(SERVERBIN)/filter $(DESTDIR)$(SERVERBIN)/backend
	install -m 0755 rastertoxid8300 $(DESTDIR)$(SERVERBIN)/filter/
	install -m 0755 xid8300 $(DESTDIR)$(SERVERBIN)/backend/
	install -d $(DESTDIR)$(SERVERBIN)/xid8300
	install -m 0644 examples/ic-encoder-example.sh $(DESTDIR)$(SERVERBIN)/xid8300/
	install -d $(DESTDIR)$(DATADIR)/model
	install -m 0644 ppd/xid8300.ppd ppd/xid8100.ppd $(DESTDIR)$(DATADIR)/model/
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 packaging/xid8300-setup packaging/xid8300-import $(DESTDIR)/usr/sbin/

clean:
	rm -rf rastertoxid8300 xid8300 ppd/xid8100.ppd tests/mkraster tests/areacheck tests/out *.dSYM tests/*.dSYM

.PHONY: all check install clean
