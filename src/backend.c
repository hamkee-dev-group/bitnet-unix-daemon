#include "backend.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#define BACKEND_PORT       18088
#define BACKEND_MAX_RETRY  30
#define BACKEND_BUF_SIZE   65536

struct backend {
    pid_t       child_pid;
    int         port;
    char        model_path[512];
    char        server_path[512];
    int         threads;
    int         ctx_size;
    int         ready;
    char        model_name[128];
};

static int
tcp_connect(const char *host, int port)
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
    return fd;
}

static int
http_request(int port, const char *method, const char *path,
             const char *body, size_t body_len,
             char *resp_buf, size_t resp_cap)
{
    int fd = tcp_connect("127.0.0.1", port);
    if (fd < 0) return -1;

    char hdr[1024];
    int hlen;
    if (body && body_len > 0) {
        hlen = snprintf(hdr, sizeof(hdr),
            "%s %s HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, body_len);
    } else {
        hlen = snprintf(hdr, sizeof(hdr),
            "%s %s HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path);
    }

    ssize_t w = write(fd, hdr, (size_t)hlen);
    if (w < 0) { close(fd); return -1; }

    if (body && body_len > 0) {
        w = write(fd, body, body_len);
        if (w < 0) { close(fd); return -1; }
    }

    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, resp_buf + total, resp_cap - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= resp_cap - 1) break;
    }
    resp_buf[total] = '\0';
    close(fd);

    return (int)total;
}

static int
wait_for_ready(int port, int max_seconds)
{
    for (int i = 0; i < max_seconds; i++) {
        char buf[4096];
        int n = http_request(port, "GET", "/health", NULL, 0, buf, sizeof(buf));
        if (n > 0) {
            if (strstr(buf, "200 OK") || strstr(buf, "\"ok\""))
                return 0;
        }
        sleep(1);
    }
    return -1;
}

backend_t *
backend_init(const config_t *cfg)
{
    backend_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    const char *model = cfg_get_str(cfg, "model", "path");
    if (!model) {
        log_error("backend: model path not configured (set [model] path in config)");
        free(b);
        return NULL;
    }
    snprintf(b->model_path, sizeof(b->model_path), "%s", model);

    const char *server = cfg_get_str(cfg, "backend", "server_path");
    if (!server) server = "llama-server";
    snprintf(b->server_path, sizeof(b->server_path), "%s", server);

    b->threads  = cfg_get_int(cfg, "model", "threads", 4);
    b->ctx_size = cfg_get_int(cfg, "model", "ctx_size", 4096);
    b->port     = cfg_get_int(cfg, "backend", "port", BACKEND_PORT);

    const char *slash = strrchr(b->model_path, '/');
    if (slash) {
        snprintf(b->model_name, sizeof(b->model_name), "%s", slash + 1);
        char *dot = strstr(b->model_name, ".gguf");
        if (dot) *dot = '\0';
    } else {
        snprintf(b->model_name, sizeof(b->model_name), "bitnet");
    }

    log_info("backend: starting llama-server on port %d", b->port);
    log_info("backend: model = %s", b->model_path);
    log_info("backend: threads = %d, ctx_size = %d", b->threads, b->ctx_size);

    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork: %s", strerror(errno));
        free(b);
        return NULL;
    }

    if (pid == 0) {
        char port_str[16], threads_str[16], ctx_str[16];
        snprintf(port_str, sizeof(port_str), "%d", b->port);
        snprintf(threads_str, sizeof(threads_str), "%d", b->threads);
        snprintf(ctx_str, sizeof(ctx_str), "%d", b->ctx_size);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execlp(b->server_path, "llama-server",
              "-m", b->model_path,
              "--port", port_str,
              "-t", threads_str,
              "-c", ctx_str,
              "--host", "127.0.0.1",
              (char *)NULL);
        _exit(127);
    }

    b->child_pid = pid;
    log_info("backend: llama-server started (pid=%d)", (int)pid);

    log_info("backend: waiting for llama-server to load model...");
    if (wait_for_ready(b->port, BACKEND_MAX_RETRY) < 0) {
        log_error("backend: llama-server failed to become ready in %d seconds",
                  BACKEND_MAX_RETRY);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        free(b);
        return NULL;
    }

    b->ready = 1;
    log_info("backend: llama-server is ready");
    return b;
}

void
backend_free(backend_t *b)
{
    if (!b) return;
    if (b->child_pid > 0) {
        log_info("backend: stopping llama-server (pid=%d)", (int)b->child_pid);
        kill(b->child_pid, SIGTERM);
        int status;
        waitpid(b->child_pid, &status, 0);
        log_info("backend: llama-server exited");
    }
    free(b);
}

