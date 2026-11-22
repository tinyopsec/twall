# twall - minimal Wayland wallpaper picker
# See README.md for build and install details.

VERSION = 0.2

PREFIX  ?= /usr/local
BINDIR  = ${PREFIX}/bin

# Linux
# FreeBSD / DragonFly
#X11INC = /usr/local/include
#X11LIB = /usr/local/lib
# NetBSD (pkgsrc)
#X11INC = /usr/pkg/include
#X11LIB = /usr/pkg/lib
# OpenBSD
#X11INC = /usr/X11R6/include
#X11LIB = /usr/X11R6/lib

CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_POSIX_C_SOURCE=200809L
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -Wstrict-prototypes \
           -Wmissing-prototypes -Wshadow -Wformat=2 -Werror \
           -fno-common -O2 ${CPPFLAGS}
LDFLAGS  =

CC ?= cc

SRC = twall.c
OBJ = ${SRC:.c=.o}

all: twall

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h

twall: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

debug: ${SRC} config.h
	${CC} -std=c99 -pedantic -Wall -Wextra -Wstrict-prototypes \
		-Wmissing-prototypes -Wshadow -Wformat=2 -g -O0 \
		${CPPFLAGS} -o twall_dbg ${SRC}

dist: clean
	mkdir -p twall-${VERSION}
	cp twall.c config.h Makefile README.md twall-${VERSION}
	tar -czf twall-${VERSION}.tar.gz twall-${VERSION}
	rm -rf twall-${VERSION}

clean:
	rm -f twall twall_dbg ${OBJ} twall-${VERSION}.tar.gz

install: all
	mkdir -p ${DESTDIR}${BINDIR}
	install -m 755 twall ${DESTDIR}${BINDIR}/twall

uninstall:
	rm -f ${DESTDIR}${BINDIR}/twall

.PHONY: all clean debug dist install uninstall
