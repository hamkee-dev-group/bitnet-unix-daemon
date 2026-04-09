#ifndef BITNETD_H
#define BITNETD_H

#define BITNETD_VERSION    "1.0.0"
#define BITNETD_NAME       "bitnetd"
#define BITNETD_CONF_PATH  "/etc/bitnetd.conf"
#define BITNETD_PID_PATH   "/var/run/bitnetd.pid"
#define BITNETD_LOG_PATH   "/var/log/bitnetd.log"
#define BITNETD_SOCK_PATH  "/var/run/bitnetd.sock"

#define BITNETD_DEFAULT_PORT       8080
#define BITNETD_DEFAULT_THREADS    4
#define BITNETD_DEFAULT_CTX_SIZE   4096
#define BITNETD_DEFAULT_MAX_CONN   64
#define BITNETD_DEFAULT_TIMEOUT    30
#define BITNETD_DEFAULT_MAX_BODY   65536
#define BITNETD_DEFAULT_BACKLOG    128
#define BITNETD_DEFAULT_RATE_LIMIT 60

#define BITNETD_MAX_HEADERS    64
#define BITNETD_MAX_PATH       2048
#define BITNETD_READ_BUF       8192
#define BITNETD_WRITE_BUF      65536

#include <signal.h>

extern volatile sig_atomic_t g_shutdown;
extern volatile sig_atomic_t g_reload;
extern volatile sig_atomic_t g_reopen_logs;
extern volatile sig_atomic_t g_dump_status;

#endif
