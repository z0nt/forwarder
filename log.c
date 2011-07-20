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

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "log.h"

void
loginit(const char *logfile)
{
        if (debug_level == MIN_DEBUG_LEVEL)
                return;

        if (syslog_flag)
                openlog(getprogname(), LOG_CONS | LOG_PID, LOG_DAEMON);
        else {
                logfd = open(logfile, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                if (logfd == -1)
                        err(1, "Cannot open logfile: %s", logfile);
        }
}

void
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

	n = snprintf(bufp, PIPE_BUF, "%02d/%02d/%04d %02d:%02d:%02d.%06ld [%s] ",
	    lt->tm_mday, lt->tm_mon, lt->tm_year, lt->tm_hour, lt->tm_min, lt->tm_sec, usec, nlevel);
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

void
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

	n = snprintf(bufp, PIPE_BUF, "%02d/%02d/%04d %02d:%02d:%02d.%06ld [%s] ",
	    lt->tm_mday, lt->tm_mon, lt->tm_year, lt->tm_hour, lt->tm_min, lt->tm_sec, usec, nlevel);
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
