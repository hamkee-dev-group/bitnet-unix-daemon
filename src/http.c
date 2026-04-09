#include "http.h"
#include "log.h"
#include "../include/bitnetd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>

static int
set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int
set_reuseaddr(int fd)
{
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
}

static http_conn_t *
conn_alloc(http_server_t *s)
{
    http_conn_t *c;
    if (s->free_list) {
        c = s->free_list;
        s->free_list = c->next;
    } else {
        c = calloc(1, sizeof(*c));
        if (!c) return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->rcap = HTTP_READ_BUF;
    c->rbuf = malloc(c->rcap);
    c->wcap = HTTP_WRITE_BUF;
    c->wbuf = malloc(c->wcap);
    if (!c->rbuf || !c->wbuf) {
        free(c->rbuf);
        free(c->wbuf);
        free(c);
        return NULL;
    }
    return c;
}

static void
conn_free(http_server_t *s, http_conn_t *c)
{
    if (c->fd >= 0) {
        poller_del(s->poller, c->fd);
        close(c->fd);
    }
    free(c->rbuf);
    free(c->wbuf);
    free(c->req.body);
    free(c->resp_body);
    c->rbuf = NULL;
    c->wbuf = NULL;
    c->req.body = NULL;
    c->resp_body = NULL;
    c->next = s->free_list;
    s->free_list = c;
    if (s->nconns > 0) s->nconns--;
}

http_server_t *
http_create(const config_t *cfg)
{
    http_server_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->listen_fd = -1;
    s->unix_fd   = -1;
    s->max_conns = cfg_get_int(cfg, "server", "max_connections",
                               BITNETD_DEFAULT_MAX_CONN);
    s->timeout   = cfg_get_int(cfg, "server", "request_timeout",
                               BITNETD_DEFAULT_TIMEOUT);
    s->max_body  = (size_t)cfg_get_int(cfg, "server", "max_body_size",
                                       BITNETD_DEFAULT_MAX_BODY);

    s->poller = poller_create(s->max_conns + 2);
    if (!s->poller) { free(s); return NULL; }

    s->conns = calloc((size_t)s->max_conns, sizeof(http_conn_t *));
    s->running = 1;
    return s;
}

void
http_destroy(http_server_t *s)
{
    if (!s) return;
    if (s->listen_fd >= 0) close(s->listen_fd);
    if (s->unix_fd >= 0)   close(s->unix_fd);
    poller_destroy(s->poller);
    http_conn_t *c = s->free_list;
    while (c) {
        http_conn_t *next = c->next;
        free(c);
        c = next;
    }
    free(s->conns);
    free(s);
}

int
http_listen_tcp(http_server_t *s, const char *addr, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log_error("socket: %s", strerror(errno)); return -1; }

    set_reuseaddr(fd);
    set_nonblock(fd);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (addr && *addr)
        inet_pton(AF_INET, addr, &sa.sin_addr);
    else
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        log_error("bind %s:%d: %s", addr ? addr : "127.0.0.1", port,
                  strerror(errno));
        close(fd);
        return -1;
    }

    int backlog = BITNETD_DEFAULT_BACKLOG;
    if (listen(fd, backlog) < 0) {
        log_error("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    s->listen_fd = fd;
    poller_add(s->poller, fd, POLLER_IN, NULL);
    log_info("listening on %s:%d", addr ? addr : "127.0.0.1", port);
    return 0;
}

int
http_listen_unix(http_server_t *s, const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    set_nonblock(fd);
    unlink(path);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        log_error("bind unix %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, BITNETD_DEFAULT_BACKLOG) < 0) {
        log_error("listen unix: %s", strerror(errno));
        close(fd);
        return -1;
    }

    s->unix_fd = fd;
    poller_add(s->poller, fd, POLLER_IN, NULL);
    log_info("listening on unix:%s", path);
    return 0;
}

void
http_add_route(http_server_t *s, const char *method, const char *path,
               http_handler_fn fn, void *ud)
{
    if (s->nroutes >= HTTP_MAX_ROUTES) return;
    http_route_t *r = &s->routes[s->nroutes++];
    snprintf(r->method, sizeof(r->method), "%s", method);
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->handler  = fn;
    r->userdata = ud;
}

static int
parse_request_line(http_conn_t *c, const char *line, size_t len)
{
    const char *p = line;
    const char *end = line + len;

    const char *sp = memchr(p, ' ', (size_t)(end - p));
    if (!sp) return -1;
    size_t mlen = (size_t)(sp - p);
    if (mlen >= sizeof(c->req.method)) return -1;
    memcpy(c->req.method, p, mlen);
    c->req.method[mlen] = '\0';

    p = sp + 1;
    sp = memchr(p, ' ', (size_t)(end - p));
    if (!sp) return -1;
    size_t plen = (size_t)(sp - p);
    if (plen >= sizeof(c->req.path)) return -1;
    memcpy(c->req.path, p, plen);
    c->req.path[plen] = '\0';

    p = sp + 1;
    if (end - p >= 8 && memcmp(p, "HTTP/", 5) == 0) {
        c->req.version_major = p[5] - '0';
        c->req.version_minor = p[7] - '0';
    }

    c->req.keep_alive = (c->req.version_minor >= 1) ? 1 : 0;
    return 0;
}

static int
parse_header(http_conn_t *c, const char *line, size_t len)
{
    const char *colon = memchr(line, ':', len);
    if (!colon) return -1;

    if (c->req.nheaders >= HTTP_MAX_HEADERS) return 0;
    http_header_t *h = &c->req.headers[c->req.nheaders];

    size_t nlen = (size_t)(colon - line);
    if (nlen >= sizeof(h->name)) nlen = sizeof(h->name) - 1;
    memcpy(h->name, line, nlen);
    h->name[nlen] = '\0';

    const char *val = colon + 1;
    while (val < line + len && *val == ' ') val++;
    size_t vlen = (size_t)(line + len - val);
    if (vlen >= sizeof(h->value)) vlen = sizeof(h->value) - 1;
    memcpy(h->value, val, vlen);
    h->value[vlen] = '\0';

    c->req.nheaders++;

    if (strcasecmp(h->name, "Content-Length") == 0) {
        c->req.content_length = (size_t)strtoul(h->value, NULL, 10);
    } else if (strcasecmp(h->name, "Connection") == 0) {
        if (strcasecmp(h->value, "close") == 0)
            c->req.keep_alive = 0;
        else if (strcasecmp(h->value, "keep-alive") == 0)
            c->req.keep_alive = 1;
    }
    return 0;
}

static int
try_parse_headers(http_conn_t *c)
{
    char *p = c->rbuf;
    char *end = c->rbuf + c->rlen;
    char *hdr_end = NULL;

    for (char *s = p; s + 3 < end; s++) {
        if (s[0] == '\r' && s[1] == '\n' && s[2] == '\r' && s[3] == '\n') {
            hdr_end = s;
            break;
        }
    }
    if (!hdr_end)
        return 0;

    char *line_end = memchr(p, '\r', (size_t)(hdr_end - p));
    if (!line_end) return -1;
    if (parse_request_line(c, p, (size_t)(line_end - p)) < 0)
        return -1;

    p = line_end + 2;
    while (p < hdr_end) {
        line_end = memchr(p, '\r', (size_t)(hdr_end - p + 1));
        if (!line_end) break;
        if (line_end > p)
            parse_header(c, p, (size_t)(line_end - p));
        p = line_end + 2;
    }

    c->headers_done = 1;
    c->body_start = (size_t)(hdr_end + 4 - c->rbuf);
    return 1;
}

static int
request_complete(http_conn_t *c)
{
    if (!c->headers_done) return 0;
    size_t body_received = c->rlen - c->body_start;
    return body_received >= c->req.content_length;
}

static void
extract_body(http_conn_t *c)
{
    if (c->req.content_length > 0) {
        c->req.body = malloc(c->req.content_length + 1);
        if (c->req.body) {
            memcpy(c->req.body, c->rbuf + c->body_start, c->req.content_length);
            c->req.body[c->req.content_length] = '\0';
            c->req.body_len = c->req.content_length;
        }
    }
}

void
http_resp_init(http_response_t *r, int status, const char *text)
{
    memset(r, 0, sizeof(*r));
    r->status = status;
    snprintf(r->status_text, sizeof(r->status_text), "%s", text);
}

void
http_resp_header(http_response_t *r, const char *name, const char *val)
{
    if (r->nheaders >= HTTP_MAX_HEADERS) return;
    http_header_t *h = &r->headers[r->nheaders++];
    snprintf(h->name, sizeof(h->name), "%s", name);
    snprintf(h->value, sizeof(h->value), "%s", val);
}

void
http_resp_body_json(http_response_t *r, const char *json, size_t len)
{
    r->body     = (char *)json;
    r->body_len = len;
    http_resp_header(r, "Content-Type", "application/json");
}

static int
format_response(http_conn_t *conn, http_response_t *resp)
{
    char *buf = conn->wbuf;
    size_t cap = conn->wcap;
    int n = snprintf(buf, cap, "HTTP/1.1 %d %s\r\n",
                     resp->status, resp->status_text);

    for (int i = 0; i < resp->nheaders; i++) {
        n += snprintf(buf + n, cap - (size_t)n, "%s: %s\r\n",
                      resp->headers[i].name, resp->headers[i].value);
    }

    if (!resp->streaming) {
        n += snprintf(buf + n, cap - (size_t)n, "Content-Length: %zu\r\n",
                      resp->body_len);
    }

    if (conn->req.keep_alive) {
        n += snprintf(buf + n, cap - (size_t)n, "Connection: keep-alive\r\n");
    } else {
        n += snprintf(buf + n, cap - (size_t)n, "Connection: close\r\n");
    }

    n += snprintf(buf + n, cap - (size_t)n, "\r\n");

    if (resp->body && resp->body_len > 0 && (size_t)n + resp->body_len < cap) {
        memcpy(buf + n, resp->body, resp->body_len);
        n += (int)resp->body_len;
    }

    conn->wlen = (size_t)n;
    conn->wpos = 0;
    return 0;
}

int
http_resp_send(http_conn_t *conn, http_response_t *resp)
{
    format_response(conn, resp);
    conn->state = CONN_WRITING;

    while (conn->wpos < conn->wlen) {
        ssize_t n = write(conn->fd, conn->wbuf + conn->wpos,
                          conn->wlen - conn->wpos);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return -1;
        }
        conn->wpos += (size_t)n;
    }

    if (conn->wpos >= conn->wlen) {
        free(conn->resp_body);
        conn->resp_body = NULL;

        if (conn->req.keep_alive) {
            conn->rlen = 0;
            conn->headers_done = 0;
            conn->body_start = 0;
            free(conn->req.body);
            conn->req.body = NULL;
            memset(&conn->req, 0, sizeof(conn->req));
            conn->state = CONN_READING;
        } else {
            conn->state = CONN_CLOSING;
            shutdown(conn->fd, SHUT_WR);
        }
    }

    return 0;
}

