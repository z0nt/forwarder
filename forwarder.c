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
static int max_active_clients;
static int ns_ids;
static client_t *cli;
static unsigned long requests_cli;
static unsigned long requests_srv;
static unsigned long duplicated_requests;
static unsigned long timeouted_requests;
static unsigned int weight;
static server_t **srvs;
static volatile sig_atomic_t sig_flag;

static void usage(void);
static void srv_init(void);
static void cli_init(void);
static void sock_init(void);
static void sig_init(void);
static void sig_handler(int signum);
static void mainloop(void);
static ssize_t recv_udp(int fd, buf_t *buf, addr_t *addr, int *id);
static ssize_t send_udp(int fd, buf_t *buf, addr_t *addr);
static server_t *find_srv(client_t *c);
static server_t *find_srv_by_addr(addr_t *addr);
static client_t *find_cli(int id);
static client_t *find_newcli(int id);
static client_t *find_to_cli(struct timeval tp);
static void get_timeout(struct timeval tp, struct timespec *to);
static void make_stat(void);
static void cleanup(void);

int
main(int argc, char **argv)
{
	int ch;
	char *endptr;
	char *config;
	char *logfile;
	int daemonize_flag;

	print_time_and_level = 0;
	logfd = STDERR_FILENO;
	debug_level = 3;
	syslog_flag = 0;
	logfile = "forwarder.log";
	daemonize_flag = 1;
	config = "forwarder.conf";
	stat_file = NULL;

	/* XXX */
	ns_ids = NS_MAXID;

	while ((ch = getopt(argc, argv, "c:d:l:ns:")) != -1) {
		switch (ch) {
		case 'c':
			config = optarg;
			break;
		case 'd':
			debug_level = strtol(optarg, &endptr, 10);
			if (*optarg == '\0' || *endptr != '\0') {
				logout(LERR, "Invalid debug level: %s", optarg);
				usage();
			}
			if (debug_level < MIN_DEBUG_LEVEL || debug_level > MAX_DEBUG_LEVEL) {
				logout(LERR, "Debug level must be %d..%d", MIN_DEBUG_LEVEL, MAX_DEBUG_LEVEL);
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
			logerr(EXIT, "Cannot daemonize");
	sig_init();
	mainloop();

	exit(0);
}

static void
usage(void)
{
	logout(EXIT, "usage: %s [-c config file] [-d debug level] [-l logfile] [-n] [-s stat file]",
	    getprogname());
}

static void
srv_init(void)
{
	int i, n;
	server_t *s;

	/* Init sockaddr structures */
	STAILQ_FOREACH(s, &srvq, next) {
		bzero(&s->addr.sin, sizeof(struct sockaddr_in));
		s->addr.len = sizeof(struct sockaddr_in);
		s->addr.sin.sin_family = PF_INET;
		s->addr.sin.sin_port = htons(s->port);
		if (inet_aton(s->name, &s->addr.sin.sin_addr) == 0)
			logerr(EXIT, "inet_aton()");
	}

	weight = 0;
	STAILQ_FOREACH(s, &srvq, next) {
		weight += s->conf_weight;
		s->weight = s->conf_weight;
	}

	srvs = malloc(sizeof(server_t *) * weight);
	if (srvs == NULL)
		logerr(EXIT, "malloc()");

	n = 0;
	STAILQ_FOREACH(s, &srvq, next) {
		for (i = 0; i < s->conf_weight; i++) {
			srvs[n] = s;
			n++;
		}
	}
}

static void
cli_init(void)
{
	int i;

	cli = malloc(sizeof(client_t) * ns_ids);
	if (cli == NULL)
		logerr(EXIT, "malloc()");

	for (i = 0; i < ns_ids; i++) {
		cli[i].inuse = 0;
		bzero(&cli[i].addr.sin, sizeof(struct sockaddr_in));
		cli[i].addr.len = sizeof(struct sockaddr_in);
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
		logerr(EXIT, "socket()");

	nonblock_socket(srvsock);

	bzero(&servaddr, sizeof(struct sockaddr_in));
	servaddr.sin_family = PF_INET;
	servaddr.sin_port = htons(PORT);
	if (inet_aton(HOST, &servaddr.sin_addr) == 0)
		logerr(EXIT, "inet_aton()");

	rc = bind(srvsock, (const struct sockaddr *)&servaddr, sizeof(struct sockaddr_in));
	if (rc == -1)
		logerr(EXIT, "bind()");

	/* Client socket */
	clisock = socket(PF_INET, SOCK_DGRAM, 0);
	if (clisock == -1)
		logerr(EXIT, "socket()");

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

	sig_flag = 0;
}

static void
sig_handler(int signum)
{
	if (signum == SIGINFO) {
		if (stat_file)
			sig_flag = 1;
	} else
		sig_flag = 2;
}

static void
mainloop(void)
{
	int i;
	int k;
	int id;
	int kq;
	int nc;
	ssize_t n;
	struct kevent *ch;
	struct kevent *ev;
	struct timespec *top;
	struct timespec to;
	struct timeval tp;
	buf_t buf;
	addr_t addr;
	client_t *c;
	clientq_t *cq;

	/* Initialization */
	bzero(&addr.sin, sizeof(struct sockaddr_in));
	addr.len = sizeof(struct sockaddr_in);

	ch = calloc(1, sizeof(struct kevent) * 2);
	if (ch == NULL)
		logerr(EXIT, "calloc()");

	ev = calloc(1, sizeof(struct kevent) * 2);
	if (ev == NULL)
		logerr(EXIT, "calloc()");

	kq = kqueue();
	if (kq == -1)
		logerr(EXIT, "kqueue()");

	buf.len = NS_PACKETSZ;
	buf.data = malloc(buf.len);
	if (buf.data == NULL)
		logerr(EXIT, "malloc()");

	active_clients = 0;
	requests_cli = 0;
	requests_srv = 0;

	nc = 0;
	EV_SET(&ch[nc++], srvsock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&ch[nc++], clisock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	STAILQ_INIT(&cliq);

	/* Main loop */
	for ( ;; ) {
		if (active_clients == 0) {
			top = NULL;
		} else {
			get_timeout(tp, &to);
			top = &to;
		}

		do {
			k = kevent(kq, ch, nc, ev, 2 /* number of sockets */, top);
			if (sig_flag == 1) {
				make_stat();
				sig_flag = 0;
			} else if (sig_flag == 2) {
				free(ev);
				free(ch);
				cleanup();
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
			buf.len = NS_PACKETSZ; /* XXX */
			if (ev[i].ident == srvsock) {
				/* Read request from client */
				n = recv_udp(srvsock, &buf, &addr, &id);
				if (n <= 0)
					continue;

				c = find_newcli(id);
				if (c == NULL)
					continue;

				memcpy(&c->addr, &addr, sizeof(addr_t));
				c->id = id;
				c->buf.data = buf.data;
				c->buf.len = n;

				buf.data = malloc(buf.len);
				if (buf.data == NULL)
					logerr(EXIT, "malloc()");

				requests_cli++;
				c->tv = tp;
				c->ret++;
				c->srv = find_srv(c);

				/* Send request to DNS server */
				send_udp(clisock, &c->buf, &c->srv->addr);
				c->srv->send++;
				requests_srv++;

				cq = malloc(sizeof(clientq_t));
				if (cq == NULL)
					logerr(EXIT, "malloc()");
				c->cq = cq;
				cq->cli = c;
				STAILQ_INSERT_TAIL(&cliq, cq, next);
			} else {
				/* Read answer from DNS server */
				n = recv_udp(clisock, &buf, &addr, &id);
				if (n <= 0)
					continue;

				c = find_cli(id);
				if (c == NULL) {
					logout(2, "Cannot find client (id: %05d)", id);
					continue;
				}

				buf.len = n;
				c->srv = find_srv_by_addr(&addr);
				if (c->srv != NULL)
					c->srv->recv++;
				else
					logout(1, "find_srv_by_addr() returns NULL");

				/* Send answer to client */
				send_udp(srvsock, &buf, &c->addr);
				free(c->buf.data);
				c->inuse = 0;
				active_clients--;

				STAILQ_REMOVE(&cliq, c->cq, clientq_s, next);
				free(c->cq);
			}
		}

		/* Find timeouted requests */
		while ((c = find_to_cli(tp)) != NULL) {
			if (c->ret > attempts) {
				free(c->buf.data);
				c->inuse = 0;
				active_clients--;
				timeouted_requests++;
				logout(2, "reached max tries: client #%03d (ret: %d); request id: %05d",
				    c->num, c->ret, id);
				continue;
			}

			logout(3, "client #%03d (id: %05d) timeouted", c->num, c->id);

			if (c->srv->weight > 1)
				c->srv->weight--;

			c->tv = tp;
			c->ret++;
			c->srv = find_srv(c);

			/* Send request to DNS server */
			send_udp(clisock, &c->buf, &c->srv->addr);
			c->srv->send++;
			requests_srv++;

			cq = malloc(sizeof(clientq_t));
			if (cq == NULL)
				logerr(EXIT, "malloc()");
			c->cq = cq;
			cq->cli = c;
			STAILQ_INSERT_TAIL(&cliq, cq, next);
		}
	}
}

static ssize_t
recv_udp(int fd, buf_t *buf, addr_t *addr, int *id)
{
	ssize_t n;
	short int dns_id;
	struct sockaddr_in *sin;
	socklen_t *len;

	sin = &addr->sin;
	len = &addr->len;

	do {
		n = recvfrom(fd, buf->data, buf->len, 0, (struct sockaddr *)sin, len);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(1, "recvfrom()");
		return (n);
	}

	memcpy(&dns_id, buf->data, sizeof(short int));
	*id = ntohs(dns_id);
	logout(3, "read %d bytes from %s (id: %05d)", n, inet_ntoa(sin->sin_addr), *id);

	return (n);
}

static ssize_t
send_udp(int fd, buf_t *buf, addr_t *addr)
{
	ssize_t n;
	short int dns_id;
	struct sockaddr_in *sin;
	socklen_t *len;

	sin = &addr->sin;
	len = &addr->len;

	do {
		n = sendto(fd, buf->data, buf->len, 0, (const struct sockaddr *)sin, *len);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(1, "sendto()");
		return (n);
	}

	memcpy(&dns_id, buf->data, sizeof(short int));
	logout(3, "wrote %d bytes to %s (id: %05d)", n, inet_ntoa(sin->sin_addr), ntohs(dns_id));

	return (n);
}

static server_t *
find_srv(client_t *c)
{
	server_t *s;

	s = srvs[(requests_srv - 1) % weight];
	logout(3, "server #%02d found (id: %d, request: %d, weight: %d/%d)",
	    s->id, c->id, requests_srv, s->conf_weight, weight);

	return (s);
}

static server_t *
find_srv_by_addr(addr_t *addr)
{
	server_t *s;

	STAILQ_FOREACH(s, &srvq, next)
		if (memcmp(&addr->sin.sin_addr, &s->addr.sin.sin_addr, (sizeof(struct in_addr))) == 0)
			return (s);

	return (NULL);
}

static client_t *
find_cli(int id)
{
	if (id < 0 || id > (ns_ids - 1)) {
		logout(1, "Wrong request id: %d", id);
		return (NULL);
	}

	if (cli[id].inuse == 1)
		return (&cli[id]);

	return (NULL);
}

static client_t *
find_newcli(int id)
{
	client_t *c;

	if (id < 0 || id > (ns_ids - 1)) {
		logout(1, "Wrong request id: %d", id);
		return (NULL);
	}

	c = find_cli(id);
	if (c != NULL) {
		logout(1, "duplicate id: %d", id);
		free(c->buf.data);
		STAILQ_REMOVE(&cliq, c->cq, clientq_s, next);
		free(c->cq);
		duplicated_requests++;
		return (c);
	}

	cli[id].inuse = 1;
	cli[id].ret = 0;
	cli[id].num = requests_cli;

	active_clients++;
	if (active_clients > max_active_clients)
		max_active_clients = active_clients;

	return (&cli[id]);
}

static client_t *
find_to_cli(struct timeval tp)
{
	clientq_t *cq;
	client_t *c;
	struct timeval tv;

	c = NULL;

	if (!STAILQ_EMPTY(&cliq)) {
		cq = STAILQ_FIRST(&cliq);
		timeradd(&cq->cli->tv, &timeout, &tv);
		if (timercmp(&tp, &tv, >))
			c = cq->cli;
	}

	if (c) {
		STAILQ_REMOVE_HEAD(&cliq, next);
		free(cq);

		logout(3, "client #%03d (id: %05d) timeouted: %d.%06ld",
		    c->num, c->id, c->tv.tv_sec, c->tv.tv_usec);
	}

	return (c);
}

static void
get_timeout(struct timeval tp, struct timespec *to)
{
	clientq_t *cq;
	struct timeval tv;
	struct timeval diff;

	cq = STAILQ_FIRST(&cliq);

	timeradd(&cq->cli->tv, &timeout, &tv);
	timersub(&tv, &tp, &diff);

	to->tv_sec = diff.tv_sec;
	to->tv_nsec = diff.tv_usec * 1000;

	logout(3, "timeout set: %d.%06d", diff.tv_sec, diff.tv_usec);
}

static void
make_stat(void)
{
	FILE *fh;
	server_t *s;

	fh = fopen(stat_file, "w");
	if (fh == NULL) {
		logerr(1, "Cannot open stat file: %s", stat_file);
		return;
	}

	fprintf(fh,
	    "Requests from clients:  %lu\n"
	    "Requests to servers:    %lu\n"
	    "Duplicated requests:    %lu\n"
	    "Timeouted requests:     %lu\n"
	    "Active clients:         %d\n"
	    "Max active clients:     %d\n",
	    requests_cli,
	    requests_srv,
	    duplicated_requests,
	    timeouted_requests,
	    active_clients,
	    max_active_clients);

	STAILQ_FOREACH(s, &srvq, next)
		fprintf(fh, "%s: send %zu, recv %zu, weight: %d\n", s->name, s->send, s->recv, s->weight);

	fclose(fh);
}

static void
cleanup(void)
{
	clientq_t *cq;
	server_t *s;

	while (!STAILQ_EMPTY(&cliq)) {
		cq = STAILQ_FIRST(&cliq);
		STAILQ_REMOVE_HEAD(&cliq, next);
		free(cq->cli->buf.data);
		free(cq);
	}

	free(cli);
	free(srvs);

	while (!STAILQ_EMPTY(&srvq)) {
		s = STAILQ_FIRST(&srvq);
		STAILQ_REMOVE_HEAD(&srvq, next);
		free(s);
	}

	exit(0);
}
