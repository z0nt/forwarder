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
#include <sys/queue.h>

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

typedef struct buf_s buf_t;
typedef struct sock_s sock_t;
typedef struct client_s client_t;
typedef struct request_s request_t;

struct buf_s {
	char *data;	/* Pointer to data */
	off_t off;	/* Received or sent bytes */
	ssize_t len;	/* Buffer length */
};

struct sock_s {
	int fd;		/* Socket descriptor number */
	int connected;	/* TCP state */
	buf_t buf;	/* Buffer */
	addr_t addr;	/* TCP address */
	server_t *srv;	/* TCP server to which this socket connected */
	void (*handler)(struct kevent *, struct timeval *);
};

struct client_s {
	u_short id;	/* DNS ID */
	u_long num;	/* Request's number */
	addr_t addr;	/* Client's address */
	buf_t buf;	/* Buffer */
	sock_t sock;	/* XXX why isn't pointer */
	server_t *srv;	/* Pointer to server */
	request_t *req;	/* Pointer to element in request queue */
	struct timeval tv;	/* XXX timeout? */
};

struct request_s {
	client_t *cli;
	TAILQ_ENTRY(request_s) next;
};
TAILQ_HEAD(, request_s) requests;
TAILQ_HEAD(, request_s) in_requests;
TAILQ_HEAD(, request_s) out_requests;

static int max_sock;
static sock_t *sockhash;
static int sock_udp_listen;
static int sock_udp_cli;
static client_t **clihash;
static int active_clients;
static int max_active_clients;
static u_long requests_cli;
static u_long requests_srv;
static u_long dropped_requests;
static u_long timeouted_requests;
static char *logfile;
static char *stat_file;
static volatile sig_atomic_t sig_flag;
static int nchanges;
static int nevents;
static struct kevent *chlist;
static struct kevent *evlist;

static void usage(void);
static void srv_init(void);
static void clihash_init(void);
static void events_init(void);
static int sock_create(int domain, int type, addr_t *addr);
static int sock_connect(server_t *s);
static void sock_init(void);
static void sig_init(void);
static void sig_handler(int signum);
static void mainloop(void);
static void handler_udp_listen(struct kevent *ev, struct timeval *now);
static void handler_udp_client(struct kevent *ev, struct timeval *now);
static void handler_tcp_listen(struct kevent *ev, struct timeval *now);
static void handler_tcp_in(struct kevent *ev, struct timeval *now);
static void handler_tcp_out(struct kevent *ev, struct timeval *now);
static void handler_timeout(struct timeval *now);
static client_t *cli_init(addr_t *addr, buf_t *buf, struct timeval *tv, u_short id);
static int cli_update(client_t *c, struct timeval *tv);
static void cli_del(client_t *c);
static int req_add(client_t *c, struct timeval *tv);
static void req_del(client_t *c);
static void *buf_init(buf_t *buf, size_t size);
static ssize_t recv_udp(int sock, buf_t *buf, addr_t *addr, u_short *id);
static ssize_t send_udp(int sock, buf_t *buf, addr_t *addr);
static ssize_t recv_len(int sock, u_short *len);
static ssize_t recv_tcp(int sock, buf_t *buf);
static ssize_t send_tcp(int sock, buf_t *buf);
static server_t *find_srv(client_t *c);
static server_t *find_srv_by_addr(addr_t *addr);
static client_t *find_cli(u_short id);
static client_t *find_to_cli(struct timeval *tp);
static void get_timeout(struct timeval *tp, struct timeval *to);
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
	uid_t uid;

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
	events_init();
	sock_init();
	if (uid)
		if (setuid(uid) == -1)
			logerr(CRIT, "setuid()");
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
	STAILQ_FOREACH(s, &servers, next) {
		s->addr.len = sizeof(struct sockaddr_in);
		bzero(&s->addr.sin, sizeof(struct sockaddr_in));
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
		logerr(CRIT, "calloc(clihash)");
}

