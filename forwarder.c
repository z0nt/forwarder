/*-
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

#include <sys/types.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/*#define HOST	INADDR_ANY*/
#define HOST	"127.0.0.1"
#define PORT	53
#define BUF_SIZ	1024
#define TIMEOUT	500 /* milliseconds */

#define nonblock_socket(s) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)

struct dns_server_s {
	char name[1024];
	unsigned short port;
	unsigned short conf_weight;
	unsigned short weight;
	struct sockaddr_in addr;
	socklen_t len;
};

struct dns_client_s {
	struct sockaddr_in addr;
	socklen_t len;
};

typedef struct dns_server_s dns_server_t;
typedef struct dns_client_s dns_client_t;

int sock;
int clisock;

/* FIXME */
int dns_servers;
dns_server_t dns_srv[2];
dns_client_t dns_cli[65536];

void sockinit(void);
void dnsinit(void);
void mainloop(void);
ssize_t recv_udp(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
ssize_t send_udp(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);

int
main(void)
{
	sockinit();
	dnsinit();
	mainloop();

	exit(0);
}

void
sockinit(void)
{
	int rc;
	struct sockaddr_in servaddr;

	/* Server socket */
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == -1)
		err(1, "socket()");

	nonblock_socket(sock);

	bzero(&servaddr, sizeof(struct sockaddr_in));
	servaddr.sin_family = PF_INET;
	servaddr.sin_addr.s_addr = inet_addr(HOST);
	servaddr.sin_port = htons(PORT);

	rc = bind(sock, (const struct sockaddr *)&servaddr, sizeof(struct sockaddr_in));
	if (rc == -1)
		err(1, "bind()");

	/* Client socket */
	clisock = socket(PF_INET, SOCK_DGRAM, 0);
	if (clisock == -1)
		err(1, "socket()");

	nonblock_socket(clisock);
}

void
dnsinit(void)
{
	int i;

	dns_servers = 2;
	strcpy(dns_srv[0].name, "95.108.142.1");
	strcpy(dns_srv[1].name, "95.108.142.2");
	dns_srv[0].conf_weight = 10;
	dns_srv[1].conf_weight = 90;

#if 0
	dns = malloc(sizeof(struct dns_server));
	if (dns == NULL)
		err(1, "malloc()");
#endif

	for (i = 0; i < dns_servers; i++) {
		bzero(&dns_srv[i].addr, sizeof(struct sockaddr_in));
		dns_srv[i].addr.sin_family = PF_INET;
		dns_srv[i].addr.sin_addr.s_addr = inet_addr(dns_srv[i].name);
		dns_srv[i].addr.sin_port = htons(dns_srv[i].port ? dns_srv[i].port : PORT);
		dns_srv[i].len = sizeof(struct sockaddr_in);
	}
}

void
mainloop(void)
{
	int i;
	int k;
	int kq;
	int nc;
	ssize_t n;
	char buf[BUF_SIZ];
	struct kevent *ch;
	struct kevent *ev;
	struct timespec *timeout;
	struct timespec tv;
	dns_server_t *srv;
	dns_client_t *cli;
	dns_client_t cli1;

	/* Init DNS server with highest weight */
	srv = &dns_srv[1];
	/* Init client structure */
	cli1.len = sizeof(struct sockaddr_in);
	cli = &cli1;

	ch = calloc(1, sizeof(struct kevent) * 2);
	if (ch == NULL)
		err(1, "malloc()");

	ev = calloc(1, sizeof(struct kevent) * 2);
	if (ev == NULL)
		err(1, "malloc()");

	kq = kqueue();
	if (kq == -1)
		err(1, "kqueue()");

	nc = 0;
	timeout = NULL;
	EV_SET(&ch[nc++], sock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&ch[nc++], clisock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	/* Main loop */
	for ( ;; ) {

		k = kevent(kq, ch, nc, ev, 2, timeout);
		if (n == -1)
			err(1, "kevent()");

		nc = 0;

		if (k == 0) {
			printf("kevent() timeout\n");
			/* Change DNS server */
			srv = &dns_srv[0];
			n = send_udp(clisock, buf, n, 0, (const struct sockaddr *)&srv->addr, srv->len);
		}

		for (i = 0; i < k; i++) {
			if (ev[i].ident == sock) {
				/* Read request from client */
				n = recv_udp(sock, buf, BUF_SIZ, 0, (struct sockaddr *)&cli->addr, &cli->len);
				/* Copy ...
				memcpy(dns_cli[id], cli, sizeof(dns_client_t));
				*/

				/* Send request to DNS server */
				n = send_udp(clisock, buf, n, 0, (const struct sockaddr *)&srv->addr, srv->len);

				tv.tv_sec = 0;
				tv.tv_nsec = TIMEOUT * 1000000;
				timeout = &tv;
			}

			if (ev[i].ident == clisock) {
				/* Read answer from DNS server */
				n = recv_udp(clisock, buf, BUF_SIZ, 0, (struct sockaddr *)&srv->addr, &srv->len);

				/* Send answer to client */
				n = send_udp(sock, buf, n, 0, (const struct sockaddr *)&cli->addr, cli->len);
				/* If second DNS answer faster than first, decrease weight of first DNS */
				timeout = NULL;
			}
		}
	}
}

ssize_t
recv_udp(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
	ssize_t n;
	short int dns_id;

	n = recvfrom(s, buf, len, flags, from, fromlen);
	if (n == -1)
		err(1, "recvfrom()");

	memcpy(&dns_id, buf, sizeof(short int));
	printf("read %d bytes from client (id: %d)\n", n, ntohs(dns_id));

	return(n);
}

ssize_t
send_udp(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
	ssize_t n;
	short int dns_id;

	n = sendto(s, buf, len, flags, to, tolen);
	if (n == -1)
		err(1, "sendto()");

	memcpy(&dns_id, buf, sizeof(short int));
	printf("write %d bytes to client (id: %d)\n", n, ntohs(dns_id));

	return(n);
}
