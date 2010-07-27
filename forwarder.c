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
#define HOST	"127.0.0.2"
#define PORT	53
#define BUF_SIZ	1024
#define TIMEOUT	500000 /* microseconds */

#define nonblock_socket(s) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)

struct dns_server_s {
	char name[15];
	unsigned short port;
	unsigned short conf_weight;
	unsigned short weight;
	int id;
	struct sockaddr_in addr;
	socklen_t len;
};

struct dns_client_s {
	int id;
	int ret;
	int inuse;
	struct timeval tv;
	struct sockaddr_in addr;
	socklen_t len;
};

typedef struct dns_server_s dns_server_t;
typedef struct dns_client_s dns_client_t;

static int sock;
static int clisock;
static int servers;
static dns_server_t *srv;
static int clients;
static dns_client_t *cli;

static void sock_init(void);
static void dns_init(void);
static void cli_init(void);
static void mainloop(void);
static ssize_t recv_udp(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen, int *id);
static ssize_t send_udp(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
static int find_newcli(void);
static int find_cli(int id);
static int find_dns(int ret);
static int find_timeout(struct timeval tp);
static int find_timeouted(struct timeval tp);

int
main(void)
{
	sock_init();
	dns_init();
	cli_init();
	mainloop();

	exit(0);
}

static void
sock_init(void)
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

static void
dns_init(void)
{
	int i;

	servers = 2;
	srv = malloc(sizeof(dns_server_t) * servers);

	strcpy(srv[0].name, "95.108.142.2");
	strcpy(srv[1].name, "95.108.142.1");
	srv[0].conf_weight = 10;
	srv[1].conf_weight = 90;

	for (i = 0; i < servers; i++) {
		bzero(&srv[i].addr, sizeof(struct sockaddr_in));
		srv[i].addr.sin_family = PF_INET;
		srv[i].addr.sin_addr.s_addr = inet_addr(srv[i].name);
		srv[i].addr.sin_port = htons(srv[i].port ? srv[i].port : PORT);
		srv[i].len = sizeof(struct sockaddr_in);
	}
}

static void
cli_init(void)
{
	int i;

	clients = 1024;
	cli = malloc(sizeof(dns_client_t) * clients);

	for (i = 0; i < clients; i++) {
		cli[i].inuse = 0;
		bzero(&cli[i].addr, sizeof(struct sockaddr_in));
		cli[i].len = sizeof(struct sockaddr_in);
	}
}

static void
mainloop(void)
{
	int i;
	int j;
	int k;
	int id;
	int kq;
	int nc;
	int to;
	ssize_t n;
	char buf[BUF_SIZ];
	struct kevent *ch;
	struct kevent *ev;
	struct timespec *timeout;
	struct timespec tv;
	dns_server_t srv1;

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

		k = kevent(kq, ch, nc, ev, 2 /* number of sockets */, timeout);
		if (n == -1)
			err(1, "kevent()");

		nc = 0;
		gettimeofday(&tp, NULL);
		printf("%d.%06ld: ", tp.tv_sec, tp.tv_usec);

		if (k == 0) {
			printf("kevent() timeout\n");
			/* Change DNS server */
			j = find_timeouted(tp);
			if (j == -1)
				continue;

			j = find_dns(cli[j].ret++);
			n = send_udp(clisock, buf, n, 0, (const struct sockaddr *)&srv[j].addr, srv[j].len);

			cli[j].tv = tp;
			to = find_timeout(tp);
			if (to == -1) {
				timeout = NULL;
			} else {
				tv.tv_sec = 0;
				tv.tv_nsec = to * 1000;
				timeout = &tv;
			}
		}

		for (i = 0; i < k; i++) {
			if (ev[i].ident == sock) {
				j = find_newcli();
				if (j == -1) {
					warnx("Max clients reach");
					continue;
				}
				/* Read request from client */
				n = recv_udp(sock, buf, BUF_SIZ, 0, (struct sockaddr *)&cli[j].addr,
				    &cli[j].len, &cli[j].id);

				j = find_dns(cli[j].ret++);
				/* Send request to DNS server */
				n = send_udp(clisock, buf, n, 0, (const struct sockaddr *)&srv[j].addr, srv[j].len);

				cli[j].tv = tp;
				to = find_timeout(tp);
				if (to == -1) {
					timeout = NULL;
				} else {
					tv.tv_sec = 0;
					tv.tv_nsec = to * 1000;
					timeout = &tv;
				}
			} else {
				/* Read answer from DNS server */
				n = recv_udp(clisock, buf, BUF_SIZ, 0, (struct sockaddr *)&srv1.addr,
				    &srv1.len, &id);
				j = find_cli(id);
				if (j == -1) {
					warnx("Cannot find client");
					continue;
				}

				/* Send answer to client */
				n = send_udp(sock, buf, n, 0, (const struct sockaddr *)&cli[j].addr, cli[j].len);
				cli[j].inuse = 0;
				/* If second DNS answer faster than first, decrease weight of first DNS */
				timeout = NULL;
			}
		}
	}
}

