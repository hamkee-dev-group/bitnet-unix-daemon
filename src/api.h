#ifndef BITNETD_API_H
#define BITNETD_API_H

#include "http.h"
#include "backend.h"
#include "metrics.h"

typedef struct api_ctx {
    backend_t *backend;
    metrics_t *metrics;
} api_ctx_t;

void api_register_routes(http_server_t *srv, api_ctx_t *ctx);

void api_chat_completions(http_conn_t *conn, http_request_t *req,
                          http_response_t *resp, void *ud);
void api_completions(http_conn_t *conn, http_request_t *req,
                     http_response_t *resp, void *ud);
void api_models(http_conn_t *conn, http_request_t *req,
                http_response_t *resp, void *ud);
void api_health(http_conn_t *conn, http_request_t *req,
                http_response_t *resp, void *ud);
void api_metrics(http_conn_t *conn, http_request_t *req,
                 http_response_t *resp, void *ud);

#endif