int
http_start_sse(http_conn_t *conn)
{
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    size_t len = strlen(hdr);
    ssize_t w = write(conn->fd, hdr, len);
    if (w < 0) return -1;
    conn->state = CONN_STREAMING;
    conn->chunked = 1;
    return 0;
}

int
http_send_sse(http_conn_t *conn, const char *data, size_t len)
{
    char prefix[] = "data: ";
    char suffix[] = "\n\n";

    struct iovec {
        char *base;
        size_t len;
    };

    ssize_t w;
    w = write(conn->fd, prefix, sizeof(prefix) - 1);
    if (w < 0) return -1;
    w = write(conn->fd, data, len);
    if (w < 0) return -1;
    w = write(conn->fd, suffix, sizeof(suffix) - 1);
    if (w < 0) return -1;
    return 0;
}

int
http_end_sse(http_conn_t *conn)
{
    const char *end_msg = "data: [DONE]\n\n";
    ssize_t w = write(conn->fd, end_msg, strlen(end_msg));
    conn->state = CONN_CLOSING;
    return (w < 0) ? -1 : 0;
}

const char *
http_req_header(const http_request_t *req, const char *name)
{
    for (int i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

static void
accept_conn(http_server_t *s, int listen_fd)
{
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    int fd = accept(listen_fd, (struct sockaddr *)&sa, &salen);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            log_error("accept: %s", strerror(errno));
        return;
    }

    if (s->nconns >= s->max_conns) {
        log_warn("max connections reached, rejecting");
        close(fd);
        return;
    }

    set_nonblock(fd);

    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    http_conn_t *c = conn_alloc(s);
    if (!c) { close(fd); return; }

    c->fd          = fd;
    c->state       = CONN_READING;
    c->last_active = time(NULL);

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&sa;
        inet_ntop(AF_INET, &sin->sin_addr, c->client_ip, sizeof(c->client_ip));
    } else if (sa.ss_family == AF_UNIX) {
        snprintf(c->client_ip, sizeof(c->client_ip), "unix");
    }

    poller_add(s->poller, fd, POLLER_IN, c);
    s->nconns++;
    log_debug("accepted connection from %s (fd=%d, total=%d)",
              c->client_ip, fd, s->nconns);
}

