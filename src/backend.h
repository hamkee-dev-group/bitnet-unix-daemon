#ifndef BITNETD_BACKEND_H
#define BITNETD_BACKEND_H

#include "config.h"
#include <stddef.h>

typedef struct backend backend_t;

typedef int (*token_cb_fn)(const char *token, size_t len, void *userdata);

/* Why generation stopped. */
typedef enum {
    BACKEND_FINISH_STOP   =  0,  /* EOS / EOT / control-token stop */
    BACKEND_FINISH_LENGTH =  1,  /* max_tokens exhausted           */
    BACKEND_FINISH_ERROR  = -1   /* backend or generation failure   */
} backend_finish_t;

backend_t       *backend_init(const config_t *cfg);
void             backend_free(backend_t *b);
int              backend_ready(const backend_t *b);
backend_finish_t backend_generate(backend_t *b, const char *prompt, int max_tokens,
                                  double temperature, int top_k, double top_p,
                                  token_cb_fn cb, void *userdata);
const char      *backend_model_name(const backend_t *b);
const char      *backend_chat_template(const backend_t *b);

#endif
