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

#include <sys/types.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "config.h"
#include "forwarder.h"
#include "log.h"

static int sock;
static int clisock;
static int active_clients;
static int clients;
static client_t *cli;
static unsigned long requests_cli;
static unsigned long requests_srv;
static unsigned int weight_factor;
static int *srv_ptr;
static int stat_flag = 0;

static void srv_init(void);
static void cli_init(void);
static void sock_init(void);
static void sig_init(void);
static void sig_handler(int signum);
static void mainloop(void);
static ssize_t recv_udp(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen, int *id);
static ssize_t send_udp(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
static int find_newcli(void);
static int find_cli(int id);
static int find_srv(int ret);
static int find_timeout(struct timeval tp);
static int find_timeouted(struct timeval tp);
static void make_stat(void);

int
main(void)
{
	config_init("forwarder.conf");
	srv_init();
	cli_init();
	sock_init();
	debug_level = 3;
	syslog_flag = 0;
	loginit("forwarder.log");
	sig_init();
	mainloop();

	exit(0);
}

static void
srv_init(void)
{
	int i;
	int j;
	int n;
	int w;

	weight_factor = 0;

	for (i = 0; i < servers; i++) {
		weight_factor += srv[i].conf_weight;
		bzero(&srv[i].addr, sizeof(struct sockaddr_in));
		srv[i].len = sizeof(struct sockaddr_in);
		srv[i].addr.sin_family = PF_INET;
		srv[i].addr.sin_port = htons(srv[i].port);
		srv[i].addr.sin_addr.s_addr = inet_addr(srv[i].name);
		if (srv[i].addr.sin_addr.s_addr == -1)
			err(1, "inet_addr()");
	}

	srv_ptr = malloc(sizeof(int) * weight_factor);
	if (srv_ptr == NULL)
		err(1, "malloc()");

	w = 0;
	for (i = 0; i < servers; i++) {
		n = srv[i].conf_weight;
		for (j = n + w; j > w; j--) {
			srv_ptr[j - 1] = i;
		}
		w += n;
	}
}

static void
cli_init(void)
{
	int i;

	clients = CLIENTS;
	cli = malloc(sizeof(client_t) * clients);
	if (cli == NULL)
		err(1, "malloc()");

	for (i = 0; i < clients; i++) {
		cli[i].inuse = 0;
		bzero(&cli[i].addr, sizeof(struct sockaddr_in));
		cli[i].len = sizeof(struct sockaddr_in);
	}
}

static void
sock_init(void)
{
	int rc;
	struct sockaddr_in servaddr;

	/* Listen socket */
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == -1)
		err(1, "socket()");

	nonblock_socket(sock);

	bzero(&servaddr, sizeof(struct sockaddr_in));
	servaddr.sin_family = PF_INET;
	servaddr.sin_port = htons(PORT);
	servaddr.sin_addr.s_addr = inet_addr(HOST);
	if (servaddr.sin_addr.s_addr == -1)
		err(1, "inet_addr()");

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
sig_init(void)
{
	struct sigaction act;
	sigset_t sigmask;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGPIPE);
	sigprocmask(SIG_BLOCK, &sigmask, NULL);

	act.sa_flags = 0;
	act.sa_handler = sig_handler;
	sigemptyset(&act.sa_mask);
	sigaction(SIGTERM, &act, 0);
	sigaction(SIGINT, &act, 0);
	sigaction(SIGUSR1, &act, 0);
}

