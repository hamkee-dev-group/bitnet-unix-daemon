#ifndef BITNETD_HTTP_H
#define BITNETD_HTTP_H

#include "poller.h"
#include "config.h"
#include <stddef.h>
#include <time.h>

#define HTTP_MAX_HEADERS   64
#define HTTP_MAX_PATH      2048
#define HTTP_READ_BUF      8192
#define HTTP_WRITE_BUF     131072

typedef struct http_header {
    char  name[128];
    char  value[512];
} http_header_t;

typedef struct http_request {
    char            method[16];
    char            path[HTTP_MAX_PATH];
    int             version_major;
    int             version_minor;
    http_header_t   headers[HTTP_MAX_HEADERS];
    int             nheaders;
    char           *body;
    size_t          body_len;
    size_t          content_length;
    int             keep_alive;
} http_request_t;

typedef struct http_response {
    int             status;
    char            status_text[64];
    http_header_t   headers[HTTP_MAX_HEADERS];
    int             nheaders;
    char           *body;
    size_t          body_len;
    int             streaming;
} http_response_t;

enum conn_state {
    CONN_READING = 0,
    CONN_PROCESSING,
    CONN_WRITING,
    CONN_STREAMING,
    CONN_CLOSING
};

typedef struct http_conn {
    int             fd;
    enum conn_state state;
    char            client_ip[64];
    time_t          last_active;

    char           *rbuf;
    size_t          rlen;
    size_t          rcap;
    int             headers_done;
    size_t          body_start;

    char           *wbuf;
    size_t          wlen;
    size_t          wpos;
    size_t          wcap;

    http_request_t  req;

    int             chunked;

    char           *resp_body;

    struct http_conn *next;
} http_conn_t;

typedef void (*http_handler_fn)(http_conn_t *conn, http_request_t *req,
                                http_response_t *resp, void *ud);

typedef struct http_route {
    char            method[16];
    char            path[HTTP_MAX_PATH];
    http_handler_fn handler;
    void           *userdata;
} http_route_t;

#define HTTP_MAX_ROUTES 32

typedef struct http_server {
    int             listen_fd;
    int             unix_fd;
    poller_t       *poller;
    http_conn_t    *conns;
    int             max_conns;
    int             nconns;
    http_conn_t    *free_list;
    http_route_t    routes[HTTP_MAX_ROUTES];
    int             nroutes;
    int             timeout;
    size_t          max_body;
    int             running;
} http_server_t;

http_server_t *http_create(const config_t *cfg);
void           http_destroy(http_server_t *s);
int            http_listen_tcp(http_server_t *s, const char *addr, int port);
int            http_listen_unix(http_server_t *s, const char *path);
void           http_add_route(http_server_t *s, const char *method,
                              const char *path, http_handler_fn fn, void *ud);
int            http_run_once(http_server_t *s, int timeout_ms);

void http_resp_init(http_response_t *r, int status, const char *text);
void http_resp_header(http_response_t *r, const char *name, const char *val);
void http_resp_body_json(http_response_t *r, const char *json, size_t len);
int  http_resp_send(http_conn_t *conn, http_response_t *resp);

int  http_start_sse(http_conn_t *conn);
int  http_send_sse(http_conn_t *conn, const char *data, size_t len);
int  http_end_sse(http_conn_t *conn);

const char *http_req_header(const http_request_t *req, const char *name);

#endif
