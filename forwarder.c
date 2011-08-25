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
#include <pwd.h>
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
static client_t **clihash;
static int active_clients;
static int max_active_clients;
static u_long requests_cli;
static u_long requests_srv;
static u_long dropped_requests;
static u_long timeouted_requests;
static char *logfile;
static char *stat_file;
static uid_t uid;
static volatile sig_atomic_t sig_flag;

static void usage(void);
static void srv_init(void);
static void clihash_init(void);
static void sock_init(void);
static void sig_init(void);
static void sig_handler(int signum);
static void mainloop(void);
static void buf_init(buf_t *buf);
static client_t *cli_init(addr_t *addr, buf_t *buf, struct timeval *tv, u_short id);
static int cli_update(client_t *c, struct timeval *tv);
static void cli_del(client_t *c);
static void req_add(client_t *c, struct timeval *tv);
static void req_del(client_t *c);
static ssize_t recv_udp(int sock, buf_t *buf, addr_t *addr, u_short *id);
static ssize_t send_udp(int sock, buf_t *buf, addr_t *addr);
static server_t *find_srv(client_t *c);
static server_t *find_srv_by_addr(addr_t *addr);
static client_t *find_cli(u_short id);
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
	int daemonize_flag;
	struct passwd *pw;

	/* Init global variables */
	print_time_and_level = 0;
	logfd = STDERR_FILENO;

	config = FORWARDER_CONF;
	debug_level = ERR;
	logfile = FORWARDER_LOG;
	syslog_flag = 0;
	daemonize_flag = 1;
	stat_file = NULL;
	uid = 0;

	while ((ch = getopt(argc, argv, "c:d:hl:ns:u:")) != -1) {
		switch (ch) {
		case 'c':
			config = optarg;
			break;
		case 'd':
			debug_level = strtol(optarg, &endp, 10);
			if (*optarg == '\0' || *endp != '\0') {
				logout(CRIT, "Invalid debug level: %s", optarg);
			}
			if (debug_level < MIN_DEBUG_LEVEL || debug_level > MAX_DEBUG_LEVEL) {
				logout(CRIT, "Debug level must be %d..%d", MIN_DEBUG_LEVEL, MAX_DEBUG_LEVEL);
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
		case 'u':
			pw = getpwnam(optarg);
			if (pw == NULL)
				logout(CRIT, "Unknown user: %s", optarg);
			uid = pw->pw_uid;
			break;
		case 'h':
		default:
			usage();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	if (argc != 0)
		usage();

	config_init(config);
	srv_init();
	clihash_init();
	sock_init();
	loginit(logfile);
	if (daemonize_flag)
		if (daemon(0, 0) == -1)
			logerr(CRIT, "Cannot daemonize");
	sig_init();
	mainloop();

	/* NOTREACHED */
	exit(0);
}

static void
usage(void)
{
	logout(CRIT,
	    "Usage: %s [-c config file] [-d debug level] [-l logfile] [-n]\n"
	    "       %*s [-s stat file] [-u username]",
	    getprogname(), strlen(getprogname()), " ");
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
			logerr(CRIT, "inet_aton(%s)", s->name);
	}
}

static void
clihash_init(void)
{
	clihash = calloc(NS_MAXID, sizeof(client_t *));
	if (clihash == NULL)
		logerr(CRIT, "calloc()");
}

static void
sock_init(void)
{
	int rc;
	struct sockaddr_in servaddr;

	/* Listen socket */
	servsock = socket(PF_INET, SOCK_DGRAM, 0);
	if (servsock == -1)
		logerr(CRIT, "socket()");

	nonblock_socket(servsock);

	bzero(&servaddr, sizeof(struct sockaddr_in));
	servaddr.sin_family = PF_INET;
	servaddr.sin_port = htons(PORT);
	if (inet_aton(HOST, &servaddr.sin_addr) == 0)
		logerr(CRIT, "inet_aton(%s)", HOST);

	rc = bind(servsock, (const struct sockaddr *)&servaddr, sizeof(struct sockaddr_in));
	if (rc == -1)
		logerr(CRIT, "bind()");

	if (uid)
		if (setuid(uid) == -1)
			logerr(CRIT, "setuid()");

	/* Client socket */
	clisock = socket(PF_INET, SOCK_DGRAM, 0);
	if (clisock == -1)
		logerr(CRIT, "socket()");

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
	sigaction(SIGUSR1, &act, 0);
	sigaction(SIGUSR2, &act, 0);

	sig_flag = FLAG_NONE;
}

static void
sig_handler(int signum)
{
	switch (signum) {
		case SIGUSR1:
			sig_flag = FLAG_REOPEN;
			break;
		case SIGUSR2:
			if (stat_file)
				sig_flag = FLAG_MKSTAT;
			break;
		default:
			sig_flag = FLAG_EXIT;
			break;
	}
}

static void
mainloop(void)
{
	int i;
	int k;
	int kq;
	int nc;
	ssize_t n;
	u_short id;
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

	ch = calloc(2, sizeof(struct kevent));
	if (ch == NULL)
		logerr(CRIT, "calloc()");

	ev = calloc(2, sizeof(struct kevent));
	if (ev == NULL)
		logerr(CRIT, "calloc()");

	kq = kqueue();
	if (kq == -1)
		logerr(CRIT, "kqueue()");

	buf_init(&buf);

	active_clients = 0;
	max_active_clients = 0;
	requests_cli = 0;
	requests_srv = 0;
	dropped_requests = 0;
	timeouted_requests = 0;

	nc = 0;
	EV_SET(&ch[nc++], servsock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&ch[nc++], clisock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	TAILQ_INIT(&requests);

	/* Main loop */
	for ( ;; ) {
		if (!TAILQ_EMPTY(&requests)) {
			get_timeout(now, &to);
			ts.tv_sec = to.tv_sec;
			ts.tv_nsec = to.tv_usec * 1000;
		}

		do {
			k = kevent(kq, ch, nc, ev, 2, TAILQ_EMPTY(&requests) ? NULL : &ts);

			switch (sig_flag) {
				case FLAG_REOPEN:
					close(logfd);
					loginit(logfile);
					sig_flag = FLAG_NONE;
					break;
				case FLAG_MKSTAT:
					make_stat();
					sig_flag = FLAG_NONE;
					break;
				case FLAG_EXIT:
					free(buf.data);
					free(ev);
					free(ch);
					cleanup();
					/* NOTREACH */
			}
		} while (k == -1 && errno == EINTR);

		if (k == -1)
			logerr(ERR, "kevent()");

		nc = 0;
		gettimeofday(&now, NULL);
		logout(DEBUG, "Now is %d.%ld", now.tv_sec, now.tv_usec);

		for (i = 0; i < k; i++) {
			if (ev[i].ident == servsock) {
				/* Read request from client */
				n = recv_udp(servsock, &buf, &addr, &id);
				if (n < NS_HFIXEDSZ)
					continue;

				c = cli_init(&addr, &buf, &now, id);
				if (c == NULL)
					continue;

				id = htons(c->num % NS_MAXID);
				memcpy(c->buf.data, &id, sizeof(u_short));

				/* Send request to DNS server */
				send_udp(clisock, &c->buf, &c->srv->addr);
				req_add(c, &now);
			} else {
				/* Read answer from DNS server */
				n = recv_udp(clisock, &buf, &addr, &id);
				if (n < NS_HFIXEDSZ)
					continue;

				c = find_cli(id);
				if (c == NULL)
					continue;

				req_del(c);

				c->srv = find_srv_by_addr(&addr);
				if (c->srv == NULL) {
					cli_del(c);
					continue;
				}

				c->srv->recv++;
				if (c->srv->threshold > 0) {
					c->srv->threshold = 0;
					c->srv->skip = 0;
				}
	
				id = htons(c->id);
				memcpy(buf.data, &id, sizeof(u_short));

				/* Send answer to client */
				send_udp(servsock, &buf, &c->addr);
				cli_del(c);
			}
		}

		/* Find timeouted requests */
		while ((c = find_to_cli(now)) != NULL) {
			req_del(c);
			c->srv->threshold++;

			if (cli_update(c, &now) == -1) {
				timeouted_requests++;
				continue;
			}


			/* Send request to DNS server */
			send_udp(clisock, &c->buf, &c->srv->addr);
			req_add(c, &now);
		}
	}
}

static client_t *
cli_init(addr_t *addr, buf_t *buf, struct timeval *tv, u_short id)
{
	client_t *c;

	/* XXX */
	if (clihash[requests_cli % NS_MAXID])
		return (NULL);

	/* Init client */
	c = malloc(sizeof(client_t));
	if (c == NULL)
		logerr(CRIT, "malloc()");
	clihash[requests_cli % NS_MAXID] = c;

	c->id = id;
	c->addr = *addr;
	c->buf.data = buf->data;
	c->buf.len = buf->len;
	c->num = requests_cli;
	c->srv = STAILQ_FIRST(&srvq);

	buf_init(buf);

	requests_cli++;
	active_clients++;
	if (active_clients > max_active_clients)
		max_active_clients = active_clients;

	if (cli_update(c, tv) == -1)
		return (NULL);

	return (c);
}

static int
cli_update(client_t *c, struct timeval *tv)
{
	c->srv = find_srv(c);
	if (c->srv == NULL) {
		cli_del(c);
		return (-1);
	}

	timeradd(tv, &timeout, &c->tv);

	return (0);
}

static void
cli_del(client_t *c)
{
	clihash[c->num % NS_MAXID] = NULL; /* must be NULL here */
	free(c->buf.data);
	free(c);
	
	active_clients--;
}

static void
req_add(client_t *c, struct timeval *tv)
{
	request_t *req;

	req = malloc(sizeof(request_t));
	if (req == NULL)
		logerr(CRIT, "malloc()");
	TAILQ_INSERT_TAIL(&requests, req, next);

	req->cli = c;
	c->req = req;

	c->srv->send++;
	requests_srv++;
}

static void
req_del(client_t *c)
{
	TAILQ_REMOVE(&requests, c->req, next);
	free(c->req);
}

static void
buf_init(buf_t *buf)
{
	buf->len = NS_PACKETSZ;
	buf->data = malloc(buf->len);
	if (buf->data == NULL)
		logerr(CRIT, "malloc()");
}

static ssize_t
recv_udp(int sock, buf_t *buf, addr_t *addr, u_short *id)
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
		logerr(ERR, "recvfrom()");
		return (n);
	}

	if (n < NS_HFIXEDSZ)
		logout(WARN, "Read %d bytes from %s:%d (request too short)",
		    n, inet_ntoa(sin->sin_addr), htons(sin->sin_port));
	else {
		buf->len = n;
		memcpy(&dns_id, buf->data, sizeof(u_short));
		*id = ntohs(dns_id);
		logout(DEBUG, "Read %d bytes from %s:%d (id: %05d)",
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
		logerr(ERR, "sendto()");
		return (n);
	}

	if (n < NS_HFIXEDSZ)
		logout(WARN, "Wrote %d bytes to %s:%d (answer too short)",
		    n, inet_ntoa(sin->sin_addr), htons(sin->sin_port));
	else {
		memcpy(&dns_id, buf->data, sizeof(u_short));
		logout(DEBUG, "Wrote %d bytes to %s:%d (id: %05d)",
		    n, inet_ntoa(sin->sin_addr), htons(sin->sin_port), ntohs(dns_id));
	}

	return (n);
}

static server_t *
find_srv(client_t *c)
{
	server_t *s;

	s = c->srv;

	do {
		if (s->threshold >= s->conf.threshold) {
			if (s->skip >= s->conf.skip) {
				s->threshold = s->conf.threshold - 1; /* XXX */
				s->skip = 0;
				break;
			}
			s->skip++;
		} else
			break;
	} while ((s = STAILQ_NEXT(s, next)) != NULL);

	if (s) {
		logout(DEBUG, "Found server #%02d for request #%lu (id: %05d)",
		    s->id, requests_srv, c->id);
	} else {
		dropped_requests++;
		logout(WARN, "Client #%03d cannot find alive server (id: %05d)", c->num, c->id);
	}

	return (s);
}

static server_t *
find_srv_by_addr(addr_t *addr)
{
	server_t *s;

	STAILQ_FOREACH(s, &srvq, next)
		if (memcmp(&addr->sin.sin_addr, &s->addr.sin.sin_addr, (sizeof(struct in_addr))) == 0)
			return (s);

	logout(ERR, "Unknown server %s:%d", inet_ntoa(addr->sin.sin_addr), htons(addr->sin.sin_port));

	return (NULL);
}

static client_t *
find_cli(u_short id)
{
	return (clihash[id]);
}

static client_t *
find_to_cli(struct timeval tp)
{
	request_t *req;
	client_t *c;

	c = NULL;

	if (!TAILQ_EMPTY(&requests)) {
		req = TAILQ_FIRST(&requests);
		if (timercmp(&tp, &req->cli->tv, >))
			c = req->cli;
	}

	if (c) {
		logout(DEBUG, "Client #%03d timeouted at %d.%06ld (id: %05d)",
		    c->num, c->tv.tv_sec, c->tv.tv_usec, c->id);
	}

	return (c);
}

static void
get_timeout(struct timeval tp, struct timeval *to)
{
	request_t *req;
	struct timeval diff;

	req = TAILQ_FIRST(&requests);

	timersub(&req->cli->tv, &tp, &diff);

	to->tv_sec = diff.tv_sec;
	to->tv_usec = diff.tv_usec;

	logout(DEBUG, "Timeout set to %d.%06d", diff.tv_sec, diff.tv_usec);
}

static void
make_stat(void)
{
	FILE *fh;
	server_t *s;

	fh = fopen(stat_file, "w");
	if (fh == NULL) {
		logerr(ERR, "Cannot open stat file: %s", stat_file);
		return;
	}

	fprintf(fh,
	    "Requests from clients:  %lu\n"
	    "Requests to servers:    %lu\n"
	    "Dropped requests:       %lu\n"
	    "Timeouted requests:     %lu\n"
	    "Active clients:         %d\n"
	    "Max active clients:     %d\n",
	    requests_cli,
	    requests_srv,
	    dropped_requests,
	    timeouted_requests,
	    active_clients,
	    max_active_clients);

	STAILQ_FOREACH(s, &srvq, next)
		fprintf(fh, "%s:%d    send: %8lu recv: %8lu\n", s->name, s->port, s->send, s->recv);

	fclose(fh);
}

static void
cleanup(void)
{
	request_t *req;
	server_t *s;

	while (!TAILQ_EMPTY(&requests)) {
		req = TAILQ_FIRST(&requests);
		TAILQ_REMOVE(&requests, req, next);
		free(req->cli->buf.data);
		free(req->cli);
		free(req);
	}

	free(clihash);

	while (!STAILQ_EMPTY(&srvq)) {
		s = STAILQ_FIRST(&srvq);
		STAILQ_REMOVE_HEAD(&srvq, next);
		free(s);
	}

	exit(0);
}
