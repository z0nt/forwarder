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

static char *stat_file;
static int srvsock;
static int clisock;
static int active_clients;
static int clients;
static client_t *cli;
static unsigned long requests_cli;
static unsigned long requests_srv;
static unsigned int weight;
static int *srvs;
static int stat_flag = 0;

static void usage(void);
static void srv_init(void);
static void cli_init(void);
static void sock_init(void);
static void sig_init(void);
static void sig_handler(int signum);
static void mainloop(void);
static ssize_t recv_udp(int s, void *buf, size_t len, sock_t *sock, int *id);
static ssize_t send_udp(int s, const void *buf, size_t len, sock_t *sock);
static int find_newcli(client_t *c);
static int find_cli(int id);
static int find_srv(client_t *c);
static int find_timeout(struct timeval tp);
static int find_timeouted(struct timeval tp);
static void make_stat(void);

int
main(int argc, char **argv)
{
	int ch;
	char *endptr;
	char *config;
	char *logfile;
	int daemonize_flag;

	debug_level = 3;
	syslog_flag = 0;
	logfile = "forwarder.log";
	daemonize_flag = 1;
	config = "forwarder.conf";
	stat_file = NULL;

	while ((ch = getopt(argc, argv, "c:d:l:ns:")) != -1) {
		switch (ch) {
		case 'c':
			config = optarg;
			break;
		case 'd':
			debug_level = strtol(optarg, &endptr, 10);
			if (*optarg == '\0' || *endptr != '\0') {
				fprintf(stderr, "Invalid debug level: %s\n", optarg);
				usage();
			}
			if (debug_level < MIN_DEBUG_LEVEL || debug_level > MAX_DEBUG_LEVEL) {
				fprintf(stderr, "Debug level must be %d..%d\n", MIN_DEBUG_LEVEL, MAX_DEBUG_LEVEL);
				usage();
			}

			break;
		case 'l':
			logfile = optarg;
			if (!strcmp(logfile, "syslog"))
				syslog_flag = 1;
			else
				syslog_flag = 0;

			break;
		case 'n':
			daemonize_flag = 0;
			break;
		case 's':
			stat_file = optarg;
			break;
		default:
			usage();
			break;
		}
	}

	config_init(config);
	srv_init();
	cli_init();
	sock_init();
	loginit(logfile);
	if (daemonize_flag)
		if (daemon(0, 0) == -1)
			err(1, "Cannot daemonize");
	sig_init();
	mainloop();

	exit(0);
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-c config file] [-d debug level] [-l logfile] [-n] [-s stat file]\n",
	    getprogname());
	exit(1);
}

static void
srv_init(void)
{
	int i, j, n;

	/* Init sockaddr structures */
	for (i = 0; i < servers; i++) {
		bzero(&srv[i].sock.addr, sizeof(struct sockaddr_in));
		srv[i].sock.len = sizeof(struct sockaddr_in);
		srv[i].sock.addr.sin_family = PF_INET;
		srv[i].sock.addr.sin_port = htons(srv[i].port);
		srv[i].sock.addr.sin_addr.s_addr = inet_addr(srv[i].name);
		if (srv[i].sock.addr.sin_addr.s_addr == INADDR_NONE)
			err(1, "inet_addr()");
	}

	weight = 0;
	for (i = 0; i < servers; i++)
		weight += srv[i].conf_weight;

	srvs = malloc(sizeof(int) * weight);
	if (srvs == NULL)
		err(1, "malloc()");

	n = 0;
	for (i = 0; i < servers; i++) {
		for (j = 0; j < srv[i].conf_weight; j++) {
			srvs[n] = i;
			n++;
		}
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
		bzero(&cli[i].sock.addr, sizeof(struct sockaddr_in));
		cli[i].sock.len = sizeof(struct sockaddr_in);
	}
}

static void
sock_init(void)
{
	int rc;
	struct sockaddr_in servaddr;

	/* Listen socket */
	srvsock = socket(PF_INET, SOCK_DGRAM, 0);
	if (srvsock == -1)
		err(1, "socket()");

	nonblock_socket(srvsock);

	bzero(&servaddr, sizeof(struct sockaddr_in));
	servaddr.sin_family = PF_INET;
	servaddr.sin_port = htons(PORT);
	servaddr.sin_addr.s_addr = inet_addr(HOST);
	if (servaddr.sin_addr.s_addr == -1)
		err(1, "inet_addr()");

	rc = bind(srvsock, (const struct sockaddr *)&servaddr, sizeof(struct sockaddr_in));
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
	sigset_t sigmask;
	struct sigaction act;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGHUP);
	sigaddset(&sigmask, SIGPIPE);
	sigprocmask(SIG_BLOCK, &sigmask, NULL);

	act.sa_flags = 0;
	act.sa_handler = sig_handler;
	sigemptyset(&act.sa_mask);
	sigaction(SIGTERM, &act, 0);
	sigaction(SIGINT, &act, 0);
	sigaction(SIGINFO, &act, 0);
}

