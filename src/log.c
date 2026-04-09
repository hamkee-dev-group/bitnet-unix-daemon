#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <syslog.h>

static FILE           *s_logfp;
static char            s_logpath[512];
static enum log_level  s_level  = LOG_LVL_INFO;
static int             s_syslog;
static int             s_stderr;

static const char *level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };
static int syslog_prio[] = { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR };

void
log_init(const char *ident, const char *logfile, enum log_level level,
         int use_syslog, int use_stderr)
{
    s_level  = level;
    s_syslog = use_syslog;
    s_stderr = use_stderr;

    if (use_syslog)
        openlog(ident ? ident : "bitnetd", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    if (logfile && *logfile) {
        snprintf(s_logpath, sizeof(s_logpath), "%s", logfile);
        s_logfp = fopen(s_logpath, "a");
    }
}

void
log_close(void)
{
    if (s_syslog)
        closelog();
    if (s_logfp) {
        fclose(s_logfp);
        s_logfp = NULL;
    }
}

void
log_reopen(void)
{
    if (s_logfp) {
        fclose(s_logfp);
        s_logfp = fopen(s_logpath, "a");
    }
}

void
log_set_level(enum log_level level)
{
    s_level = level;
}

void
log_msg(enum log_level level, const char *fmt, ...)
{
    if (level < s_level)
        return;

    va_list ap;
    char msg[2048];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (s_syslog)
        syslog(syslog_prio[level], "%s", msg);

    char timebuf[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm);

    if (s_logfp) {
        fprintf(s_logfp, "%s [%s] %s\n", timebuf, level_str[level], msg);
        fflush(s_logfp);
    }

    if (s_stderr) {
        fprintf(stderr, "%s [%s] %s\n", timebuf, level_str[level], msg);
        fflush(stderr);
    }
}