static void
events_init(void)
{

	nevents = 512; /* XXX */

	chlist = calloc(nevents, sizeof(struct kevent));
	if (chlist == NULL)
		logerr(CRIT, "calloc(chlist)");

	evlist = calloc(nevents, sizeof(struct kevent));
	if (evlist == NULL)
		logerr(CRIT, "calloc(evlist)");

	nchanges = 0;
}

static int
sock_create(int domain, int type, addr_t *addr)
{
	int sock;
	int reuseaddr;

	sock = socket(domain, type, 0);
	if (sock == -1) {
		logerr(ERR, "socket()");
		return (-1);
	}

	if (sock >= max_sock) {
		logerr(ERR, "Max sock reached: %d", max_sock);
		goto error;
	}

	nonblock_socket(sock);

	if (addr != NULL) {
		if (type == SOCK_STREAM) {
			reuseaddr = 1;
			if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int)) == -1) {
				logerr(ERR, "setsockopt()");
				goto error;
			}
		}

		if (bind(sock, (const struct sockaddr *)&addr->sin, addr->len) == -1) {
			logerr(ERR, "bind()");
			goto error;
		}

		if (type == SOCK_STREAM) {
			if (listen(sock, -1) == -1) {
				logerr(ERR, "listen()");
				goto error;
			}
		}
	}

	return (sock);

error:
	close(sock);
	return (-1);
}

static int
sock_connect(server_t *s)
{
	int sock;

	sock = sock_create(PF_INET, SOCK_STREAM, NULL);
	if (sock == -1)
		return (-1);


	if (connect(sock, (const struct sockaddr *)&s->addr.sin, s->addr.len) == -1 &&
	    errno != EINPROGRESS) {
		logerr(ERR, "connect()");
		close(sock);
		return (-1);
	}

	EV_SET(&chlist[nchanges++], sock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&chlist[nchanges++], sock, EVFILT_WRITE, EV_ADD, 0, 0, NULL);

	sockhash[sock].fd = sock;
	sockhash[sock].connected = 0;
	sockhash[sock].handler = handler_tcp_out;
	sockhash[sock].srv = s;

	return (sock);
}

