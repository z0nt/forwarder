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
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "forwarder.h"
#include "log.h"

static int servsock;
static int clisock;
static int ns_ids;
static client_t *clihash;
static server_t **srvhash;
static int active_clients;
static int max_active_clients;
static u_long requests_cli;
static u_long requests_srv;
static u_long duplicated_requests;
static u_long timeouted_requests;
static u_int weight;
static u_int dweight;
static char *stat_file;
static volatile sig_atomic_t sig_flag;

static void usage(void);
static void srv_init(void);
static void srv_rehash(void);
static void clihash_init(void);
static void sock_init(void);
static void sig_init(void);
static void sig_handler(int signum);
static void mainloop(void);
static void buf_init(buf_t *buf);
static client_t *cli_init(addr_t *addr, buf_t *buf, struct timeval *tv, int id);
static void cli_update(client_t *c, struct timeval *tv);
static void cli_del(client_t *c);
static void req_add(client_t *c, struct timeval *tv);
static void req_del(client_t *c);
static ssize_t recv_udp(int sock, buf_t *buf, addr_t *addr, int *id);
static ssize_t send_udp(int sock, buf_t *buf, addr_t *addr);
static server_t *find_srv(client_t *c);
static server_t *find_srv_by_addr(addr_t *addr);
static client_t *find_cli(int id);
static client_t *find_to_cli(struct timeval tp);
static void get_timeout(struct timeval tp, struct timeval *to);
static void make_stat(void);
static void cleanup(void);