static ssize_t
recv_udp(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen, int *id)
{
	ssize_t n;
	short int dns_id;

	n = recvfrom(s, buf, len, flags, from, fromlen);
	if (n == -1)
		err(1, "recvfrom()");

	memcpy(&dns_id, buf, sizeof(short int));
	printf("read %d bytes from client (id: %d)\n", n, ntohs(dns_id));
	*id = ntohs(dns_id);

	return(n);
}

static ssize_t
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

static int
find_newcli(void)
{
	int i;

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 0) {
			cli[i].inuse = 1;
			cli[i].ret = 0;
			return(i);
		}
	}

	return(-1);
}

static int
find_cli(int id)
{
	int i;

	for (i = 0; i < clients; i++) {
		if (cli[i].id == id)
			return(i);
	}

	return(-1);
}

static int
find_dns(int ret)
{
	if (ret % servers == 0)
		return(0);
	else
		return(1);
}

static int
find_timeout(struct timeval tp)
{
	int i;
	int to;
	struct timeval diff;
	struct timeval diff1;

	diff.tv_sec = 0;
	diff.tv_usec = 0;

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 1) {

			printf("client #%d time: %d.%06ld\n", i, cli[i].tv.tv_sec, cli[i].tv.tv_usec);

			diff1.tv_sec = tp.tv_sec - cli[i].tv.tv_sec;
			if (tp.tv_usec < cli[i].tv.tv_usec) {
				diff1.tv_usec = 1000000 + tp.tv_usec - cli[i].tv.tv_usec;
				diff1.tv_sec = diff1.tv_sec - 1;
			} else {
				diff1.tv_usec = tp.tv_usec - cli[i].tv.tv_usec;
			}

			printf("client #%d diff: %d.%06ld\n", i, diff1.tv_sec, diff1.tv_usec);

			if (diff1.tv_usec > 0 && diff1.tv_sec >= diff.tv_sec && diff1.tv_usec > diff.tv_usec) {
				diff = diff1;
			}
		}
	}

	printf("diff: %d.%06ld\n", diff.tv_sec, diff.tv_usec);

	if (TIMEOUT - diff.tv_usec > 0)
		to = TIMEOUT - diff.tv_usec;
	else
		to = TIMEOUT;

	printf("timeout set: 0.%d\n", to);

	return(to);
}

static int
find_timeouted(struct timeval tp)
{
	int i;

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 1) {
			if (tp.tv_sec > cli[i].tv.tv_sec) {
				if (tp.tv_usec < cli[i].tv.tv_usec) {
					if (1000000 + tp.tv_usec - cli[i].tv.tv_usec + TIMEOUT) {
						return(i);
					}
				}
			} else if (tp.tv_sec == cli[i].tv.tv_sec && tp.tv_usec > cli[i].tv.tv_usec + TIMEOUT) {
				return(i);
			}
		}
	}

	return(-1);
}
