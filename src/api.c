#include "api.h"
#include "json.h"
#include "log.h"
#include "../include/bitnetd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define API_BUF_SIZE 131072

/* ── Control-token filter ──────────────────────────────────────────
 * Buffers partial <|…|> sequences.  Complete control tokens are
 * silently dropped; anything else is forwarded to the downstream
 * emit callback.  Both streaming and non-streaming chat paths feed
 * tokens through this filter so their visible output is identical.
 */

#define TOKEN_FILTER_BUF 64

typedef struct {
    char    pending[TOKEN_FILTER_BUF];
    size_t  plen;
    int     backend_tokens;
    int     (*emit)(const char *text, size_t len, void *ud);
    void   *ud;
} token_filter_t;

static const char *control_tokens[] = {
    "<|begin_of_text|>",
    "<|end_of_text|>",
    "<|start_header_id|>",
    "<|end_header_id|>",
    "<|eot_id|>",
    NULL
};

static int
is_control_token(const char *s, size_t len)
{
    for (const char **ct = control_tokens; *ct; ct++)
        if (len == strlen(*ct) && memcmp(s, *ct, len) == 0)
            return 1;
    return 0;
}

static int
could_be_control_prefix(const char *s, size_t len)
{
    for (const char **ct = control_tokens; *ct; ct++) {
        size_t ctlen = strlen(*ct);
        if (len <= ctlen && memcmp(s, *ct, len) == 0)
            return 1;
    }
    return 0;
}

static void
token_filter_init(token_filter_t *f,
                  int (*emit)(const char *, size_t, void *), void *ud)
{
    f->plen = 0;
    f->backend_tokens = 0;
    f->emit = emit;
    f->ud   = ud;
}

static int
token_filter_feed(token_filter_t *f, const char *token, size_t len)
{
    size_t i = 0;
    while (i < len) {
        if (f->plen > 0) {
            char ch = token[i];
            if (f->plen + 1 < TOKEN_FILTER_BUF) {
                f->pending[f->plen] = ch;
                if (could_be_control_prefix(f->pending, f->plen + 1)) {
                    f->plen++;
                    if (ch == '>' && is_control_token(f->pending, f->plen)) {
                        f->plen = 0;          /* drop control token */
                    }
                    i++;
                    continue;
                }
            }
            /* Not a valid prefix — flush pending as normal text */
            int rc = f->emit(f->pending, f->plen, f->ud);
            if (rc) return rc;
            f->plen = 0;
            /* reprocess current char below */
            continue;
        }

        /* Fast-scan for the next '<' */
        size_t start = i;
        while (i < len && token[i] != '<') i++;
        if (i > start) {
            int rc = f->emit(token + start, i - start, f->ud);
            if (rc) return rc;
        }
        if (i < len) {                       /* hit a '<' */
            f->pending[0] = '<';
            f->plen = 1;
            i++;
        }
    }
    return 0;
}

static int
token_filter_flush(token_filter_t *f)
{
    if (f->plen > 0) {
        int rc = f->emit(f->pending, f->plen, f->ud);
        f->plen = 0;
        return rc;
    }
    return 0;
}

/* Callback handed to backend_generate — feeds through the filter. */
static int
filtered_token_cb(const char *token, size_t len, void *ud)
{
    token_filter_t *f = (token_filter_t *)ud;
    f->backend_tokens++;
    return token_filter_feed(f, token, len);
}

/* ── End of filter ─────────────────────────────────────────────── */

static int
apply_chat_template_llama3(const json_value_t *messages, char *buf, size_t cap)
{
    int pos = 0;
    pos += snprintf(buf + pos, cap - (size_t)pos,
                    "<|begin_of_text|>");

    size_t n = json_array_len(messages);
    for (size_t i = 0; i < n; i++) {
        json_value_t *msg = json_array_get(messages, i);
        if (!msg) continue;

        const char *role    = json_get_str(msg, "role");
        const char *content = json_get_str(msg, "content");
        if (!role || !content) continue;

        pos += snprintf(buf + pos, cap - (size_t)pos,
            "<|start_header_id|>%s<|end_header_id|>\n\n%s<|eot_id|>",
            role, content);
    }

    pos += snprintf(buf + pos, cap - (size_t)pos,
        "<|start_header_id|>assistant<|end_header_id|>\n\n");

    return pos;
}