static void
dispatch_request(http_server_t *s, http_conn_t *c)
{
    c->state = CONN_PROCESSING;
    extract_body(c);

    log_debug("%s %s from %s", c->req.method, c->req.path, c->client_ip);

    for (int i = 0; i < s->nroutes; i++) {
        http_route_t *r = &s->routes[i];
        if (strcmp(r->method, c->req.method) == 0 &&
            strcmp(r->path, c->req.path) == 0) {
            http_response_t resp;
            memset(&resp, 0, sizeof(resp));
            r->handler(c, &c->req, &resp, r->userdata);
            if (c->state == CONN_PROCESSING) {
                http_resp_init(&resp, 500, "Internal Server Error");
                http_resp_body_json(&resp, "{\"error\":\"no response\"}", 22);
                http_resp_send(c, &resp);
            }
            return;
        }
    }

    for (int i = 0; i < s->nroutes; i++) {
        http_route_t *r = &s->routes[i];
        if (strcmp(r->method, "*") == 0 &&
            strcmp(r->path, c->req.path) == 0) {
            http_response_t resp;
            memset(&resp, 0, sizeof(resp));
            r->handler(c, &c->req, &resp, r->userdata);
            if (c->state == CONN_PROCESSING) {
                http_resp_init(&resp, 500, "Internal Server Error");
                http_resp_body_json(&resp, "{\"error\":\"no response\"}", 22);
                http_resp_send(c, &resp);
            }
            return;
        }
    }

    http_response_t resp;
    http_resp_init(&resp, 404, "Not Found");
    const char *body = "{\"error\":\"not found\"}";
    http_resp_body_json(&resp, body, strlen(body));
    http_resp_send(c, &resp);
}

