#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PIDFILE "/var/run/bitnetd.pid"
#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_PORT    8080
#define BUF_SIZE        65536

static pid_t
read_pid(const char *pidfile)
{
    FILE *fp = fopen(pidfile, "r");
    if (!fp) return -1;
    pid_t pid = 0;
    if (fscanf(fp, "%d", &pid) != 1)
        pid = -1;
    fclose(fp);
    return pid;
}

static int
http_get(const char *host, int port, const char *path, char *buf, size_t cap)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &sa.sin_addr);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }

    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n", path, host);

    ssize_t w = write(fd, req, (size_t)rlen);
    if (w < 0) { close(fd); return -1; }

    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, cap - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    close(fd);

    char *body = strstr(buf, "\r\n\r\n");
    if (body) {
        body += 4;
        memmove(buf, body, strlen(body) + 1);
    }
    return 0;
}

static void
cmd_status(const char *host, int port)
{
    char buf[BUF_SIZE];
    if (http_get(host, port, "/health", buf, sizeof(buf)) < 0) {
        fprintf(stderr, "error: cannot connect to %s:%d\n", host, port);
        exit(1);
    }
    printf("%s\n", buf);
}

static void
cmd_metrics(const char *host, int port)
{
    char buf[BUF_SIZE];
    if (http_get(host, port, "/metrics", buf, sizeof(buf)) < 0) {
        fprintf(stderr, "error: cannot connect to %s:%d\n", host, port);
        exit(1);
    }
    printf("%s", buf);
}

static void
cmd_models(const char *host, int port)
{
    char buf[BUF_SIZE];
    if (http_get(host, port, "/v1/models", buf, sizeof(buf)) < 0) {
        fprintf(stderr, "error: cannot connect to %s:%d\n", host, port);
        exit(1);
    }
    printf("%s\n", buf);
}

static void
cmd_reload(const char *pidfile)
{
    pid_t pid = read_pid(pidfile);
    if (pid <= 0) {
        fprintf(stderr, "error: cannot read pidfile %s\n", pidfile);
        exit(1);
    }
    if (kill(pid, SIGHUP) < 0) {
        fprintf(stderr, "error: kill(%d, SIGHUP): %s\n", pid, strerror(errno));
        exit(1);
    }
    printf("sent SIGHUP to pid %d\n", pid);
}

static void
cmd_stop(const char *pidfile)
{
    pid_t pid = read_pid(pidfile);
    if (pid <= 0) {
        fprintf(stderr, "error: cannot read pidfile %s\n", pidfile);
        exit(1);
    }
    if (kill(pid, SIGTERM) < 0) {
        fprintf(stderr, "error: kill(%d, SIGTERM): %s\n", pid, strerror(errno));
        exit(1);
    }
    printf("sent SIGTERM to pid %d\n", pid);
}

static void
usage(void)
{
    fprintf(stderr,
        "Usage: bitnetctl [options] <command>\n"
        "\n"
        "Commands:\n"
        "  status    Show daemon health status\n"
        "  metrics   Show Prometheus metrics\n"
        "  models    List loaded models\n"
        "  reload    Send SIGHUP to reload config\n"
        "  stop      Send SIGTERM for graceful shutdown\n"
        "\n"
        "Options:\n"
        "  -H HOST   Daemon host (default: " DEFAULT_HOST ")\n"
        "  -p PORT   Daemon port (default: %d)\n"
        "  -P FILE   PID file (default: " DEFAULT_PIDFILE ")\n",
        DEFAULT_PORT);
}

int
main(int argc, char **argv)
{
    const char *host    = DEFAULT_HOST;
    int         port    = DEFAULT_PORT;
    const char *pidfile = DEFAULT_PIDFILE;
    int opt;

    while ((opt = getopt(argc, argv, "H:p:P:h")) != -1) {
        switch (opt) {
        case 'H': host    = optarg;       break;
        case 'p': port    = atoi(optarg); break;
        case 'P': pidfile = optarg;       break;
        case 'h':
        default:
            usage();
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (optind >= argc) {
        usage();
        return 1;
    }

    const char *cmd = argv[optind];

    if (strcmp(cmd, "status") == 0)       cmd_status(host, port);
    else if (strcmp(cmd, "metrics") == 0) cmd_metrics(host, port);
    else if (strcmp(cmd, "models") == 0)  cmd_models(host, port);
    else if (strcmp(cmd, "reload") == 0)  cmd_reload(pidfile);
    else if (strcmp(cmd, "stop") == 0)    cmd_stop(pidfile);
    else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        usage();
        return 1;
    }

    return 0;
}