static int
apply_chat_template_bitnet(const json_value_t *messages, char *buf, size_t cap)
{
    int pos = 0;

    size_t n = json_array_len(messages);
    for (size_t i = 0; i < n; i++) {
        json_value_t *msg = json_array_get(messages, i);
        if (!msg) continue;

        const char *role    = json_get_str(msg, "role");
        const char *content = json_get_str(msg, "content");
        if (!role || !content) continue;

        pos += snprintf(buf + pos, cap - (size_t)pos,
            "%s: %s\n", role, content);
    }

    pos += snprintf(buf + pos, cap - (size_t)pos, "assistant:");

    return pos;
}

static int
apply_chat_template(const json_value_t *messages, const char *tmpl,
                    char *buf, size_t cap)
{
    if (strcmp(tmpl, "llama3") == 0)
        return apply_chat_template_llama3(messages, buf, cap);
    return apply_chat_template_bitnet(messages, buf, cap);
}

static void
gen_id(char *buf, size_t cap, const char *prefix)
{
    static long counter;
    long ts = (long)time(NULL);
    snprintf(buf, cap, "%s-%lx-%04lx", prefix, ts, ++counter & 0xFFFF);
}

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
    int     token_count;   /* used by legacy /v1/completions */
} collect_ctx_t;

/* Emit callback: appends (sanitized) text to buffer. */
static int
collect_emit(const char *text, size_t len, void *ud)
{
    collect_ctx_t *ctx = (collect_ctx_t *)ud;
    if (ctx->len + len >= ctx->cap) return -1;
    memcpy(ctx->buf + ctx->len, text, len);
    ctx->len += len;
    ctx->buf[ctx->len] = '\0';
    return 0;
}

/* Raw token callback for /v1/completions (no control-token filtering). */
static int
collect_token(const char *token, size_t len, void *ud)
{
    collect_ctx_t *ctx = (collect_ctx_t *)ud;
    ctx->token_count++;
    return collect_emit(token, len, ud);
}

typedef struct {
    http_conn_t *conn;
    char         id[64];
    const char  *model;
} stream_ctx_t;

/* Emit callback for streaming: JSON-escapes sanitized text and sends SSE. */
static int
stream_emit(const char *text, size_t len, void *ud)
{
    stream_ctx_t *ctx = (stream_ctx_t *)ud;
    char chunk[API_BUF_SIZE];

    char escaped[4096];
    size_t ei = 0;
    for (size_t i = 0; i < len && ei + 6 < sizeof(escaped); i++) {
        switch (text[i]) {
        case '"':  escaped[ei++] = '\\'; escaped[ei++] = '"';  break;
        case '\\': escaped[ei++] = '\\'; escaped[ei++] = '\\'; break;
        case '\n': escaped[ei++] = '\\'; escaped[ei++] = 'n';  break;
        case '\r': escaped[ei++] = '\\'; escaped[ei++] = 'r';  break;
        case '\t': escaped[ei++] = '\\'; escaped[ei++] = 't';  break;
        default:   escaped[ei++] = text[i]; break;
        }
    }
    escaped[ei] = '\0';

    int n = snprintf(chunk, sizeof(chunk),
        "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\","
        "\"created\":%ld,\"model\":\"%s\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%s\"},"
        "\"finish_reason\":null}]}",
        ctx->id, (long)time(NULL), ctx->model, escaped);

    http_send_sse(ctx->conn, chunk, (size_t)n);
    return 0;
}

