/*
 * Copyright (c) 2010 Andrey Zonov <andrey.zonov@gmail.com>
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

/*#define HOST	INADDR_ANY*/
#define HOST	"127.0.0.2"
#define PORT	53
#define BUF_SIZ	512	/* max packet size */
#define TIMEOUT	500000	/* microseconds */
#define CLIENTS	1024	/* default clients number */

#define IP_LEN		sizeof("255.255.255.255")
#define PORT_LEN	sizeof("65535")
#define WEIGHT_LEN	sizeof("65535")

#define nonblock_socket(s) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)

struct server_s {
	char name[IP_LEN];
	unsigned short port;
	unsigned short conf_weight;
	unsigned short weight;
	int id;
	struct sockaddr_in addr;
	socklen_t len;
};

struct client_s {
	int id;
	int ret;
	int inuse;
	char buf[BUF_SIZ];
	struct timeval tv;
	struct sockaddr_in addr;
	socklen_t len;
};

typedef struct server_s server_t;
typedef struct client_s client_t;

int servers;
server_t *srv;