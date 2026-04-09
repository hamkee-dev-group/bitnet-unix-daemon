#ifndef BITNETD_LOG_H
#define BITNETD_LOG_H

enum log_level {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR
};

void log_init(const char *ident, const char *logfile, enum log_level level,
              int use_syslog, int use_stderr);
void log_close(void);
void log_reopen(void);
void log_set_level(enum log_level level);

void log_msg(enum log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define log_debug(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_LVL_INFO,  __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_LVL_WARN,  __VA_ARGS__)
#define log_error(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)

#endif