static void
sock_init(void)
{
	int sock;
	addr_t addr;
	server_t *s;

	max_sock = 1024; /* XXX */
	sockhash = calloc(max_sock, sizeof(sock_t));
	if (sockhash == NULL)
		logerr(CRIT, "calloc(sockhash)");

	/* UDP listen socket */
	addr.len = sizeof(struct sockaddr_in);
	bzero(&addr.sin, sizeof(struct sockaddr_in));
	addr.sin.sin_family = PF_INET;
	addr.sin.sin_port = htons(PORT);
	if (inet_aton(HOST, &addr.sin.sin_addr) == 0)
		logerr(CRIT, "inet_aton(%s)", HOST);

	sock = sock_create(PF_INET, SOCK_DGRAM, &addr);
	if (sock == -1)
		logout(CRIT, "Cannot create UDP listen socket");

	EV_SET(&chlist[nchanges++], sock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	sockhash[sock].fd = sock;
	sockhash[sock].handler = handler_udp_listen;

	sock_udp_listen = sock;

	/* UDP client socket */
	sock = sock_create(PF_INET, SOCK_DGRAM, NULL);
	if (sock == -1)
		logout(CRIT, "Cannot create UDP client socket");

	EV_SET(&chlist[nchanges++], sock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	sockhash[sock].fd = sock;
	sockhash[sock].handler = handler_udp_client;

	sock_udp_cli = sock;

	if (buf_init(&sockhash[sock].buf, NS_PACKETSZ) == NULL)
		logout(CRIT, "Cannot init UDP client socket");

	/* TCP listen socket */
	addr.len = sizeof(struct sockaddr_in);
	bzero(&addr.sin, sizeof(struct sockaddr_in));
	addr.sin.sin_family = PF_INET;
	addr.sin.sin_port = htons(PORT);
	if (inet_aton(HOST, &addr.sin.sin_addr) == 0)
		logerr(CRIT, "inet_aton(%s)", HOST);

	sock = sock_create(PF_INET, SOCK_STREAM, &addr);
	if (sock == -1)
		logout(CRIT, "Cannot create TCP listen socket");

	EV_SET(&chlist[nchanges++], sock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	sockhash[sock].fd = sock;
	sockhash[sock].handler = handler_tcp_listen;

	/* TCP client socket */
	STAILQ_FOREACH(s, &servers, next) {
		sock = sock_connect(s);
		if (sock == -1)
			continue;
		s->fd = sock;
	}
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
	struct timeval now;
	struct timeval to;
	struct timespec ts;
	sock_t *sock;

	/* Initialization */
	kq = kqueue();
	if (kq == -1)
		logerr(CRIT, "kqueue()");

	active_clients = 0;
	max_active_clients = 0;
	requests_cli = 0;
	requests_srv = 0;
	dropped_requests = 0;
	timeouted_requests = 0;

	TAILQ_INIT(&requests);
	TAILQ_INIT(&in_requests);
	TAILQ_INIT(&out_requests);

	/* Main loop */
	for ( ;; ) {
		if (!TAILQ_EMPTY(&requests)) {
			get_timeout(&now, &to);
			ts.tv_sec = to.tv_sec;
			ts.tv_nsec = to.tv_usec * 1000;
		}

		do {
			k = kevent(kq, chlist, nchanges, evlist, nevents, TAILQ_EMPTY(&requests) ? NULL : &ts);

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
				cleanup();
				/* NOTREACH */
			}
		} while (k == -1 && errno == EINTR);

		if (k == -1)
			logerr(ERR, "kevent()");

		nchanges = 0;
		gettimeofday(&now, NULL);
		logout(DEBUG, "Now is %d.%06ld", now.tv_sec, now.tv_usec);

		for (i = 0; i < k; i++) {
			if (evlist[i].ident >= max_sock) {
				logerr(ERR, "Unknown socket: %d", evlist[i].ident);
				continue;
			}

			sock = &sockhash[evlist[i].ident];
			sock->handler(&evlist[i], &now);
		}

		handler_timeout(&now);
	}
}

static void
handler_udp_listen(struct kevent *ev, struct timeval *now)
{
	ssize_t n;
	addr_t addr;
	u_short id;
	client_t *c;
	sock_t *sock;

	sock = &sockhash[ev->ident];

	if (sock->buf.data == NULL)
		if (buf_init(&sock->buf, NS_PACKETSZ) == NULL)
			return;

	/* Read request from client */
	n = recv_udp(sock_udp_listen, &sock->buf, &addr, &id);
	if (n < NS_HFIXEDSZ)
		return;

	c = cli_init(&addr, &sock->buf, now, id);
	if (c == NULL)
		return;

	id = htons(c->num % NS_MAXID);
	memcpy(c->buf.data, &id, sizeof(u_short));

	/* Send request to DNS server */
	send_udp(sock_udp_cli, &c->buf, &c->srv->addr);
	req_add(c, now);
}

static void
handler_udp_client(struct kevent *ev, struct timeval *now)
{
	ssize_t n;
	addr_t addr;
	u_short id;
	client_t *c;
	sock_t *sock;

	sock = &sockhash[ev->ident];

	/* Read answer from DNS server */
	n = recv_udp(sock_udp_cli, &sock->buf, &addr, &id);
	if (n < NS_HFIXEDSZ)
		return;

	c = find_cli(id);
	if (c == NULL)
		return;

	req_del(c);

	c->srv = find_srv_by_addr(&addr);
	if (c->srv == NULL) {
		cli_del(c);
		return;
	}

	c->srv->recv++;
	if (c->srv->threshold > 0) {
		c->srv->threshold = 0;
		c->srv->skip = 0;
	}

	id = htons(c->id);
	memcpy(sock->buf.data, &id, sizeof(u_short));

	/* Send answer to client */
	send_udp(sock_udp_listen, &sock->buf, &c->addr);
	cli_del(c);
}

static void
handler_tcp_listen(struct kevent *ev, struct timeval *now)
{
	int newsock;
	sock_t *sock;
	addr_t addr;

	sock = &sockhash[ev->ident];

	addr.len = sizeof(struct sockaddr_in);
	bzero(&addr.sin, sizeof(struct sockaddr_in));

	newsock = accept(sock->fd, (struct sockaddr *)&addr.sin, &addr.len);
	if (newsock == -1) {
		logerr(ERR, "accept()");
		return;
	}

	if (newsock >= max_sock) {
		close(newsock);
		logout(ERR, "Max sockets reached: %d", max_sock);
		return;
	}

	logout(DEBUG, "New TCP client");

	EV_SET(&chlist[nchanges++], newsock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	sockhash[newsock].fd = newsock;
	sockhash[newsock].addr = addr;
	sockhash[newsock].handler = handler_tcp_in;
}

static void
handler_tcp_in(struct kevent *ev, struct timeval *now)
{
	ssize_t n;
	u_short len;
	u_short id;
	sock_t *sock;
	client_t *c;
	request_t *req;

	sock = &sockhash[ev->ident];

	if (ev->filter == EVFILT_READ) {
		n = recv_len(sock->fd, &len);
		if (n == -1 || n == 0) {
			close(sock->fd);
			return;
		} else if (n != sizeof(u_short))
			return;

		if (sock->buf.data == NULL)
			if (buf_init(&sock->buf, len + sizeof(u_short)) == NULL)
				return;

		n = recv_tcp(sock->fd, &sock->buf);
		if (n == -1 || n == 0) {
			close(sock->fd);
			return;
		} else if (sock->buf.off == sock->buf.len) {
			memcpy(&id, sock->buf.data + sizeof(u_short), sizeof(u_short));
			id = ntohs(id);
			logout(DEBUG, "Receive %d bytes from sock: %d (id: %05d)", n, sock->fd, id);

			c = cli_init(NULL, &sock->buf, now, id);
			if (c == NULL) {
				close(sock->fd);
				return;
			}

			c->buf.off = 0;
			c->sock.fd = sock->fd; /* XXX */

			id = htons(c->num % NS_MAXID);
			memcpy(c->buf.data + sizeof(u_short), &id, sizeof(u_short));

			EV_SET(&chlist[nchanges++], c->srv->fd, EVFILT_WRITE, EV_ADD /*EV_ENABLE*/, 0, 0, NULL);
			req_add(c, now);

			req = malloc(sizeof(request_t));
			if (req == NULL) {
				close(sock->fd);
				return;
			}
			req->cli = c;
			TAILQ_INSERT_TAIL(&in_requests, req, next);
		}
		return;
	}

	if (ev->filter == EVFILT_WRITE) {
		while (!TAILQ_EMPTY(&out_requests)) {
			req = TAILQ_FIRST(&out_requests);
			TAILQ_REMOVE(&out_requests, req, next);
			send_tcp(sock->fd, &req->cli->buf);
			cli_del(req->cli);
			free(req);
		}

		EV_SET(&chlist[nchanges++], sock->fd, EVFILT_WRITE, EV_DELETE /*EV_DISABLE*/, 0, 0, NULL);
		return;
	}
}

static void
handler_tcp_out(struct kevent *ev, struct timeval *now)
{
	ssize_t n;
	u_short len;
	u_short id;
	sock_t *sock;
	client_t *c;
	request_t *req;

	sock = &sockhash[ev->ident];

	if (ev->filter == EVFILT_READ) {
		n = recv_len(sock->fd, &len);
		if (n == -1 || n == 0) {
			close(sock->fd);
			sleep(1); /* XXX */
			sock_connect(sock->srv);
			return;
		} else if (n != sizeof(u_short))
			return;

		if (sock->buf.data == NULL)
			if (buf_init(&sock->buf, len + sizeof(u_short)) == NULL)
				return;

		n = recv_tcp(sock->fd, &sock->buf);
		if (n == -1 || n == 0) {
			close(sock->fd);
			sleep(1); /* XXX */
			sock_connect(sock->srv);
			return;
		} else if (sock->buf.off == sock->buf.len) {
			memcpy(&id, sock->buf.data + sizeof(u_short), sizeof(u_short));
			id = ntohs(id);
			logout(DEBUG, "Receive %d bytes from sock: %d (id: %05d)", n, sock->fd, id);

			c = find_cli(id);
			if (c == NULL) {
				close(sock->fd);
				return;
			}

			req_del(c);

			sock->buf.off = 0;
			c->buf = sock->buf;
			sock->buf.data = NULL; /* must be NULL here */

			id = htons(c->id);
			memcpy(c->buf.data + sizeof(u_short), &id, sizeof(u_short));

			EV_SET(&chlist[nchanges++], sock->fd, EVFILT_WRITE, EV_ADD /*EV_DISABLE*/, 0, 0, NULL);
			EV_SET(&chlist[nchanges++], c->sock.fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);

			req = malloc(sizeof(request_t));
			if (req == NULL) {
				close(c->sock.fd);
				return;
			}
			req->cli = c;
			TAILQ_INSERT_TAIL(&out_requests, req, next);
		}
		return;
	}

	if (ev->filter == EVFILT_WRITE) {
		if (sock->connected) {
			while (!TAILQ_EMPTY(&in_requests)) {
				req = TAILQ_FIRST(&in_requests);
				TAILQ_REMOVE(&in_requests, req, next);
				send_tcp(sock->fd, &req->cli->buf);
				free(req->cli->buf.data);
				free(req);
				/* We've already waited for input data on this socket */
			}
		} else {
			sock->connected = 1;
			logout(DEBUG, "Connection established to %s:%d", sock->srv->name, sock->srv->port);
		}
		EV_SET(&chlist[nchanges++], sock->fd, EVFILT_WRITE, EV_DELETE /*EV_DISABLE*/, 0, 0, NULL);
		return;
	}
}

static void
handler_timeout(struct timeval *now)
{
	client_t *c;

	/* Find timeouted requests */
	while ((c = find_to_cli(now)) != NULL) {
		req_del(c);
		c->srv->threshold++;

		if (cli_update(c, now) == -1) {
			timeouted_requests++;
			continue;
		}

		/* Send request to DNS server */
		send_udp(sock_udp_cli, &c->buf, &c->srv->addr);
		req_add(c, now);
	}
}

static client_t *
cli_init(addr_t *addr, buf_t *buf, struct timeval *tv, u_short id)
{
	client_t *c;

	/* XXX must be chained */
	if (clihash[requests_cli % NS_MAXID])
		return (NULL);

	/* Init client */
	c = malloc(sizeof(client_t));
	if (c == NULL) {
		logerr(ERR, "malloc(client)");
		return (NULL);
	}
	clihash[requests_cli % NS_MAXID] = c;

	c->id = id;
	if (addr != NULL)
		c->addr = *addr;
	c->buf = *buf;
	buf->data = NULL; /* must be NULL here */
	c->num = requests_cli;
	c->srv = STAILQ_FIRST(&servers);

	requests_cli++;

	if (cli_update(c, tv) == -1)
		return (NULL);

	active_clients++;
	if (active_clients > max_active_clients)
		max_active_clients = active_clients;

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

static int
req_add(client_t *c, struct timeval *tv)
{
	request_t *req;

	c->srv->send++;
	requests_srv++;

	req = malloc(sizeof(request_t));
	if (req == NULL) {
		logerr(ERR, "malloc(request)");
		cli_del(c);
		return (-1);
	}

	TAILQ_INSERT_TAIL(&requests, req, next);

	req->cli = c;
	c->req = req;

	return (0);
}

static void
req_del(client_t *c)
{

	TAILQ_REMOVE(&requests, c->req, next);
	free(c->req);
}

static void *
buf_init(buf_t *buf, size_t size)
{

	buf->off = 0;
	buf->len = size;
	/* size = ((size / NS_PACKETSZ) + (size % NS_PACKETSZ) ? 1 : 0) * NS_PACKETSZ; */
	buf->data = malloc(buf->len);
	if (buf->data == NULL)
		logerr(ERR, "malloc(buf)");

	return (buf->data);
}

static ssize_t
recv_udp(int sock, buf_t *buf, addr_t *addr, u_short *id)
{
	ssize_t n;
	u_short dns_id;
	struct sockaddr_in *sin;
	socklen_t *len;

	bzero(&addr->sin, sizeof(struct sockaddr_in));
	addr->len = sizeof(struct sockaddr_in);

	sin = &addr->sin;
	len = &addr->len;

	do {
		n = recvfrom(sock, buf->data, buf->len, 0, (struct sockaddr *)sin, len);
	} while (n == -1 && errno == EINTR);

	if (n == -1) {
		logerr(ERR, "recvfrom()");
		return (n);
	}

	if (n < NS_HFIXEDSZ)
		logout(WARN, "Read %d bytes from %s:%d (request too short)",
		    n, inet_ntoa(sin->sin_addr), htons(sin->sin_port));
	else {
		buf->off = n;
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
		n = sendto(sock, buf->data, buf->off, 0, (const struct sockaddr *)sin, *len);
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

static ssize_t
recv_len(int sock, u_short *len)
{
	ssize_t n;

	n = recv(sock, len, sizeof(u_short), MSG_PEEK);

	if (n == -1)
		logerr(ERR, "recv()");

	if (n == 0)
		logout(DEBUG, "Connection closed by peer");

	if (n == sizeof(u_short)) {
		*len = ntohs(*len);
		logout(DEBUG, "Receive len: %u", *len);
		return (n);
	}

	/* Can't read len, just return and try later */

	return (n);
}

static ssize_t
recv_tcp(int sock, buf_t *buf)
{
	ssize_t n;

	n = recv(sock, buf->data + buf->off, buf->len - buf->off, 0);
	if (n == -1)
		logerr(ERR, "recv()");

	if (n == 0)
		logout(DEBUG, "Connection closed by peer");

	if (n > 0)
		buf->off += n;

	return (n);
}

static ssize_t
send_tcp(int sock, buf_t *buf)
{
	ssize_t n;

	n = send(sock, buf->data + buf->off, buf->len - buf->off, 0);
	if (n == -1)
		logerr(ERR, "send()");

	logout(DEBUG, "Send %d bytes to sock: %d", n, sock);
	buf->off += n;

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

	STAILQ_FOREACH(s, &servers, next)
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
find_to_cli(struct timeval *tp)
{
	request_t *req;
	client_t *c;

	c = NULL;

	if (!TAILQ_EMPTY(&requests)) {
		req = TAILQ_FIRST(&requests);
		if (timercmp(tp, &req->cli->tv, >))
			c = req->cli;
	}

	if (c) {
		logout(DEBUG, "Client #%03d timeouted at %d.%06ld (id: %05d)",
		    c->num, c->tv.tv_sec, c->tv.tv_usec, c->id);
	}

	return (c);
}

static void
get_timeout(struct timeval *tp, struct timeval *to)
{
	request_t *req;
	struct timeval diff;

	req = TAILQ_FIRST(&requests);

	timersub(&req->cli->tv, tp, &diff);

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

	STAILQ_FOREACH(s, &servers, next)
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

	free(sockhash[sock_udp_cli].buf.data);

	free(sockhash);
	free(evlist);
	free(chlist);
	free(clihash);

	while (!STAILQ_EMPTY(&servers)) {
		s = STAILQ_FIRST(&servers);
		STAILQ_REMOVE_HEAD(&servers, next);
		free(s);
	}

	exit(0);
}
