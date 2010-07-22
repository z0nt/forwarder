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
#define	HOST	"127.0.0.1"
#define DNSHOST	"95.108.142.1"
#define	PORT	53
#define	BUF_SIZ	1024

#define nonblock_socket(s) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)

struct dns_server {
	int sock;
	struct sockaddr_in addr;
	socklen_t len;
};

int sock;
struct dns_server *dns;

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

	/* Server */
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

}

void
dnsinit(void)
{
	dns = (struct dns_server *)malloc(sizeof(struct dns_server));
	if (dns == NULL)
		err(1, "malloc()");

	/* Socket for DNS server */
	dns->sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (dns->sock == -1)
		err(1, "socket()");

	nonblock_socket(dns->sock);

	bzero(&dns->addr, sizeof(struct sockaddr_in));
	dns->addr.sin_family = PF_INET;
	dns->addr.sin_addr.s_addr = inet_addr(DNSHOST);
	dns->addr.sin_port = htons(PORT);
	dns->len = sizeof(struct sockaddr_in);
}

void
mainloop(void)
{
	int i;
	int kq;
	int nc;
	ssize_t n;
	char buf[BUF_SIZ];
	struct kevent *ch;
	struct kevent *ev;
	struct sockaddr_in cliaddr;
	socklen_t clilen;

	clilen = sizeof(struct sockaddr_in);

	ch = (struct kevent *)calloc(1, sizeof(struct kevent) * 2);
	if (ch == NULL)
		err(1, "malloc()");

	ev = (struct kevent *)calloc(1, sizeof(struct kevent) * 2);
	if (ev == NULL)
		err(1, "malloc()");

	kq = kqueue();
	if (kq == -1)
		err(1, "kqueue()");

	nc = 0;
	EV_SET(&ch[nc++], sock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&ch[nc++], dns->sock, EVFILT_READ, EV_ADD, 0, 0, NULL);

	/* Main loop */
	for ( ;; ) {

		n = kevent(kq, ch, nc, ev, 2, NULL);
		if (n == -1)
			err(1, "kevent()");

		nc = 0;

		for (i = 0; i < n; i++) {
			if (ev[i].ident == sock) {
				/* Read request from client */
				n = recv_udp(sock, buf, BUF_SIZ, 0, (struct sockaddr *)&cliaddr, &clilen);

				/* Send request to DNS server */
				n = send_udp(dns->sock, buf, n, 0, (const struct sockaddr *)&dns->addr, dns->len);
			}

			if (ev[i].ident == dns->sock) {
				/* Read answer from DNS server */
				n = recv_udp(dns->sock, buf, BUF_SIZ, 0, (struct sockaddr *)&dns->addr, &dns->len);

				/* Send answer to client */
				n = send_udp(sock, buf, n, 0, (const struct sockaddr *)&cliaddr, clilen);
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
