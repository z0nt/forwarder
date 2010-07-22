#

PROG=	forwarder
SRCS=	forwarder.c
NO_MAN=	1

DEBUG_FLAGS=	-g
CFLAGS= -Wall -pedantic -ansi

.include <bsd.prog.mk>
