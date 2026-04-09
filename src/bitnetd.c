#include "../include/bitnetd.h"
#include "config.h"
#include "log.h"
#include "http.h"
#include "api.h"
#include "backend.h"
#include "metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <pwd.h>
#include <grp.h>

volatile sig_atomic_t g_shutdown    = 0;
volatile sig_atomic_t g_reload      = 0;
volatile sig_atomic_t g_reopen_logs = 0;
volatile sig_atomic_t g_dump_status = 0;

static void
sig_handler(int sig)
{
    switch (sig) {
    case SIGTERM:
    case SIGINT:
        g_shutdown = 1;
        break;
    case SIGHUP:
        g_reload = 1;
        break;
    case SIGUSR1:
        g_reopen_logs = 1;
        break;
    case SIGUSR2:
        g_dump_status = 1;
        break;
    default:
        break;
    }
}

static void
install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);
}

static int
write_pidfile(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("pidfile open %s: %s", path, strerror(errno));
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        log_error("pidfile locked — daemon already running?");
        close(fd);
        return -1;
    }

    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;

    return fd;
}

static int
daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (setsid() < 0) return -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }

    umask(0022);
    return 0;
}

static int
drop_privileges(const char *user, const char *group)
{
    if (getuid() != 0)
        return 0;

    if (group) {
        struct group *gr = getgrnam(group);
        if (gr) {
            if (setgid(gr->gr_gid) < 0) {
                log_error("setgid(%s): %s", group, strerror(errno));
                return -1;
            }
        }
    }

    if (user) {
        struct passwd *pw = getpwnam(user);
        if (pw) {
            if (setuid(pw->pw_uid) < 0) {
                log_error("setuid(%s): %s", user, strerror(errno));
                return -1;
            }
        }
    }

    return 0;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -c FILE   Configuration file (default: " BITNETD_CONF_PATH ")\n"
        "  -f        Run in foreground (don't daemonize)\n"
        "  -t        Test configuration and exit\n"
        "  -v        Print version and exit\n"
        "  -h        Show this help\n",
        prog);
}

