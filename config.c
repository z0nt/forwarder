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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "forwarder.h"
#include "log.h"

#define skip_blanks(a)		while (isblank(*a)) { a++; }
#define skip_comments(a)	if (isspace(*a) || *a == '\0' || *a == '#') { continue; }
#define check_and_skip_blanks(a)	if (!isblank(*a)) {			\
						config_err(buf, a, line);	\
					} else {				\
						 skip_blanks(a);		\
					}
#define check_and_skip_comments(a)	if (*a == '#' && !isspace(*(a - 1))) {			\
						config_err(buf, a, line);			\
					} else if (!isspace(*a) && *a != '\0' && *a != '#') {	\
						config_err(buf, a, line);			\
					}

static void config_err(const char *buf, const char *str, int line);
static void strtoipv4(const char *str, char **endp, char *ip);
static void float2timer(float f, struct timeval *tv);

void
config_init(const char *path)
{
	int line;
	int optint;
	int options_parsed;
	int timeout_parsed;
	int servers;
	FILE *config;
	char buf[LINE_MAX];
	char *str;
	char *endp;
	server_t *s;
	float optfloat;

	config = fopen(path, "r");
	if (config == NULL)
		logerr(CRIT, "Cannot open config file: %s", path);

	line = 0;
	options_parsed = 0;
	timeout_parsed = 0;

	/* Init global variables */
	float2timer(TIMEOUT, &timeout);
	servers = 0;
	STAILQ_INIT(&srvq);

	while ((str = fgets(buf, sizeof(buf), config)) != NULL) {
		line++;

		/* skip blanks and comments */
		skip_blanks(str);
		skip_comments(str);

		/* nameserver */
		if (strncmp(str, "nameserver", sizeof("nameserver") - 1) == 0) {
			str += sizeof("nameserver") - 1;

			/* check and skip blanks */
			check_and_skip_blanks(str);

			s = calloc(1, sizeof(server_t));
			if (s == NULL)
				logerr(CRIT, "calloc()");
			STAILQ_INSERT_TAIL(&srvq, s, next);

			/* Default values */
			s->port = PORT;

			/* IP address */
			strtoipv4(str, &endp, s->name);
			if (endp == str || (*endp != ':' && !isspace(*endp)))
				config_err(buf, str, line);
			str = endp;

			/* port number */
			if (*str == ':') {
				str++;

				optint = strtol(str, &endp, 10);
				if (endp == str)
					config_err(buf, str, line);
				if (optint < 1 || optint > IPPORT_MAX)
					logout(CRIT, "Port must be between 1..%d", IPPORT_MAX);
				str = endp;
				s->port = optint;
			}

			/* check and skip blanks and comments */
			check_and_skip_blanks(str);
			skip_comments(str);

			/* threshold */
			if (strncmp(str, "threshold", sizeof("threshold") - 1) == 0) {
				str += sizeof("threshold") - 1;

				/* check and skip blanks */
				check_and_skip_blanks(str);

				optint = strtol(str, &endp, 10);
				if (endp == str)
					config_err(buf, str, line);
				if (optint <= 0)
					logout(CRIT, "Threshold must be positive");
				str = endp;
				s->conf.threshold = optint;

				if (*str == ':') {
					str++;

					optint = strtol(str, &endp, 10);
					if (endp == str)
						config_err(buf, str, line);
					if (optint <= 0)
						logout(CRIT, "Second parameter of threshold must be positive");
					str = endp;
					s->conf.skip = optint;
				} else
					config_err(buf, str, line);
			} else
				logout(CRIT, "Threshold must be specified");

			/* skip blanks and comments */
			skip_blanks(str);
			check_and_skip_comments(str);

			s->id = servers;
			servers++;

		/* options */
		} else if (strncmp(str, "options", sizeof("options") - 1) == 0) {
			str += sizeof("options") - 1;
			if (options_parsed)
				logout(CRIT, "Only one options line must be used");
			options_parsed = 1;

			/* check and skip blanks */
			check_and_skip_blanks(str);

			/* timeout */
			if (strncmp(str, "timeout:", sizeof("timeout:") - 1) == 0) {
				str += sizeof("timeout:") - 1;

				optfloat = strtof(str, &endp);
				if (endp == str)
					config_err(buf, str, line);
				if (optfloat <= 0)
					logout(CRIT, "Timeout must be positive");
				str = endp;
				float2timer(optfloat, &timeout);
			}

			/* skip blanks and comments */
			skip_blanks(str);
			check_and_skip_comments(str);
		} else
			config_err(buf, str, line);
	}

	if (servers == 0)
		logout(CRIT, "You must specify at least one server");

	fclose(config);
}

static void
config_err(const char *buf, const char *str, int line)
{
	int pos;
	int len;
	char *s;

	pos = 0;
	len = strlen(str);
	s = (char *) buf;

	while (*s != '\0') {
		if (strncmp(s, str, len) == 0)
			break;
		if (*s == '\t')
			pos = pos - (pos % 8) + 8;
		else
			pos++;
		s++;
	}

	logout(CRIT,
	    "Error parsing config at line:%d, position: %d\n"
	    "%s"
	    "%*s^",
	    line, pos, buf, pos, pos > 0 ? " " : "");
}

static void
strtoipv4(const char *str, char **endp, char *ip)
{
	int i;
	const char *s;
	struct in_addr addr;

	s = str;
	for (i = 0; i < INET_ADDRSTRLEN; i++) {
		if (isdigit(*s) || *s == '.') {
			ip[i] = *s;
			s++;
		} else {
			ip[i] = '\0';
			break;
		}
	}

	*endp = (char *) str;
	if (inet_aton(ip, &addr))
		*endp = (char *) s;
}

static void
float2timer(float f, struct timeval *tv)
{
	tv->tv_sec = f;
	f -= tv->tv_sec;
	tv->tv_usec = f * 1000000;
}