int
main(int argc, char **argv)
{
	int ch;
	char *endp;
	char *config;
	char *logfile;
	int daemonize_flag;

	/* Init global variables */
	print_time_and_level = 0;
	logfd = STDERR_FILENO;
	config = "forwarder.conf";
	debug_level = 3;
	logfile = "forwarder.log";
	daemonize_flag = 1;
	syslog_flag = 0;
	stat_file = NULL;
	ns_ids = NS_MAXID;

	while ((ch = getopt(argc, argv, "c:d:l:ns:")) != -1) {
		switch (ch) {
		case 'c':
			config = optarg;
			break;
		case 'd':
			debug_level = strtol(optarg, &endp, 10);
			if (*optarg == '\0' || *endp != '\0') {
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
			if (strncmp(logfile, "syslog", sizeof("syslog") - 1) == 0)
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
			/* NOTREACHED */
		}
	}

	config_init(config);
	srv_init();
	clihash_init();
	sock_init();
	loginit(logfile);
	if (daemonize_flag)
		if (daemon(0, 0) == -1)
			logerr(EXIT, "Cannot daemonize");
	sig_init();
	mainloop();

	/* NOTREACHED */
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

	dweight = 0;
	weight = 0;
	STAILQ_FOREACH(s, &srvq, next) {
		weight += s->conf.weight;
		s->weight = s->conf.weight;
	}

	srvhash = malloc(sizeof(server_t *) * weight);
	if (srvhash == NULL)
		logerr(EXIT, "malloc()");

	srv_rehash();
}

static void
srv_rehash(void)
{
	int i, n;
	server_t *s;

	n = 0;
	STAILQ_FOREACH(s, &srvq, next) {
		for (i = 0; i < s->weight; i++) {
			srvhash[n] = s;
			n++;
		}
	}
}

static void
clihash_init(void)
{
	int i;

	clihash = malloc(sizeof(client_t) * ns_ids);
	if (clihash == NULL)
		logerr(EXIT, "malloc()");

	for (i = 0; i < ns_ids; i++) {
		clihash[i].req = NULL;
		bzero(&clihash[i].addr.sin, sizeof(struct sockaddr_in));
		clihash[i].addr.len = sizeof(struct sockaddr_in);
	}
}

static void
sock_init(void)
{
	int rc;
	struct sockaddr_in servaddr;

	/* Listen socket */
	servsock = socket(PF_INET, SOCK_DGRAM, 0);
	if (servsock == -1)
		logerr(EXIT, "socket()");

	nonblock_socket(servsock);

	bzero(&servaddr, sizeof(struct sockaddr_in));
	servaddr.sin_family = PF_INET;
	servaddr.sin_port = htons(PORT);
	if (inet_aton(HOST, &servaddr.sin_addr) == 0)
		logerr(EXIT, "inet_aton()");

	rc = bind(servsock, (const struct sockaddr *)&servaddr, sizeof(struct sockaddr_in));
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
	struct timeval now;
	struct timeval to;
	struct timespec ts;
	buf_t buf;
	addr_t addr;
	client_t *c;

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

	buf_init(&buf);

	active_clients = 0;
	max_active_clients = 0;
	requests_cli = 0;
	requests_srv = 0;
	duplicated_requests = 0;
	timeouted_requests = 0;

	nc = 0;
	EV_SET(&ch[nc++], servsock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&ch[nc++], clisock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	STAILQ_INIT(&requests);

	/* Main loop */
	for ( ;; ) {
		if (!STAILQ_EMPTY(&requests)) {
			get_timeout(now, &to);
			ts.tv_sec = to.tv_sec;
			ts.tv_nsec = to.tv_usec * 1000;
		}

		do {
			k = kevent(kq, ch, nc, ev, 2, STAILQ_EMPTY(&requests) ? NULL : &ts);
			if (sig_flag == 1) {
				make_stat();
				sig_flag = 0;
			} else if (sig_flag == 2) {
				free(buf.data); /* isn't safe */
				free(ev);
				free(ch);
				cleanup();
			}
		} while (k == -1 && errno == EINTR);

		if (k == -1)
			logerr(1, "kevent()");

		nc = 0;
		gettimeofday(&now, NULL);
		logout(3, "time: %d.%06ld", now.tv_sec, now.tv_usec);

		if (k == 0)
			logout(3, "kevent() timeouted");

		for (i = 0; i < k; i++) {
			if (ev[i].ident == servsock) {
				/* Read request from client */
				n = recv_udp(servsock, &buf, &addr, &id);
				if (n <= 0)
					continue;

				c = cli_init(&addr, &buf, &now, id);
				buf_init(&buf);

				/* Send request to DNS server */
				send_udp(clisock, &c->buf, &c->srv->addr);
				req_add(c, &now);
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

				c->srv = find_srv_by_addr(&addr);
				if (c->srv != NULL) {
					c->srv->recv++;
					if (autoweight && dweight > 0) {
						c->srv->weight++;				
						dweight--;
						if (dweight == 0)
							srv_rehash();
					}
				} else
					logout(1, "find_srv_by_addr() returns NULL");

				/* Send answer to client */
				send_udp(servsock, &buf, &c->addr);
				cli_del(c);
			}
		}

		/* Find timeouted requests */
		while ((c = find_to_cli(now)) != NULL) {
			if (c->ret > attempts) {
				cli_del(c);

				timeouted_requests++;
				logout(2, "reached max tries: client #%03d (ret: %d); request id: %05d",
				    c->num, c->ret, id);
				continue;
			}

			req_del(c);

			if (autoweight && c->srv->weight > 1) {
				c->srv->weight--;
				dweight++;
			}

			cli_update(c, &now);

			/* Send request to DNS server */
			send_udp(clisock, &c->buf, &c->srv->addr);
			req_add(c, &now);
		}
	}
}

static void
buf_init(buf_t *buf)
{
	buf->len = NS_PACKETSZ;
	buf->data = malloc(buf->len);
	if (buf->data == NULL)
		logerr(EXIT, "malloc()");
}

static client_t *
cli_init(addr_t *addr, buf_t *buf, struct timeval *tv, int id)
{
	client_t *c;

	/* Check for duplicates */
	c = find_cli(id);
	if (c != NULL) {
		free(buf->data); /* FIXME: check address and chaining if new request */
		req_del(c);

		duplicated_requests++;
		logout(1, "duplicate id: %d", id);
		return (c);
	}

	/* Init client */
	c = &clihash[id];

	c->id = id;
	c->ret = 0;
	c->addr = *addr;
	c->buf.data = buf->data;
	c->buf.len = buf->len;
	c->num = requests_cli;

	requests_cli++;
	active_clients++;
	if (active_clients > max_active_clients)
		max_active_clients = active_clients;

	cli_update(c, tv);

	return (c);
}

static void
cli_update(client_t *c, struct timeval *tv)
{
	timeradd(tv, &timeout, &c->tv);
	c->ret++;
	c->srv = find_srv(c);
}

static void
cli_del(client_t *c)
{
	free(c->buf.data);
	active_clients--;

	req_del(c);
}

static void
req_add(client_t *c, struct timeval *tv)
{
	request_t *req;

	req = malloc(sizeof(request_t));
	if (req == NULL)
		logerr(EXIT, "malloc()");
	req->cli = c;
	STAILQ_INSERT_TAIL(&requests, req, next);

	c->req = req;
	c->srv->send++;

	requests_srv++;
}

static void
req_del(client_t *c)
{
	STAILQ_REMOVE(&requests, c->req, request_s, next);
	free(c->req);
	c->req = NULL; /* must be NULL here */
}

static ssize_t
recv_udp(int sock, buf_t *buf, addr_t *addr, int *id)
{
	ssize_t n;
	u_short dns_id;
	struct sockaddr_in *sin;
	socklen_t *len;

	sin = &addr->sin;
	len = &addr->len;

	do {
		n = recvfrom(sock, buf->data, NS_PACKETSZ, 0, (struct sockaddr *)sin, len);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(1, "recvfrom()");
		return (n);
	}

	if (n > 0) { /* FIXME: zero result must be also checked */
		buf->len = n;
		memcpy(&dns_id, buf->data, sizeof(u_short));
		*id = ntohs(dns_id);
		logout(3, "read %d bytes from %s:%d (id: %05d)",
		    n, inet_ntoa(sin->sin_addr), htons(sin->sin_port), *id);
	}

	return (n);
}

static ssize_t
send_udp(int sock, buf_t *buf, addr_t *addr)
{
	ssize_t n;
	u_short dns_id;
	struct sockaddr_in *sin;
	socklen_t *len;

	sin = &addr->sin;
	len = &addr->len;

	do {
		n = sendto(sock, buf->data, buf->len, 0, (const struct sockaddr *)sin, *len);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(1, "sendto()");
		return (n);
	}

	memcpy(&dns_id, buf->data, sizeof(u_short));
	logout(3, "wrote %d bytes to %s:%d (id: %05d)",
	    n, inet_ntoa(sin->sin_addr), htons(sin->sin_port), ntohs(dns_id));

	return (n);
}

static server_t *
find_srv(client_t *c)
{
	server_t *s;

	s = srvhash[requests_srv % weight];
	logout(3, "server #%02d found (id: %d, request: %d, weight: %d/%d)",
	    s->id, c->id, requests_srv, s->conf.weight, weight);

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

	if (clihash[id].req)
		return (&clihash[id]);

	return (NULL);
}

static client_t *
find_to_cli(struct timeval tp)
{
	request_t *req;
	client_t *c;

	c = NULL;

	if (!STAILQ_EMPTY(&requests)) {
		req = STAILQ_FIRST(&requests);
		if (timercmp(&tp, &req->cli->tv, >))
			c = req->cli;
	}

	if (c) {
		logout(3, "client #%03d (id: %05d) timeouted: %d.%06ld",
		    c->num, c->id, c->tv.tv_sec, c->tv.tv_usec);
	}

	return (c);
}

static void
get_timeout(struct timeval tp, struct timeval *to)
{
	request_t *req;
	struct timeval diff;

	req = STAILQ_FIRST(&requests);

	timersub(&req->cli->tv, &tp, &diff);

	to->tv_sec = diff.tv_sec;
	to->tv_usec = diff.tv_usec;

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
		fprintf(fh, "%s: send %zu, recv %zu, weight: %d/%d\n", s->name, s->send, s->recv, s->weight, weight);

	fclose(fh);
}

static void
cleanup(void)
{
	request_t *req;
	server_t *s;

	while (!STAILQ_EMPTY(&requests)) {
		req = STAILQ_FIRST(&requests);
		STAILQ_REMOVE_HEAD(&requests, next);
		free(req->cli->buf.data);
		free(req);
	}

	free(clihash);
	free(srvhash);

	while (!STAILQ_EMPTY(&srvq)) {
		s = STAILQ_FIRST(&srvq);
		STAILQ_REMOVE_HEAD(&srvq, next);
		free(s);
	}

	exit(0);
}