int
main(int argc, char **argv)
{
    const char *conf_path = BITNETD_CONF_PATH;
    int foreground = 0;
    int test_only  = 0;
    int opt;

    while ((opt = getopt(argc, argv, "c:ftvh")) != -1) {
        switch (opt) {
        case 'c': conf_path  = optarg; break;
        case 'f': foreground = 1;      break;
        case 't': test_only  = 1;      break;
        case 'v':
            printf("%s %s\n", BITNETD_NAME, BITNETD_VERSION);
            return 0;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    config_t *cfg = cfg_create();
    if (!cfg) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (cfg_load(cfg, conf_path) < 0) {
        fprintf(stderr, "cannot load config: %s\n", conf_path);
        cfg_free(cfg);
        return 1;
    }

    if (test_only) {
        printf("configuration OK: %s\n", conf_path);
        cfg_free(cfg);
        return 0;
    }

    const char *logfile = cfg_get_str(cfg, "daemon", "logfile");
    const char *loglevel_str = cfg_get_str(cfg, "daemon", "loglevel");
    enum log_level loglevel = LOG_LVL_INFO;
    if (loglevel_str) {
        if (strcmp(loglevel_str, "debug") == 0) loglevel = LOG_LVL_DEBUG;
        else if (strcmp(loglevel_str, "warn") == 0) loglevel = LOG_LVL_WARN;
        else if (strcmp(loglevel_str, "error") == 0) loglevel = LOG_LVL_ERROR;
    }
    log_init(BITNETD_NAME, foreground ? NULL : logfile, loglevel,
             !foreground, foreground);

    log_info("%s %s starting", BITNETD_NAME, BITNETD_VERSION);

    install_signals();

    if (!foreground) {
        if (daemonize() < 0) {
            log_error("daemonize failed");
            cfg_free(cfg);
            return 1;
        }
    }

    const char *pidpath = cfg_get_str(cfg, "daemon", "pidfile");
    if (!pidpath) pidpath = BITNETD_PID_PATH;
    int pidfd = write_pidfile(pidpath);
    if (pidfd < 0) {
        log_error("could not write pidfile");
        cfg_free(cfg);
        return 1;
    }

    metrics_t metrics;
    metrics_init(&metrics);

    log_info("initializing inference backend...");
    backend_t *backend = backend_init(cfg);
    if (!backend) {
        log_error("backend initialization failed");
        close(pidfd);
        unlink(pidpath);
        cfg_free(cfg);
        return 1;
    }
    metrics_set_model_loaded(&metrics, 1);

    http_server_t *srv = http_create(cfg);
    if (!srv) {
        log_error("http server creation failed");
        backend_free(backend);
        close(pidfd);
        unlink(pidpath);
        cfg_free(cfg);
        return 1;
    }

    const char *listen_addr = cfg_get_str(cfg, "server", "listen");
    char addr[256] = "127.0.0.1";
    int port = BITNETD_DEFAULT_PORT;
    if (listen_addr) {
        const char *colon = strrchr(listen_addr, ':');
        if (colon) {
            size_t alen = (size_t)(colon - listen_addr);
            if (alen < sizeof(addr)) {
                memcpy(addr, listen_addr, alen);
                addr[alen] = '\0';
            }
            port = atoi(colon + 1);
        }
    }
    if (http_listen_tcp(srv, addr, port) < 0) {
        log_error("failed to bind TCP listener");
        http_destroy(srv);
        backend_free(backend);
        close(pidfd);
        unlink(pidpath);
        cfg_free(cfg);
        return 1;
    }

    const char *unix_path = cfg_get_str(cfg, "server", "unix_socket");
    if (unix_path && *unix_path) {
        http_listen_unix(srv, unix_path);
    }

    const char *run_user  = cfg_get_str(cfg, "daemon", "user");
    const char *run_group = cfg_get_str(cfg, "daemon", "group");
    if (run_user || run_group) {
        drop_privileges(run_user, run_group);
    }

#ifdef __OpenBSD__
    {
        const char *model_path = cfg_get_str(cfg, "model", "path");
        if (model_path)
            unveil(model_path, "r");
        unveil(NULL, NULL);
        pledge("stdio inet unix rpath", NULL);
    }
#endif

    api_ctx_t api_ctx = { .backend = backend, .metrics = &metrics };
    api_register_routes(srv, &api_ctx);

    log_info("%s ready — serving on %s:%d", BITNETD_NAME, addr, port);

    while (!g_shutdown) {
        http_run_once(srv, 1000);

        metrics_set_connections(&metrics, srv->nconns);

        if (g_reload) {
            g_reload = 0;
            log_info("SIGHUP received, reloading configuration");
            config_t *newcfg = cfg_create();
            if (newcfg && cfg_load(newcfg, conf_path) == 0) {
                log_info("configuration reloaded");
                const char *new_loglevel = cfg_get_str(newcfg, "daemon", "loglevel");
                if (new_loglevel) {
                    if (strcmp(new_loglevel, "debug") == 0)
                        log_set_level(LOG_LVL_DEBUG);
                    else if (strcmp(new_loglevel, "info") == 0)
                        log_set_level(LOG_LVL_INFO);
                    else if (strcmp(new_loglevel, "warn") == 0)
                        log_set_level(LOG_LVL_WARN);
                    else if (strcmp(new_loglevel, "error") == 0)
                        log_set_level(LOG_LVL_ERROR);
                }
                cfg_free(newcfg);
            } else {
                log_error("config reload failed, keeping old config");
                cfg_free(newcfg);
            }
        }

        if (g_reopen_logs) {
            g_reopen_logs = 0;
            log_info("SIGUSR1 received, reopening log files");
            log_reopen();
        }

        if (g_dump_status) {
            g_dump_status = 0;
            log_info("status: connections=%d model=%s ready=%d",
                     srv->nconns, backend_model_name(backend),
                     backend_ready(backend));
        }
    }

    log_info("shutting down...");

    http_destroy(srv);
    backend_free(backend);
    metrics_set_model_loaded(&metrics, 0);

    close(pidfd);
    unlink(pidpath);

    if (unix_path && *unix_path)
        unlink(unix_path);

    cfg_free(cfg);
    log_info("%s stopped", BITNETD_NAME);
    log_close();

    return 0;
}