static void
sig_handler(int signum)
{
	if (signum == SIGUSR1) {
		stat_flag = 1;
	} else {
		exit(0);
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
	int active;
	ssize_t n;
	void *ptr;
	char buf[BUF_SIZ];
	struct kevent *ch;
	struct kevent *ev;
	struct timespec *timeout;
	struct timespec tv;
	struct timeval tp;
	server_t srv1;

	srv1.len = sizeof(struct sockaddr_in);

	ch = calloc(1, sizeof(struct kevent) * 2);
	if (ch == NULL) {
		logerr(1, "calloc()");
		exit(1);
	}

	ev = calloc(1, sizeof(struct kevent) * 2);
	if (ev == NULL) {
		logerr(1, "calloc()");
		exit(1);
	}

	kq = kqueue();
	if (kq == -1) {
		logerr(1, "kqueue()");
		exit(1);
	}

	active_clients = 0;
	requests_cli = 0;
	requests_srv = 0;

	nc = 0;
	EV_SET(&ch[nc++], sock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&ch[nc++], clisock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	/* Main loop */
	for ( ;; ) {
		if (active_clients == 0) {
			timeout = NULL;
		} else {
			to = find_timeout(tp);
			tv.tv_sec = 0;
			tv.tv_nsec = to * 1000;
			timeout = &tv;
		}

		do {
			k = kevent(kq, ch, nc, ev, 2 /* number of sockets */, timeout);
			if (stat_flag == 1) {
				make_stat();
				stat_flag = 0;
			}
		} while (k == -1 && errno == EINTR);

		if (k == -1)
			logerr(1, "kevent()");

		nc = 0;
		gettimeofday(&tp, NULL);
		logout(3, "time: %d.%06ld", tp.tv_sec, tp.tv_usec);

		if (k == 0) {
			logout(3, "kevent() timeout");
		}

		for (i = 0; i < k; i++) {
			if (ev[i].ident == sock) {
				j = find_newcli();
				if (j == -1) {
					logout(1, "Max clients reach");
					continue;
				}
				/* Read request from client */
				n = recv_udp(sock, cli[j].buf, BUF_SIZ, 0, (struct sockaddr *)&cli[j].addr,
				    &cli[j].len, &cli[j].id);
				++active_clients;
				++requests_cli;
				cli[j].tv = tp;
				ptr = cli[j].buf;

				++requests_srv;
				j = find_srv(cli[j].ret++);
				/* Send request to DNS server */
				send_udp(clisock, ptr, n, 0, (const struct sockaddr *)&srv[j].addr, srv[j].len);
			} else {
				/* Read answer from DNS server */
				n = recv_udp(clisock, buf, BUF_SIZ, 0, (struct sockaddr *)&srv1.addr,
				    &srv1.len, &id);
				j = find_cli(id);
				if (j == -1) {
					logout(1, "Cannot find client (id: %05d)", id);
					continue;
				}

				/* Send answer to client */
				send_udp(sock, buf, n, 0, (const struct sockaddr *)&cli[j].addr, cli[j].len);
				cli[j].inuse = 0;
				--active_clients;
			}
		}

		/* Find timeouted requests */
		active = active_clients;
		for (i = 0; i < clients; i++) {
			if (cli[i].inuse == 1) {
				j = find_timeouted(tp);
				if (j == -1)
					break;

				logout(3, "timeouted client: idx = %03d (id: %05d)", j, cli[j].id);

				cli[j].tv = tp;
				ptr = cli[j].buf;

				++requests_srv;
				j = find_srv(cli[j].ret++);
				/* Send request to DNS server */
				send_udp(clisock, ptr, n, 0, (const struct sockaddr *)&srv[j].addr, srv[j].len);

				while (--active > 0) {
					break;
				}
			}
		}
	}
}

static ssize_t
recv_udp(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen, int *id)
{
	ssize_t n;
	short int dns_id;

	do {
		n = recvfrom(s, buf, len, flags, from, fromlen);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(1, "recvfrom()");
		return(n);
	}

	memcpy(&dns_id, buf, sizeof(short int));
	logout(3, "read %d bytes from %s (id: %05d)", n, inet_ntoa(((struct sockaddr_in *)from)->sin_addr), ntohs(dns_id));
	*id = ntohs(dns_id);

	return(n);
}

static ssize_t
send_udp(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
	ssize_t n;
	short int dns_id;

	do {
		n = sendto(s, buf, len, flags, to, tolen);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(1, "sendto()");
		return(n);
	}

	memcpy(&dns_id, buf, sizeof(short int));
	logout(3, "write %d bytes to %s (id: %05d)", n, inet_ntoa(((struct sockaddr_in *)to)->sin_addr), ntohs(dns_id));

	return(n);
}

static int
find_newcli(void)
{
	int i;

	/* XXX need check for dup requests */

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
		if (cli[i].inuse == 1) {
			if (cli[i].id == id) {
				return(i);
			}
		}
	}

	return(-1);
}

static int
find_srv(int ret)
{
	int i;

	i = srv_ptr[(requests_srv - 1) % weight_factor];
	logout(3, "server #%02d finded (r: %d, factor: %d, weight: %d)", i, requests_srv, weight_factor, srv[i].conf_weight);

	return(i);
}

static int
find_timeout(struct timeval tp)
{
	int i;
	int to;
	int active;
	struct timeval diff;
	struct timeval diff1;

	active = active_clients;

	diff.tv_sec = 0;
	diff.tv_usec = 0;

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 1) {

			logout(3, "client #%03d (id: %05d) time: %d.%06ld", i, cli[i].id, cli[i].tv.tv_sec, cli[i].tv.tv_usec);

			diff1.tv_sec = tp.tv_sec - cli[i].tv.tv_sec;
			if (tp.tv_usec < cli[i].tv.tv_usec) {
				diff1.tv_usec = 1000000 + tp.tv_usec - cli[i].tv.tv_usec;
				diff1.tv_sec = diff1.tv_sec - 1;
			} else {
				diff1.tv_usec = tp.tv_usec - cli[i].tv.tv_usec;
			}

			logout(3, "client #%03d (id: %05d) diff: %d.%06ld", i, cli[i].id, diff1.tv_sec, diff1.tv_usec);

			if (diff1.tv_usec > 0 && diff1.tv_sec >= diff.tv_sec && diff1.tv_usec > diff.tv_usec) {
				diff = diff1;
			}

			while (--active > 0) {
				break;
			}
		}
	}

	logout(3, "max diff: %d.%06ld", diff.tv_sec, diff.tv_usec);

	if (TIMEOUT - diff.tv_usec > 0) {
		to = TIMEOUT - diff.tv_usec;
	} else {
		to = TIMEOUT;
	}

	logout(3, "timeout set: 0.%06d", to);

	return(to);
}

static int
find_timeouted(struct timeval tp)
{
	int i;
	int active;

	active = active_clients;

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 1) {

			logout(3, "client #%03d (id: %05d) timeouted: %d.%06ld", i, cli[i].id, cli[i].tv.tv_sec, cli[i].tv.tv_usec);

			if (tp.tv_sec > cli[i].tv.tv_sec) {
				if (tp.tv_usec < cli[i].tv.tv_usec) {
					if (1000000 + tp.tv_usec > cli[i].tv.tv_usec + TIMEOUT) {
						return(i);
					}
				}
			} else if (tp.tv_sec == cli[i].tv.tv_sec && tp.tv_usec > cli[i].tv.tv_usec + TIMEOUT) {
				return(i);
			}

			while (--active > 0) {
				break;
			}
		}
	}

	return(-1);
}

static void
make_stat(void)
{
	fprintf(stderr,
	    "Requests (in):  %lu\n"
	    "Requests (out): %lu\n",
	    requests_cli,
	    requests_srv);
}
