#include "backend.h"
#include "bitnetd.h"
#include "log.h"

#include "bitnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct backend {
    bitnet_model_t  *model;
    bitnet_ctx_t    *ctx;
    bitnet_params_t  params;
    char             model_name[128];
    char             chat_template[64];
    int              ready;
};

backend_t *
backend_init(const config_t *cfg)
{
    backend_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    const char *model_path = cfg_get_str(cfg, "model", "path");
    if (!model_path) {
        log_error("backend: model path not configured (set [model] path in config)");
        free(b);
        return NULL;
    }

    int threads  = cfg_get_int(cfg, "model", "threads", 4);
    int ctx_size = cfg_get_int(cfg, "model", "ctx_size", BITNETD_DEFAULT_CTX_SIZE);

    int   default_top_k = cfg_get_int(cfg, "model", "top_k", 40);
    float default_top_p = (float)cfg_get_int(cfg, "model", "top_p", 95) / 100.0f;
    {
        const char *tp = cfg_get_str(cfg, "model", "top_p");
        if (tp && strchr(tp, '.'))
            default_top_p = (float)atof(tp);
    }
    float default_temp = 0.8f;  /* documented default: conf/bitnetd.conf.example */
    {
        const char *tv = cfg_get_str(cfg, "model", "temperature");
        if (tv)
            default_temp = (float)atof(tv);
    }

    const char *slash = strrchr(model_path, '/');
    if (slash) {
        snprintf(b->model_name, sizeof(b->model_name), "%s", slash + 1);
        char *dot = strstr(b->model_name, ".gguf");
        if (dot) *dot = '\0';
    } else {
        snprintf(b->model_name, sizeof(b->model_name), "bitnet");
    }

    const char *tmpl = cfg_get_str(cfg, "model", "chat_template");
    snprintf(b->chat_template, sizeof(b->chat_template), "%s",
             tmpl ? tmpl : "bitnet");

    log_info("backend[native]: loading model %s", model_path);
    log_info("backend[native]: threads=%d, ctx_size=%d", threads, ctx_size);

    b->model = bitnet_model_load(model_path);
    if (!b->model) {
        log_error("backend[native]: failed to load model");
        free(b);
        return NULL;
    }

    b->params = bitnet_params_default();
    b->params.n_threads   = threads;
    b->params.n_ctx       = ctx_size;
    b->params.temperature = default_temp;
    b->params.top_k       = default_top_k;
    b->params.top_p       = default_top_p;

    b->ctx = bitnet_ctx_new(b->model, b->params);
    if (!b->ctx) {
        log_error("backend[native]: failed to create context");
        bitnet_model_free(b->model);
        free(b);
        return NULL;
    }

    b->ready = 1;
    log_info("backend[native]: ready (%s)", b->model_name);
    return b;
}

void
backend_free(backend_t *b)
{
    if (!b) return;
    log_info("backend[native]: shutting down");
    if (b->ctx)   bitnet_ctx_free(b->ctx);
    if (b->model) bitnet_model_free(b->model);
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

const char *
backend_chat_template(const backend_t *b)
{
    return b ? b->chat_template : "bitnet";
}

/* Control markers that signal end-of-turn or chat structure boundaries. */
static const char *control_markers[] = {
    "<|end_of_text|>",
    "<|eot_id|>",
    "<|end_of_header|>",
    "<|start_of_header|>",
    "<|end_header_id|>",
    "<|start_header_id|>",
    "<|begin_of_text|>",
    NULL
};

static int
is_control_token(const char *text)
{
    if (!text) return 0;
    /* Exact match against known markers. */
    for (const char **m = control_markers; *m; m++) {
        if (strcmp(text, *m) == 0)
            return 1;
    }
    /* Catch-all: any <|...|> pattern is a special token. */
    size_t len = strlen(text);
    if (len >= 5 && text[0] == '<' && text[1] == '|'
                  && text[len-1] == '>' && text[len-2] == '|')
        return 1;
    return 0;
}

struct gen_state {
    token_cb_fn cb;
    void       *userdata;
    int         stopped_eos;   /* set when a control token ends generation */
};

static bool
gen_callback(int token, const char *text, void *ud)
{
    (void)token;
    struct gen_state *s = ud;
    if (is_control_token(text)) {
        s->stopped_eos = 1;
        return false;
    }
    if (s->cb && text)
        s->cb(text, strlen(text), s->userdata);
    return true;
}

backend_finish_t
backend_generate(backend_t *b, const char *prompt, int max_tokens,
                 double temperature, int top_k, double top_p,
                 token_cb_fn cb, void *userdata)
{
    if (!b || !b->ready) return BACKEND_FINISH_ERROR;

    /* Reset KV cache for a fresh generation and reinit sampler with
     * per-request parameters through the public API.                   */
    bitnet_kv_clear(b->ctx);

    float req_temp  = (float)temperature;
    int   req_top_k = (top_k >= 0)  ? top_k        : b->params.top_k;
    float req_top_p = (top_p >= 0.0) ? (float)top_p : b->params.top_p;
    bitnet_sampler_configure(b->ctx, req_temp, req_top_k, req_top_p);

    int ctx_budget = b->params.n_ctx;
    int *tokens = malloc(ctx_budget * sizeof(*tokens));
    if (!tokens) {
        log_error("backend[native]: failed to allocate token buffer");
        return BACKEND_FINISH_ERROR;
    }
    int n_tokens = bitnet_tokenize(b->ctx, prompt, tokens, ctx_budget);
    if (n_tokens <= 0) {
        log_error("backend[native]: tokenization failed");
        free(tokens);
        return BACKEND_FINISH_ERROR;
    }

    if (n_tokens + max_tokens > ctx_budget) {
        log_error("backend[native]: prompt (%d) + max_tokens (%d) exceeds "
                  "context window (%d)", n_tokens, max_tokens, ctx_budget);
        free(tokens);
        return BACKEND_FINISH_ERROR;
    }

    log_debug("backend[native]: prompt = %d tokens, generating up to %d",
              n_tokens, max_tokens);

    struct gen_state state = { .cb = cb, .userdata = userdata, .stopped_eos = 0 };
    int generated = bitnet_generate(b->ctx, tokens, n_tokens, max_tokens,
                                    gen_callback, &state);

    free(tokens);
    if (generated < 0) return BACKEND_FINISH_ERROR;

    log_debug("backend[native]: generated %d tokens", generated);

    if (state.stopped_eos)
        return BACKEND_FINISH_STOP;
    if (generated >= max_tokens)
        return BACKEND_FINISH_LENGTH;
    return BACKEND_FINISH_STOP;
}
