/*
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

static void log_to_syslog(const int print_errno, const int level, const char *fmt, va_list ap);
static void log_to_file(const int print_errno, const int level, const char *fmt, va_list ap);

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
	va_list ap;

	va_start(ap, fmt);
	if (syslog_flag)
		log_to_syslog(0, level, fmt, ap);
	else
		log_to_file(0, level, fmt, ap);
	va_end(ap);
}

void
logerr(const int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (syslog_flag)
		log_to_syslog(1, level, fmt, ap);
	else
		log_to_file(1, level, fmt, ap);
	va_end(ap);

}

static void
log_to_syslog(const int print_errno, const int level, const char *fmt, va_list ap)
{
	int n, avail;
	char buf[PIPE_BUF], *bufp;

	avail = PIPE_BUF;
	bufp = buf;

	if (level > debug_level)
		return;

	n = vsnprintf(bufp, avail, fmt, ap);
	bufp += n;
	avail -= n;

	if (print_errno)
		snprintf(bufp, avail, " (%d: %%m)", errno);

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
}

static void
log_to_file(const int print_errno, const int level, const char *fmt, va_list ap)
{
	int n, avail;
	char *nlevel;
	char buf[PIPE_BUF], *bufp, errbuf[PIPE_BUF];
	long int usec;
	struct timeval tv;
	time_t tv_time;
	struct tm *lt;

	avail = PIPE_BUF;
	bufp = buf;

	if (level > debug_level)
		return;

	gettimeofday(&tv, (struct timezone *)NULL);
	tv_time = tv.tv_sec;
	lt = localtime(&tv_time);
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

	n = snprintf(bufp, avail, "%02d/%02d/%04d %02d:%02d:%02d.%06ld [%s] ",
	    lt->tm_mday, lt->tm_mon, lt->tm_year, lt->tm_hour, lt->tm_min, lt->tm_sec, usec, nlevel);
	bufp += n;
	avail -= n;

	n = vsnprintf(bufp, avail, fmt, ap);
	bufp += n;
	avail -= n;

	if (print_errno) {
		strerror_r(errno, errbuf, sizeof(errbuf));
		n = snprintf(bufp, avail, " (%d: %s)", errno, errbuf);
		bufp += n;
		avail -= n;
	}

	/* Truncate overflowed string */
	if (avail <= 0) {
		bufp = buf + PIPE_BUF - 2;
		avail = 2;
	}

	n = snprintf(bufp, avail, "\n");
	avail -= n;

	write(logfd, buf, sizeof(buf) - avail);
}
