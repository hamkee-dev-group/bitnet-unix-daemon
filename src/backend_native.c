#include "backend.h"
#include "log.h"

#include "bitnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_MAX_TOKENS 8192

struct backend {
    bitnet_model_t *model;
    bitnet_ctx_t   *ctx;
    char            model_name[128];
    int             ready;
    int             default_top_k;
    float           default_top_p;
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
    int ctx_size = cfg_get_int(cfg, "model", "ctx_size", 2048);

    b->default_top_k = cfg_get_int(cfg, "model", "top_k", 40);
    b->default_top_p = (float)cfg_get_int(cfg, "model", "top_p", 95) / 100.0f;
    {
        const char *tp = cfg_get_str(cfg, "model", "top_p");
        if (tp && strchr(tp, '.'))
            b->default_top_p = (float)atof(tp);
    }

    const char *slash = strrchr(model_path, '/');
    if (slash) {
        snprintf(b->model_name, sizeof(b->model_name), "%s", slash + 1);
        char *dot = strstr(b->model_name, ".gguf");
        if (dot) *dot = '\0';
    } else {
        snprintf(b->model_name, sizeof(b->model_name), "bitnet");
    }

    log_info("backend[native]: loading model %s", model_path);
    log_info("backend[native]: threads=%d, ctx_size=%d", threads, ctx_size);

    b->model = bitnet_model_load(model_path);
    if (!b->model) {
        log_error("backend[native]: failed to load model");
        free(b);
        return NULL;
    }

    bitnet_params_t params = bitnet_params_default();
    params.n_threads   = threads;
    params.n_ctx       = ctx_size;
    params.temperature = 0.8f;
    params.top_k       = b->default_top_k;
    params.top_p       = b->default_top_p;

    b->ctx = bitnet_ctx_new(b->model, params);
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

/* Control markers that signal end-of-turn or chat structure boundaries. */
static const char *control_markers[] = {
    "<|end_of_text|>",
    "<|eot_id|>",
    "<|end_of_header|>",
    "<|start_of_header|>",
    "<|begin_of_text|>",
    NULL
};

static int
is_control_token(const char *text)
{
    if (!text) return 0;
    for (const char **m = control_markers; *m; m++) {
        if (strcmp(text, *m) == 0)
            return 1;
    }
    return 0;
}

int
backend_generate(backend_t *b, const char *prompt, int max_tokens,
                 double temperature, token_cb_fn cb, void *userdata)
{
    if (!b || !b->ready || !b->ctx) return -1;

    bitnet_ctx_t *ctx = b->ctx;

    ctx->kv_len = 0;

    ctx->sampler.temperature = (float)temperature;

    int tokens[NATIVE_MAX_TOKENS];
    int n_tokens = bitnet_tokenize(ctx, prompt, tokens, NATIVE_MAX_TOKENS);
    if (n_tokens <= 0) {
        log_error("backend[native]: tokenization failed");
        return -1;
    }

    log_debug("backend[native]: prompt = %d tokens, generating up to %d",
              n_tokens, max_tokens);

    int eos = bn_token_eos(ctx->tokenizer);

    /* Prefill: run all prompt tokens through the model. */
    float *logits = NULL;
    for (int i = 0; i < n_tokens; i++) {
        int need_logits = (i == n_tokens - 1);
        float *ret = bitnet_forward(ctx, &tokens[i], 1, need_logits);
        if (need_logits && !ret) return -1;
        if (!need_logits && ctx->kv_len == 0) return -1;
        if (ret) logits = ret;
    }

    /* Decode loop: sample, check for stop conditions, then emit. */
    int generated = 0;
    for (int i = 0; i < max_tokens; i++) {
        int token = bn_sample(&ctx->sampler, logits, ctx->model->n_vocab);

        if (token == eos)
            break;

        const char *text = bn_token_text(ctx->tokenizer, token);
        if (is_control_token(text))
            break;

        if (cb && text)
            cb(text, strlen(text), userdata);

        generated++;
        logits = bitnet_forward(ctx, &token, 1, 1);
        if (!logits) return -1;
    }

    log_debug("backend[native]: generated %d tokens", generated);
    return 0;
}
