/*_
 * Copyright (c) 2010, 2011 Andrey Zonov <andrey@zonov.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _FORWARDER_H
#define _FORWARDER_H

#include <sys/queue.h>

/*#define HOST	INADDR_ANY*/
#define HOST	"127.0.0.2"
#define PORT	53

#define NS_MAXID	(1<<16)	/* maximum query ID */
#define NS_PACKETSZ	512	/* default UDP packet size from <arpa/nameser.h> */

#define nonblock_socket(s) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)

typedef struct addr_s addr_t;
typedef struct buf_s buf_t;
typedef struct server_s server_t;
typedef struct client_s client_t;
typedef struct request_s request_t;

struct addr_s {
	struct sockaddr_in sin;
	socklen_t len;
};

struct buf_s {
	void *data;
	ssize_t len;
};

struct server_s {
	u_short id;
	u_int weight;
	struct {
		u_int weight;
	} conf;
	size_t send;
	size_t recv;
	addr_t addr;
	in_port_t port;
	char name[INET_ADDRSTRLEN];
	STAILQ_ENTRY(server_s) next;
};
STAILQ_HEAD(, server_s) srvq;

struct client_s {
	u_short id;
	u_short ret;
	u_long num;
	struct timeval tv;
	addr_t addr;
	buf_t buf;
	server_t *srv;
	request_t *req;
};

struct request_s {
	client_t *cli;
	STAILQ_ENTRY(request_s) next;
};
STAILQ_HEAD(, request_s) requests;

int servers;
int attempts;
int autoweight;
struct timeval timeout;

#endif