int
backend_ready(const backend_t *b)
{
    return b ? b->ready : 0;
}

const char *
backend_model_name(const backend_t *b)
{
    return b ? b->model_name : "unknown";
}

backend_finish_t
backend_generate(backend_t *b, const char *prompt, int max_tokens,
                 double temperature, int top_k, double top_p,
                 token_cb_fn cb, void *userdata)
{
    if (!b || !b->ready) return BACKEND_FINISH_ERROR;

    char *req_body = malloc(strlen(prompt) + 512);
    if (!req_body) return BACKEND_FINISH_ERROR;

    size_t escaped_cap = strlen(prompt) * 2 + 1;
    char *escaped = malloc(escaped_cap);
    if (!escaped) { free(req_body); return BACKEND_FINISH_ERROR; }

    size_t ei = 0;
    for (const char *p = prompt; *p; p++) {
        if (ei + 6 >= escaped_cap) break;
        switch (*p) {
        case '"':  escaped[ei++] = '\\'; escaped[ei++] = '"';  break;
        case '\\': escaped[ei++] = '\\'; escaped[ei++] = '\\'; break;
        case '\n': escaped[ei++] = '\\'; escaped[ei++] = 'n';  break;
        case '\r': escaped[ei++] = '\\'; escaped[ei++] = 'r';  break;
        case '\t': escaped[ei++] = '\\'; escaped[ei++] = 't';  break;
        default:
            if ((unsigned char)*p < 0x20) {
                ei += (size_t)snprintf(escaped + ei, escaped_cap - ei,
                                       "\\u%04x", (unsigned char)*p);
            } else {
                escaped[ei++] = *p;
            }
            break;
        }
    }
    escaped[ei] = '\0';

    int pos = snprintf(req_body, strlen(prompt) + 512,
        "{\"prompt\":\"%s\",\"n_predict\":%d,\"temperature\":%.2f",
        escaped, max_tokens, temperature);
    free(escaped);
    if (top_k >= 0)
        pos += snprintf(req_body + pos, 64, ",\"top_k\":%d", top_k);
    if (top_p >= 0.0)
        pos += snprintf(req_body + pos, 64, ",\"top_p\":%.4f", top_p);
    pos += snprintf(req_body + pos, 64, ",\"stream\":false,\"cache_prompt\":false}");
    int body_len = pos;

    char *resp = malloc(BACKEND_BUF_SIZE);
    if (!resp) { free(req_body); return BACKEND_FINISH_ERROR; }

    int n = http_request(b->port, "POST", "/completion",
                         req_body, (size_t)body_len,
                         resp, BACKEND_BUF_SIZE);
    free(req_body);

    if (n <= 0) {
        log_error("backend: request failed");
        free(resp);
        return BACKEND_FINISH_ERROR;
    }

    char *json_body = strstr(resp, "\r\n\r\n");
    if (!json_body) {
        log_error("backend: malformed response");
        free(resp);
        return BACKEND_FINISH_ERROR;
    }
    json_body += 4;

    char *content_key = strstr(json_body, "\"content\"");
    if (!content_key) {
        log_error("backend: no content in response");
        free(resp);
        return BACKEND_FINISH_ERROR;
    }

    char *colon = strchr(content_key, ':');
    if (!colon) { free(resp); return BACKEND_FINISH_ERROR; }

    char *p = colon + 1;
    while (*p == ' ' || *p == '\t') p++;

    if (*p != '"') { free(resp); return BACKEND_FINISH_ERROR; }
    p++;

    size_t content_cap = (size_t)(resp + n - p);
    char *content = malloc(content_cap + 1);
    if (!content) { free(resp); return BACKEND_FINISH_ERROR; }

    size_t ci = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  content[ci++] = '"';  break;
            case '\\': content[ci++] = '\\'; break;
            case 'n':  content[ci++] = '\n'; break;
            case 'r':  content[ci++] = '\r'; break;
            case 't':  content[ci++] = '\t'; break;
            case '/':  content[ci++] = '/';  break;
            default:   content[ci++] = *p;   break;
            }
        } else {
            content[ci++] = *p;
        }
        p++;
    }
    content[ci] = '\0';

    if (cb && ci > 0) {
        cb(content, ci, userdata);
    }

    /* Determine finish reason from llama-server response fields. */
    backend_finish_t finish = BACKEND_FINISH_STOP;
    if (strstr(json_body, "\"stopped_limit\":true"))
        finish = BACKEND_FINISH_LENGTH;

    free(content);
    free(resp);
    return finish;
}