void
api_chat_completions(http_conn_t *conn, http_request_t *req,
                     http_response_t *resp, void *ud)
{
    api_ctx_t *ctx = (api_ctx_t *)ud;
    metrics_inc_requests(ctx->metrics);

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    json_value_t *root = json_parse(req->body, req->body_len);
    if (!root) {
        http_resp_init(resp, 400, "Bad Request");
        const char *err = "{\"error\":{\"message\":\"invalid JSON\",\"type\":\"invalid_request_error\"}}";
        http_resp_body_json(resp, err, strlen(err));
        http_resp_send(conn, resp);
        metrics_inc_errors(ctx->metrics);
        return;
    }

    json_value_t *messages = json_get(root, "messages");
    if (!messages || messages->type != JSON_ARRAY) {
        json_free(root);
        http_resp_init(resp, 400, "Bad Request");
        const char *err = "{\"error\":{\"message\":\"messages is required\",\"type\":\"invalid_request_error\"}}";
        http_resp_body_json(resp, err, strlen(err));
        http_resp_send(conn, resp);
        metrics_inc_errors(ctx->metrics);
        return;
    }

    int max_tokens    = (int)json_get_num(root, "max_tokens", 256);
    double temperature = json_get_num(root, "temperature", 0.8);
    int top_k         = (int)json_get_num(root, "top_k", -1);
    double top_p      = json_get_num(root, "top_p", -1.0);
    int stream        = json_get_bool_val(root, "stream", 0);

    char *prompt = malloc(API_BUF_SIZE);
    if (!prompt) {
        json_free(root);
        http_resp_init(resp, 500, "Internal Server Error");
        const char *err = "{\"error\":{\"message\":\"out of memory\"}}";
        http_resp_body_json(resp, err, strlen(err));
        http_resp_send(conn, resp);
        return;
    }
    apply_chat_template(messages, backend_chat_template(ctx->backend),
                        prompt, API_BUF_SIZE);
    json_free(root);

    const char *model_name = backend_model_name(ctx->backend);

    if (stream) {
        stream_ctx_t sctx;
        sctx.conn = conn;
        sctx.model = model_name;
        gen_id(sctx.id, sizeof(sctx.id), "chatcmpl");

        token_filter_t filt;
        token_filter_init(&filt, stream_emit, &sctx);

        http_start_sse(conn);
        backend_finish_t finish = backend_generate(ctx->backend, prompt,
                             max_tokens, temperature,
                             top_k, top_p, filtered_token_cb, &filt);
        token_filter_flush(&filt);

        if (finish == BACKEND_FINISH_ERROR) {
            char err_chunk[1024];
            int en = snprintf(err_chunk, sizeof(err_chunk),
                "{\"error\":{\"message\":\"inference failed\","
                "\"type\":\"server_error\"}}");
            http_send_sse(conn, err_chunk, (size_t)en);
            http_end_sse(conn);
            metrics_inc_errors(ctx->metrics);
            free(prompt);
        } else {
            const char *reason = (finish == BACKEND_FINISH_LENGTH)
                                 ? "length" : "stop";
            char final_chunk[1024];
            int fn = snprintf(final_chunk, sizeof(final_chunk),
                "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\","
                "\"created\":%ld,\"model\":\"%s\","
                "\"choices\":[{\"index\":0,\"delta\":{},"
                "\"finish_reason\":\"%s\"}]}",
                sctx.id, (long)time(NULL), model_name, reason);
            http_send_sse(conn, final_chunk, (size_t)fn);
            http_end_sse(conn);

            metrics_add_tokens(ctx->metrics, filt.backend_tokens);
            free(prompt);
        }
    } else {
        collect_ctx_t cctx;
        cctx.cap = API_BUF_SIZE;
        cctx.buf = malloc(cctx.cap);
        cctx.len = 0;

        if (!cctx.buf) {
            free(prompt);
            http_resp_init(resp, 500, "Internal Server Error");
            const char *err = "{\"error\":{\"message\":\"out of memory\"}}";
            http_resp_body_json(resp, err, strlen(err));
            http_resp_send(conn, resp);
            return;
        }

        token_filter_t filt;
        token_filter_init(&filt, collect_emit, &cctx);

        int prompt_tokens = 10;
        {
            int spaces = 0;
            for (const char *pp = prompt; *pp; pp++)
                if (*pp == ' ') spaces++;
            if (spaces > 0) prompt_tokens = spaces;
        }

        backend_finish_t finish = backend_generate(ctx->backend, prompt,
                                  max_tokens, temperature, top_k, top_p,
                                  filtered_token_cb, &filt);
        token_filter_flush(&filt);
        free(prompt);

        if (finish == BACKEND_FINISH_ERROR) {
            free(cctx.buf);
            http_resp_init(resp, 500, "Internal Server Error");
            const char *err = "{\"error\":{\"message\":\"inference failed\"}}";
            http_resp_body_json(resp, err, strlen(err));
            http_resp_send(conn, resp);
            metrics_inc_errors(ctx->metrics);
            return;
        }

        char id[64];
        gen_id(id, sizeof(id), "chatcmpl");

        size_t escaped_cap = cctx.len * 2 + 1;
        char *escaped = malloc(escaped_cap);
        if (!escaped) { free(cctx.buf); return; }
        size_t ei = 0;
        for (size_t i = 0; i < cctx.len && ei + 6 < escaped_cap; i++) {
            switch (cctx.buf[i]) {
            case '"':  escaped[ei++] = '\\'; escaped[ei++] = '"';  break;
            case '\\': escaped[ei++] = '\\'; escaped[ei++] = '\\'; break;
            case '\n': escaped[ei++] = '\\'; escaped[ei++] = 'n';  break;
            case '\r': escaped[ei++] = '\\'; escaped[ei++] = 'r';  break;
            case '\t': escaped[ei++] = '\\'; escaped[ei++] = 't';  break;
            default:   escaped[ei++] = cctx.buf[i]; break;
            }
        }
        escaped[ei] = '\0';

        const char *reason = (finish == BACKEND_FINISH_LENGTH)
                             ? "length" : "stop";

        char *out = malloc(API_BUF_SIZE);
        if (!out) { free(cctx.buf); free(escaped); return; }

        int n = snprintf(out, API_BUF_SIZE,
            "{\"id\":\"%s\","
            "\"object\":\"chat.completion\","
            "\"created\":%ld,"
            "\"model\":\"%s\","
            "\"choices\":[{"
                "\"index\":0,"
                "\"message\":{\"role\":\"assistant\",\"content\":\"%s\"},"
                "\"finish_reason\":\"%s\""
            "}],"
            "\"usage\":{"
                "\"prompt_tokens\":%d,"
                "\"completion_tokens\":%d,"
                "\"total_tokens\":%d"
            "}}",
            id, (long)time(NULL), model_name,
            escaped, reason,
            prompt_tokens, filt.backend_tokens,
            prompt_tokens + filt.backend_tokens);

        http_resp_init(resp, 200, "OK");
        http_resp_body_json(resp, out, (size_t)n);
        http_resp_send(conn, resp);

        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        double elapsed = (double)(t_end.tv_sec - t_start.tv_sec) +
                         (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;
        metrics_observe_latency(ctx->metrics, elapsed);
        metrics_add_tokens(ctx->metrics, filt.backend_tokens);

        if (elapsed > 0 && filt.backend_tokens > 0)
            metrics_set_tps(ctx->metrics,
                            (double)filt.backend_tokens / elapsed);

        free(cctx.buf);
        free(escaped);
        conn->resp_body = out;
    }
}

void
api_completions(http_conn_t *conn, http_request_t *req,
                http_response_t *resp, void *ud)
{
    api_ctx_t *ctx = (api_ctx_t *)ud;
    metrics_inc_requests(ctx->metrics);

    json_value_t *root = json_parse(req->body, req->body_len);
    if (!root) {
        http_resp_init(resp, 400, "Bad Request");
        const char *err = "{\"error\":{\"message\":\"invalid JSON\"}}";
        http_resp_body_json(resp, err, strlen(err));
        http_resp_send(conn, resp);
        return;
    }

    const char *prompt = json_get_str(root, "prompt");
    int max_tokens     = (int)json_get_num(root, "max_tokens", 256);
    double temperature = json_get_num(root, "temperature", 0.8);
    int top_k          = (int)json_get_num(root, "top_k", -1);
    double top_p       = json_get_num(root, "top_p", -1.0);

    if (!prompt) {
        json_free(root);
        http_resp_init(resp, 400, "Bad Request");
        const char *err = "{\"error\":{\"message\":\"prompt is required\"}}";
        http_resp_body_json(resp, err, strlen(err));
        http_resp_send(conn, resp);
        return;
    }

    collect_ctx_t cctx;
    cctx.cap = API_BUF_SIZE;
    cctx.buf = malloc(cctx.cap);
    cctx.len = 0;
    cctx.token_count = 0;

    if (!cctx.buf) {
        json_free(root);
        http_resp_init(resp, 500, "Internal Server Error");
        const char *err = "{\"error\":{\"message\":\"out of memory\"}}";
        http_resp_body_json(resp, err, strlen(err));
        http_resp_send(conn, resp);
        return;
    }

    backend_generate(ctx->backend, prompt, max_tokens, temperature,
                     top_k, top_p, collect_token, &cctx);
    json_free(root);

    char id[64];
    gen_id(id, sizeof(id), "cmpl");

    size_t escaped_cap = cctx.len * 2 + 1;
    char *escaped = malloc(escaped_cap);
    if (!escaped) { free(cctx.buf); return; }
    size_t ei = 0;
    for (size_t i = 0; i < cctx.len && ei + 6 < escaped_cap; i++) {
        switch (cctx.buf[i]) {
        case '"':  escaped[ei++] = '\\'; escaped[ei++] = '"';  break;
        case '\\': escaped[ei++] = '\\'; escaped[ei++] = '\\'; break;
        case '\n': escaped[ei++] = '\\'; escaped[ei++] = 'n';  break;
        case '\r': escaped[ei++] = '\\'; escaped[ei++] = 'r';  break;
        default:   escaped[ei++] = cctx.buf[i]; break;
        }
    }
    escaped[ei] = '\0';

    char *out = malloc(API_BUF_SIZE);
    if (!out) { free(cctx.buf); free(escaped); return; }

    int n = snprintf(out, API_BUF_SIZE,
        "{\"id\":\"%s\","
        "\"object\":\"text_completion\","
        "\"created\":%ld,"
        "\"model\":\"%s\","
        "\"choices\":[{"
            "\"index\":0,"
            "\"text\":\"%s\","
            "\"finish_reason\":\"stop\""
        "}],"
        "\"usage\":{"
            "\"prompt_tokens\":0,"
            "\"completion_tokens\":%d,"
            "\"total_tokens\":%d"
        "}}",
        id, (long)time(NULL), backend_model_name(ctx->backend),
        escaped, cctx.token_count, cctx.token_count);

    http_resp_init(resp, 200, "OK");
    http_resp_body_json(resp, out, (size_t)n);
    http_resp_send(conn, resp);
    conn->resp_body = out;

    metrics_add_tokens(ctx->metrics, cctx.token_count);
    free(cctx.buf);
    free(escaped);
}

void
api_models(http_conn_t *conn, http_request_t *req,
           http_response_t *resp, void *ud)
{
    (void)req;
    api_ctx_t *ctx = (api_ctx_t *)ud;
    metrics_inc_requests(ctx->metrics);

    char *out = malloc(1024);
    if (!out) return;

    int n = snprintf(out, 1024,
        "{\"object\":\"list\","
        "\"data\":[{"
            "\"id\":\"%s\","
            "\"object\":\"model\","
            "\"created\":%ld,"
            "\"owned_by\":\"bitnetd\""
        "}]}",
        backend_model_name(ctx->backend), (long)time(NULL));

    http_resp_init(resp, 200, "OK");
    http_resp_body_json(resp, out, (size_t)n);
    http_resp_send(conn, resp);
    conn->resp_body = out;
}

void
api_health(http_conn_t *conn, http_request_t *req,
           http_response_t *resp, void *ud)
{
    (void)req;
    api_ctx_t *ctx = (api_ctx_t *)ud;

    if (backend_ready(ctx->backend)) {
        http_resp_init(resp, 200, "OK");
        const char *ok = "{\"status\":\"ok\"}";
        http_resp_body_json(resp, ok, strlen(ok));
    } else {
        http_resp_init(resp, 503, "Service Unavailable");
        const char *loading = "{\"status\":\"loading\"}";
        http_resp_body_json(resp, loading, strlen(loading));
    }
    http_resp_send(conn, resp);
}

void
api_metrics(http_conn_t *conn, http_request_t *req,
            http_response_t *resp, void *ud)
{
    (void)req;
    api_ctx_t *ctx = (api_ctx_t *)ud;

    char *buf = malloc(8192);
    if (!buf) return;

    int n = metrics_render(ctx->metrics, buf, 8192);

    http_resp_init(resp, 200, "OK");
    http_resp_header(resp, "Content-Type",
                     "text/plain; version=0.0.4; charset=utf-8");
    resp->body     = buf;
    resp->body_len = (size_t)n;
    http_resp_send(conn, resp);
    conn->resp_body = buf;
}

void
api_register_routes(http_server_t *srv, api_ctx_t *ctx)
{
    http_add_route(srv, "POST", "/v1/chat/completions",
                   api_chat_completions, ctx);
    http_add_route(srv, "POST", "/v1/completions",
                   api_completions, ctx);
    http_add_route(srv, "GET",  "/v1/models",
                   api_models, ctx);
    http_add_route(srv, "GET",  "/health",
                   api_health, ctx);
    http_add_route(srv, "GET",  "/metrics",
                   api_metrics, ctx);
}
