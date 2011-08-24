#

PROG=	forwarder
SRCS=	config.c forwarder.c log.c
NO_MAN=	1

DEBUG_FLAGS=	-g
CFLAGS= -Wall -pedantic -ansi
#LDFLAGS=	-pg

.include <bsd.prog.mk>