static void
handle_read(http_server_t *s, http_conn_t *c)
{
    if (c->rlen >= c->rcap) {
        size_t nc = c->rcap * 2;
        if (nc > s->max_body + 4096) {
            log_warn("request too large from %s", c->client_ip);
            conn_free(s, c);
            return;
        }
        char *nb = realloc(c->rbuf, nc);
        if (!nb) { conn_free(s, c); return; }
        c->rbuf = nb;
        c->rcap = nc;
    }

    ssize_t n = read(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            conn_free(s, c);
        }
        return;
    }

    c->rlen += (size_t)n;
    c->last_active = time(NULL);

    if (!c->headers_done) {
        int rc = try_parse_headers(c);
        if (rc < 0) {
            conn_free(s, c);
            return;
        }
        if (rc == 0) return;

        if (c->req.content_length > s->max_body) {
            log_warn("body too large (%zu) from %s",
                     c->req.content_length, c->client_ip);
            http_response_t resp;
            http_resp_init(&resp, 413, "Payload Too Large");
            const char *body = "{\"error\":\"request body too large\"}";
            http_resp_body_json(&resp, body, strlen(body));
            http_resp_send(c, &resp);
            return;
        }
    }

    if (request_complete(c)) {
        dispatch_request(s, c);
    }
}

static void
handle_write(http_server_t *s, http_conn_t *c)
{
    if (c->wpos >= c->wlen) {
        free(c->resp_body);
        c->resp_body = NULL;

        if (c->req.keep_alive && c->state != CONN_CLOSING) {
            c->rlen = 0;
            c->headers_done = 0;
            c->body_start = 0;
            free(c->req.body);
            c->req.body = NULL;
            memset(&c->req, 0, sizeof(c->req));
            c->state = CONN_READING;
            poller_mod(s->poller, c->fd, POLLER_IN);
        } else {
            conn_free(s, c);
        }
        return;
    }

    ssize_t n = write(c->fd, c->wbuf + c->wpos, c->wlen - c->wpos);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            conn_free(s, c);
        return;
    }
    c->wpos += (size_t)n;
    c->last_active = time(NULL);
}

int
http_run_once(http_server_t *s, int timeout_ms)
{
    poller_event_t events[64];
    int n = poller_wait(s->poller, events, 64, timeout_ms);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        int fd = events[i].fd;

        if (fd == s->listen_fd || fd == s->unix_fd) {
            accept_conn(s, fd);
            continue;
        }

        http_conn_t *c = (http_conn_t *)events[i].userdata;
        if (!c) continue;

        if (events[i].events & (POLLER_ERR | POLLER_HUP)) {
            conn_free(s, c);
            continue;
        }

        if (events[i].events & POLLER_IN) {
            if (c->state == CONN_READING)
                handle_read(s, c);
        }

        if (events[i].events & POLLER_OUT) {
            if (c->state == CONN_WRITING)
                handle_write(s, c);
        }
    }

    return n;
}
