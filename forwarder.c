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
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <unistd.h>

/*#define HOST	INADDR_ANY*/
#define HOST	"127.0.0.2"
#define PORT	53
#define IP_LEN	15
#define BUF_SIZ	1024
#define TIMEOUT	500000 /* microseconds */

#define nonblock_socket(s) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)

struct dns_server_s {
	char name[IP_LEN];
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
	char buf[BUF_SIZ];
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
static int active_clients;
static int clients;
static dns_client_t *cli;
static int debug_level = 3;
static int syslog_flag = 0;
static int logfd;
static unsigned long requests;
static unsigned long requests_dns;
static unsigned int weight_factor;
static int *srv_ptr;

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
static void loginit(char *logfile);
static void logout(const int level, const char *fmt, ...);
static void logerr(const int level, const char *fmt, ...);

int
main(void)
{
	sock_init();
	dns_init();
	cli_init();
	loginit("forwarder.log");
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
	int j;
	int n;
	int w;

	weight_factor = 0;

	servers = 3;
	srv = malloc(sizeof(dns_server_t) * servers);
	if (srv == NULL)
		err(1, "malloc()");
	

	strcpy(srv[0].name, "95.108.142.1");
	strcpy(srv[1].name, "95.108.142.1");
	strcpy(srv[2].name, "213.180.205.1");
	srv[0].conf_weight = 2;
	srv[1].conf_weight = 3;
	srv[2].conf_weight = 1;

	for (i = 0; i < servers; i++) {
		weight_factor += srv[i].conf_weight;
		bzero(&srv[i].addr, sizeof(struct sockaddr_in));
		srv[i].addr.sin_family = PF_INET;
		srv[i].addr.sin_addr.s_addr = inet_addr(srv[i].name);
		srv[i].addr.sin_port = htons(srv[i].port ? srv[i].port : PORT);
		srv[i].len = sizeof(struct sockaddr_in);
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

	clients = 1024;
	cli = malloc(sizeof(dns_client_t) * clients);
	if (cli == NULL)
		err(1, "malloc()");

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
	int active;
	ssize_t n;
	void *ptr;
	char buf[BUF_SIZ];
	struct kevent *ch;
	struct kevent *ev;
	struct timespec *timeout;
	struct timespec tv;
	struct timeval tp;
	dns_server_t srv1;

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
	requests = 0;
	requests_dns = 0;

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
				++requests;
				cli[j].tv = tp;
				ptr = cli[j].buf;

				++requests_dns;
				j = find_dns(cli[j].ret++);
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

				++requests_dns;
				j = find_dns(cli[j].ret++);
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
find_dns(int ret)
{
	int i;

#if 0
	for (i = 0; i < servers; i++) {
		logout(3, "server #%02d finded (r: %d, factor: %d, weight: %d)", i, requests_dns, weight_factor, srv[i].conf_weight);
		if (requests_dns * srv[i].conf_weight / weight_factor == 0) {
			return(i);
		}
	}

	logout(1, "Cannot find DNS server, exiting...");
	exit(1);
#endif
	i = srv_ptr[(requests_dns - 1) % weight_factor];
	logout(3, "server #%02d finded (r: %d, factor: %d, weight: %d)", i, requests_dns, weight_factor, srv[i].conf_weight);

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
loginit(char *logfile)
{
        if (debug_level == 0)
                return;

        if (syslog_flag)
                openlog(getprogname(), LOG_CONS | LOG_PID, LOG_DAEMON);
        else {
                logfd = open(logfile, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                if (logfd == -1)
                        err(1, "Cannot open logfile: %s", logfile);
        }
}

static void
logout(const int level, const char *fmt, ...)
{
	int n, m;
	long int usec;
	char *nlevel;
	char buf[PIPE_BUF], *bufp;
	va_list ap;
	time_t tv_time_t;
	struct timeval tv;
	struct tm *lt;

	n = m = 0;
	bufp = buf;

	if (level > debug_level)
		return;

	if (syslog_flag) {
		va_start(ap, fmt);
		n = vsnprintf(bufp, PIPE_BUF, fmt, ap);
		va_end(ap);

		switch(level) {
		case 1:
			syslog(LOG_ERR, buf);
			break;
		case 2:
			syslog(LOG_WARNING, buf);
			break;
		case 3:
			syslog(LOG_DEBUG, buf);
			break;
		default:
			break;
		}

		return;
	}

	gettimeofday(&tv, (struct timezone *)NULL);
	tv_time_t = tv.tv_sec;
	lt = localtime(&tv_time_t);
	lt->tm_mon++;
	lt->tm_year += 1900;
	usec = tv.tv_usec;

	switch(level) {
		case 1:
			nlevel = "error";
			break;
		case 2:
			nlevel = "warn";
			break;
		case 3:
			nlevel = "debug";
			break;
		default:
			break;
	}

	n = snprintf(bufp, PIPE_BUF, "%02d/%02d/%04d %02d:%02d:%02d.%06ld [%s] ", lt->tm_mday, lt->tm_mon, lt->tm_year, lt->tm_hour, lt->tm_min, lt->tm_sec, usec, nlevel);
	bufp += n;
	m +=n;

	va_start(ap, fmt);
	if (fmt != NULL) {
		n = vsnprintf(bufp, (PIPE_BUF - m), fmt, ap);
		bufp += n;
		m += n;
	}
	va_end(ap);

	if (m >= PIPE_BUF) {
		bufp += - m + PIPE_BUF - 2;
		m = PIPE_BUF - 2;
	}

	n = snprintf(bufp, (PIPE_BUF - m), "\n");
	m += n;

	write(logfd, buf, m);
}

static void
logerr(const int level, const char *fmt, ...)
{
	int n, m;
	long int usec;
	char *nlevel;
	char buf[PIPE_BUF], *bufp, errbuf[PIPE_BUF], *errbufp;
	va_list ap;
	time_t tv_time_t;
	struct timeval tv;
	struct tm *lt;

	n = m = 0;
	bufp = buf;
	errbufp = errbuf;

	if (level > debug_level)
		return;

	if (syslog_flag) {
		va_start(ap, fmt);
		n = vsnprintf(bufp, PIPE_BUF, fmt, ap);
		snprintf(bufp + n, (PIPE_BUF - n), " (%d: %%m)", errno);
		va_end(ap);

		switch(level) {
		case 1:
			syslog(LOG_ERR, buf);
			break;
		case 2:
			syslog(LOG_WARNING, buf);
			break;
		case 3:
			syslog(LOG_DEBUG, buf);
			break;
		default:
			break;
		}

		return;
	}

	gettimeofday(&tv, (struct timezone *)NULL);
	tv_time_t = tv.tv_sec;
	lt = localtime(&tv_time_t);
	lt->tm_mon++;
	lt->tm_year += 1900;
	usec = tv.tv_usec;

	switch(level) {
		case 1:
			nlevel = "error";
			break;
		case 2:
			nlevel = "warn";
			break;
		case 3:
			nlevel = "debug";
			break;
		default:
			break;
	}

	n = snprintf(bufp, PIPE_BUF, "%02d/%02d/%04d %02d:%02d:%02d.%06ld [%s] ", lt->tm_mday, lt->tm_mon, lt->tm_year, lt->tm_hour, lt->tm_min, lt->tm_sec, usec, nlevel);
	bufp += n;
	m +=n;

	va_start(ap, fmt);
	if (fmt != NULL) {
		n = vsnprintf(bufp, (PIPE_BUF - m), fmt, ap);
		bufp += n;
		m += n;
	}
	va_end(ap);

	strerror_r(errno, errbufp, PIPE_BUF - m);
	n = snprintf(bufp, (PIPE_BUF - m), " (%d: %s)", errno, errbuf);
	bufp += n;
	m += n;

	if (m >= PIPE_BUF) {
		bufp += - m + PIPE_BUF - 2;
		m = PIPE_BUF - 2;
	}

	n = snprintf(bufp, (PIPE_BUF - m), "\n");
	m += n;

	write(logfd, buf, m);
}