static void
sig_handler(int signum)
{
	if (signum == SIGINFO) {
		if (stat_file)
			stat_flag = 1;
	} else {
		/* XXX */
		exit(0);
	}
}

static void
mainloop(void)
{
	int i;
	int k;
	int id;
	int kq;
	int nc;
	int to;
	int cli_idx;
	int srv_idx;
	int active;
	ssize_t n;
	void *ptr;
	char buf[BUF_SIZ];
	struct kevent *ch;
	struct kevent *ev;
	struct timespec *timeout;
	struct timespec tv;
	struct timeval tp;
	client_t cli1;
	server_t srv1;

	/* Initialization */
	bzero(&cli1.sock.addr, sizeof(struct sockaddr_in));
	cli1.sock.len = sizeof(struct sockaddr_in);
	bzero(&srv1.sock.addr, sizeof(struct sockaddr_in));
	srv1.sock.len = sizeof(struct sockaddr_in);

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
	EV_SET(&ch[nc++], srvsock, EVFILT_READ, EV_ADD, 0, 0, NULL);
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

		if (k == 0)
			logout(3, "kevent() timeouted");

		for (i = 0; i < k; i++) {
			if (ev[i].ident == srvsock) {
				/* Read request from client */
				n = recv_udp(srvsock, buf, BUF_SIZ, &cli1.sock, &cli1.id);
				if (n <= 0)
					continue;

				cli_idx = find_newcli(&cli1);
				if (cli_idx == -1) {
					logout(1, "Max clients reached");
					continue;
				}

				cli[cli_idx].id = cli1.id;
				memcpy(&cli[cli_idx].sock, &cli1.sock, sizeof(sock_t));
				memcpy(cli[cli_idx].buf, buf, n);

				requests_cli++;
				cli[cli_idx].tv = tp;
				ptr = cli[cli_idx].buf;

				/* Send request to DNS server */
				requests_srv++;
				cli[cli_idx].ret++;
				srv_idx = find_srv(&cli[cli_idx]);
				cli[cli_idx].srv = &srv[srv_idx];
				cli[cli_idx].srv->send++;
				send_udp(clisock, ptr, n, &srv[srv_idx].sock);
			} else {
				/* Read answer from DNS server */
				n = recv_udp(clisock, buf, BUF_SIZ, &srv1.sock, &id);
				if (n <= 0)
					continue;

				cli_idx = find_cli(id);
				if (cli_idx == -1) {
					logout(1, "Cannot find client (id: %05d)", id);
					continue;
				}

				cli[cli_idx].srv->recv++;

				if (strncmp(cli[cli_idx].srv->name, "95.108.142.2", IP_LEN) == 0)
					logout(1, "Client id: %03d (ret: %d); request id: %05d",
					    cli_idx, cli[cli_idx].ret, id);

				/* Send answer to client */
				send_udp(srvsock, buf, n, &cli[cli_idx].sock);
				cli[cli_idx].inuse = 0;
				active_clients--;
			}
		}

		/* Find timeouted requests */
		active = active_clients;
		for (i = 0; i < clients; i++) {
			if (cli[i].inuse == 1) {
				cli_idx = find_timeouted(tp);
				if (cli_idx == -1)
					continue;

				logout(3, "client #%03d (id: %05d) timeouted", cli[cli_idx].num, cli[cli_idx].id);

				cli[cli_idx].tv = tp;
				ptr = cli[cli_idx].buf;

				/* Send request to DNS server */
				requests_srv++;
				cli[cli_idx].ret++;
				srv_idx = find_srv(&cli[cli_idx]);
				cli[cli_idx].srv = &srv[srv_idx];
				cli[cli_idx].srv->send++;

				send_udp(clisock, ptr, n, &srv[srv_idx].sock);

				while (--active > 0) {
					break;
				}
			}
		}
	}
}

