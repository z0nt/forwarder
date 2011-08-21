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
	int i;
	int line;
	int srv_id;
	int port;
	u_int weight;
	int options_parsed;
	int attempts_parsed;
	int autoweight_parsed;
	int timeout_parsed;
	FILE *config;
	char buf[LINE_MAX];
	char *str;
	char *endp;
	server_t *s;
	float to;

	config = fopen(path, "r");
	if (config == NULL)
		logerr(EXIT, "Cannot open config file: %s", path);

	line = 0;
	srv_id = 0;

	options_parsed = 0;
	attempts_parsed = 0;
	autoweight_parsed = 0;
	timeout_parsed = 0;

	/* Init global variables */
	attempts = ATTEMPTS;
	autoweight = AUTOWEIGHT;
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

			servers++;
			s = malloc(sizeof(server_t));
			if (s == NULL)
				logerr(EXIT, "malloc()");
			/* Default values */
			s->port = PORT;
			s->conf.weight = 1;

			/* check and skip blanks */
			check_and_skip_blanks(str);

			/* IP address */
			strtoipv4(str, &endp, s->name);
			if (endp == str || (*endp != ':' && !isspace(*endp)))
				config_err(buf, str, line);
			str = endp;

			/* port number */
			if (*str == ':') {
				str++;

				port = strtol(str, &endp, 10);
				if (endp == str)
					config_err(buf, str, line);
				if (port < 1 || port > 65535)
					logout(EXIT, "Port must be between 1..65535");
				str = endp;
				s->port = port;
			}

			/* check and skip blanks and comments */
			check_and_skip_blanks(str);
			skip_comments(str);

			/* weight */
			if (strncmp(str, "weight", sizeof("weight") - 1) == 0) {
				str += sizeof("weight") - 1;

				/* check and skip blanks */
				check_and_skip_blanks(str);

				weight = strtol(str, &endp, 10);
				if (endp == str)
					config_err(buf, str, line);
				if (weight <= 0)
					logout(EXIT, "Weight must be positive");
				str = endp;
				s->conf.weight = weight;
			}

			/* skip blanks and comments */
			skip_blanks(str);
			check_and_skip_comments(str);

			s->id = srv_id;
			STAILQ_INSERT_TAIL(&srvq, s, next);

			srv_id++;

		/* options */
		} else if (strncmp(str, "options", sizeof("options") - 1) == 0) {
			str += sizeof("options") - 1;
			if (options_parsed)
				logout(EXIT, "Only one options line must be used");
			options_parsed = 1;

			for (i = 0; i < 3; i++) {
				/* skip blanks */
				skip_blanks(str);

				/* attempts */
				if (isspace(*(str - 1)) &&
				    strncmp(str, "attempts:", sizeof("attempts:") - 1) == 0) {
					str += sizeof("attempts:") - 1;
					if (attempts_parsed)
						logout(EXIT, "Option attempts is already declared");
					attempts_parsed = 1;

					attempts = strtol(str, &endp, 10);
					if (endp == str)
						config_err(buf, str, line);
					if (attempts <= 0)
						logout(EXIT, "Number of attempts must be positive");
					str = endp;
				}

				/* autoweight */
				if (isspace(*(str - 1)) &&
				    strncmp(str, "autoweight:", sizeof("autoweight:") - 1) == 0) {
					str += sizeof("autoweight:") - 1;
					if (autoweight_parsed)
						logout(EXIT, "Option autoweight is already declared");
					autoweight_parsed = 1;

					if (strncmp(str, "on", sizeof("on") - 1) == 0) {
						str += sizeof("on") - 1;
						autoweight = 1;
					} else if (strncmp(str, "off", sizeof("off") - 1) == 0) {
						str += sizeof("off") - 1;
						autoweight = 0;
					} else
						logout(EXIT, "Wrong statement, use on/off");
				}
				
				/* timeout */
				if (isspace(*(str - 1)) &&
				    strncmp(str, "timeout:", sizeof("timeout:") - 1) == 0) {
					str += sizeof("timeout:") - 1;
					if (timeout_parsed)
						logout(EXIT, "Option timeout is already declared");
					timeout_parsed = 1;

					to = strtof(str, &endp);
					if (endp == str)
						config_err(buf, str, line);
					if (to <= 0)
						logout(EXIT, "Timeout must be positive");
					str = endp;
					float2timer(to, &timeout);
				}
			}

			if (attempts_parsed == 0 && autoweight_parsed == 0 && timeout_parsed == 0)
				logout(EXIT, "You must declare at least one option");

			/* skip blanks and comments */
			skip_blanks(str);
			check_and_skip_comments(str);
		} else
			config_err(buf, str, line);
	}

	if (servers == 0)
		logout(EXIT, "You must specify at least one server");

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

	logout(EXIT,
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
