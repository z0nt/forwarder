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

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

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

static char logbuf[PIPE_BUF];

static void log_to_syslog(const int saved_errno, const int level, const char *fmt, va_list ap);
static void log_to_file(const int saved_errno, const int level, const char *fmt, va_list ap);

void
loginit(const char *logfile)
{
	int fd;

	if (debug_level == MIN_DEBUG_LEVEL)
		return;

	if (syslog_flag)
		openlog(getprogname(), LOG_CONS | LOG_PID, LOG_DAEMON);
	else {
		fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
		if (fd == -1)
			logerr(CRIT, "Cannot open logfile: %s", logfile);

		logfd = fd;
		print_time_and_level = 1;
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

	if (level == CRIT)
		exit(1);
}

void
logerr(const int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (syslog_flag)
		log_to_syslog(errno, level, fmt, ap);
	else
		log_to_file(errno, level, fmt, ap);
	va_end(ap);

	if (level == CRIT)
		exit(1);
}

static void
log_to_syslog(const int saved_errno, const int level, const char *fmt, va_list ap)
{
	int n, avail;
	char *bufp;

	avail = PIPE_BUF;
	bufp = logbuf;

	if (level > debug_level)
		return;

	if (fmt != NULL) {
		n = vsnprintf(bufp, avail, fmt, ap);
		bufp += n;
		avail -= n;
	}

	if (saved_errno)
		snprintf(bufp, avail, " (%d: %%m)", saved_errno);

	switch(level) {
		case CRIT:
			syslog(LOG_CRIT, logbuf);
			break;
		case ERR:
			syslog(LOG_ERR, logbuf);
			break;
		case WARN:
			syslog(LOG_WARNING, logbuf);
			break;
		case DEBUG:
			syslog(LOG_DEBUG, logbuf);
			break;
	}
}

static void
log_to_file(const int saved_errno, const int level, const char *fmt, va_list ap)
{
	int n, avail;
	char *nlevel;
	char *bufp;
	struct timeval tv;
	time_t tv_time;
	struct tm *lt;

	avail = PIPE_BUF;
	bufp = logbuf;

	if (level > debug_level)
		return;

	if (print_time_and_level) {
		gettimeofday(&tv, (struct timezone *)NULL);
		tv_time = tv.tv_sec;
		lt = localtime(&tv_time);
		lt->tm_mon++;
		lt->tm_year += 1900;

		switch(level) {
			case CRIT:
				nlevel = "crit";
				break;
			case ERR:
				nlevel = "error";
				break;
			case WARN:
				nlevel = "warn";
				break;
			case DEBUG:
				nlevel = "debug";
				break;
			default:
				nlevel = "unknown";
		}

		n = snprintf(bufp, avail, "%02d/%02d/%04d %02d:%02d:%02d.%06ld [%s] ",
		    lt->tm_mday, lt->tm_mon, lt->tm_year, lt->tm_hour, lt->tm_min, lt->tm_sec, tv.tv_usec, nlevel);
		bufp += n;
		avail -= n;
	}

	if (fmt != NULL) {
		n = vsnprintf(bufp, avail > 0 ? avail : 0, fmt, ap);
		bufp += n;
		avail -= n;
	}

	if (saved_errno) {
		n = snprintf(bufp, avail > 0 ? avail : 0, " (%d: %s)", saved_errno, strerror(saved_errno));
		bufp += n;
		avail -= n;
	}

	/* Truncate overflowed string */
	if (avail < 2) {
		bufp = logbuf + PIPE_BUF - 2;
		avail = 2;
	}

	n = snprintf(bufp, avail, "\n");
	avail -= n;

	write(logfd, logbuf, PIPE_BUF - avail);
}