static ssize_t
recv_udp(int s, void *buf, size_t len, sock_t *sock, int *id)
{
	ssize_t n;
	short int dns_id;
	struct sockaddr_in *from;
	socklen_t *fromlen;

	from = &(sock->addr);
	fromlen = &(sock->len);

	do {
		n = recvfrom(s, buf, len, 0, (struct sockaddr *)from, fromlen);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(1, "recvfrom()");
		return (n);
	}

	memcpy(&dns_id, buf, sizeof(short int));
	*id = ntohs(dns_id);
	logout(3, "read %d bytes from %s (id: %05d)", n, inet_ntoa(from->sin_addr), *id);

	return (n);
}

static ssize_t
send_udp(int s, const void *buf, size_t len, sock_t *sock)
{
	ssize_t n;
	short int dns_id;
	struct sockaddr_in *to;
	socklen_t tolen;

	to = &(sock->addr);
	tolen = sock->len;

	do {
		n = sendto(s, buf, len, 0, (const struct sockaddr *)to, tolen);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(1, "sendto()");
		return (n);
	}

	memcpy(&dns_id, buf, sizeof(short int));
	logout(3, "wrote %d bytes to %s (id: %05d)", n, inet_ntoa(to->sin_addr), ntohs(dns_id));

	return (n);
}

static int
find_newcli(client_t *c)
{
	int i;

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 1 && cli[i].id == c->id) {
			logout(1, "duplicate id: %d", c->id);
			return (i);
		}
	}

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 0) {
			cli[i].inuse = 1;
			cli[i].ret = 0;
			cli[i].num = requests_cli;
			active_clients++;
			return (i);
		}
	}

	return (-1);
}

static int
find_cli(int id)
{
	int i;

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 1) {
			if (cli[i].id == id) {
				return (i);
			}
		}
	}

	return (-1);
}

static int
find_srv(client_t *c)
{
	int i;

	i = srvs[(requests_srv - 1) % weight];
	logout(3, "server #%02d found (id: %d, request: %d, weight: %d/%d)",
	    i, c->id, requests_srv, srv[i].conf_weight, weight);

	return (i);
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

			logout(3, "client #%03d (id: %05d) time: %d.%06ld",
			    cli[i].num, cli[i].id, cli[i].tv.tv_sec, cli[i].tv.tv_usec);

			diff1.tv_sec = tp.tv_sec - cli[i].tv.tv_sec;
			if (tp.tv_usec < cli[i].tv.tv_usec) {
				diff1.tv_usec = 1000000 + tp.tv_usec - cli[i].tv.tv_usec;
				diff1.tv_sec--;
			} else {
				diff1.tv_usec = tp.tv_usec - cli[i].tv.tv_usec;
			}

			logout(3, "client #%03d (id: %05d) diff: %d.%06ld",
			    cli[i].num, cli[i].id, diff1.tv_sec, diff1.tv_usec);

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

	return (to);
}

static int
find_timeouted(struct timeval tp)
{
	int i;
	int active;

	active = active_clients;

	for (i = 0; i < clients; i++) {
		if (cli[i].inuse == 1) {

			logout(3, "client #%03d (id: %05d) timeouted: %d.%06ld",
			    cli[i].num, cli[i].id, cli[i].tv.tv_sec, cli[i].tv.tv_usec);

			if (tp.tv_sec > cli[i].tv.tv_sec) {
				if (tp.tv_usec < cli[i].tv.tv_usec) {
					if (1000000 + tp.tv_usec > cli[i].tv.tv_usec + TIMEOUT) {
						return (i);
					}
				}
			} else if (tp.tv_sec == cli[i].tv.tv_sec && tp.tv_usec > cli[i].tv.tv_usec + TIMEOUT) {
				return (i);
			}

			while (--active > 0) {
				break;
			}
		}
	}

	return (-1);
}

static void
make_stat(void)
{
	int i;
	FILE *fh;

	fh = fopen(stat_file, "w");
	if (fh == NULL) {
		logerr(1, "Cannot open stat file: %s", stat_file);
		return;
	}

	fprintf(fh,
	    "Requests from clients:  %lu\n"
	    "Requests to servers:    %lu\n"
	    "Active clients:         %d\n",
	    requests_cli,
	    requests_srv,
	    active_clients);

	for (i = 0; i < servers; i++)
		fprintf(fh, "%s: send %zu; recv %zu\n", srv[i].name, srv[i].send, srv[i].recv);

	fclose(fh);
}
