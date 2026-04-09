#ifndef BITNETD_BACKEND_H
#define BITNETD_BACKEND_H

#include "config.h"
#include <stddef.h>

typedef struct backend backend_t;

typedef int (*token_cb_fn)(const char *token, size_t len, void *userdata);

backend_t  *backend_init(const config_t *cfg);
void        backend_free(backend_t *b);
int         backend_ready(const backend_t *b);
int         backend_generate(backend_t *b, const char *prompt, int max_tokens,
                             double temperature, token_cb_fn cb, void *userdata);
const char *backend_model_name(const backend_t *b);

#endif
