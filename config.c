/*
 * Copyright (c) 2010 Andrey Zonov <andrey@zonov.org>
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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <err.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "forwarder.h"

#define MAX_LINE_LEN LINE_MAX

static void config_err(const char *buf, int line, size_t len);

void
config_init(const char *path)
{
	int i;
	int id;
	int port;
	int weight;
	int line;
	size_t len;
	size_t len1;
	FILE *config;
	char buf[MAX_LINE_LEN];
	char ip[IP_LEN];
	char strport[PORT_LEN];
	char strweight[WEIGHT_LEN];
	char *p;
	server_t *s;

	config = fopen(path, "r");
	if (config == NULL)
		err(1, "Cannot open config file: %s", path);

	id = 0;
	line = 0;
	servers = 0;
	STAILQ_INIT(&srvq);
	while ((p = fgets(buf, MAX_LINE_LEN, config)) != NULL) {
		line++;
		len1 = strlen(buf);
		len = len1;

		if (len == 0)
			continue;

		while (len > 0) {

			if (*p == '\n' || *p == '\r' || *p == '\0' || *p == '#')
				break;

			while (*p == ' ' || *p == '\t') {
				len--;
				p++;
				continue;
			}

			/* nameserver */
			if (!strncmp(p, "nameserver", sizeof("nameserver") - 1) &&
			    (p[sizeof("nameserver") - 1] == ' ' ||
			     p[sizeof("nameserver") - 1] == '\t')) {
				len -= sizeof("nameserver") - 1;
				p += sizeof("nameserver") - 1;
				servers++;
				s = malloc(sizeof(server_t));
				if (s == NULL)
					err(1, "malloc()");

				if (len <= 0)
					config_err(buf, line, len1 - len);

				while (*p == ' ' || *p == '\t') {
					len--;
					p++;
				}

				/* IP address */
				ip[0] = '\0';
				for (i = 0; i < IP_LEN && len > 0 && (isdigit(*p) || *p == '.' || *p == ':'); i++) {
					if (*p == ':') {
						len--;
						p++;
						break;
					} else {
						ip[i] = *p;
						ip[i + 1] = '\0';
						len--;
						p++;
					}
				}

				if (ip[0] == '\0')
					config_err(buf, line, len1 - len);

				if (inet_addr(ip) == -1)
					config_err(buf, line, len1 - len);
	
				/* port number */
				strport[0] = '\0';
				for (i = 0; i < PORT_LEN && len > 0 && isdigit(*p); i++) {
					strport[i] = *p;
					strport[i + 1] = '\0';
					len--;
					p++;
				}

				/* skip spaces and commentaries */
				if (!(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\0'))
					config_err(buf, line, len1 - len);

				while (*p == ' ' || *p == '\t') {
					len--;
					p++;
				}

				/* weight */
				if (!strncmp(p, "weight", sizeof("weight") - 1) &&
				    (p[sizeof("weight") - 1] == ' ' ||
				     p[sizeof("weight") - 1] == '\t')) {
					len -= sizeof("weight") - 1;
					p += sizeof("weight") - 1;

					if (len <= 0)
						config_err(buf, line, len1 - len);

					while (*p == ' ' || *p == '\t') {
						len--;
						p++;
					}

					strweight[0] = '\0';
					for (i = 0; i < WEIGHT_LEN && len > 0 && isdigit(*p); i++) {
						strweight[i] = *p;
						strweight[i + 1] = '\0';
						len--;
						p++;
					}
				}

				/* skip spaces and commentaries */
				if (!(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\0'))
					config_err(buf, line, len1 - len);

				while (*p == ' ' || *p == '\t') {
					len--;
					p++;
				}

				if (!(*p == '\n' || *p == '\r' || *p == '\0' || *p == '#'))
					config_err(buf, line, len1 - len);

				/* save parsed data */
				port = atoi(strport);
				if (port < 0 || port > 65535)
					config_err(buf, line, len1 - len);
				else if (port == 0)
					port = PORT;

				s->port = port;

				strncpy(s->name, ip, IP_LEN);
				s->name[IP_LEN] = '\0';

				weight = atoi(strweight);
				if (weight < 0 || weight > 65535)
					config_err(buf, line, len1 - len);
				else if (weight == 0) {
					/* XXX */
					weight = 1;
				}

				s->conf_weight = weight;
				s->id = id;
				id++;
				STAILQ_INSERT_TAIL(&srvq, s, next);
			} else
				config_err(buf, line, len1 - len);
		}
	}
}

static void
config_err(const char *buf, int line, size_t len)
{
	fprintf(stderr,
	    "Parsing error: %d:%ld\n"
	    "%s"
	    "%*s^",
	    line, len, buf, (int)(len - 1), " ");

	exit(1);
}
