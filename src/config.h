#ifndef BITNETD_CONFIG_H
#define BITNETD_CONFIG_H

#include <stddef.h>

typedef struct config config_t;

config_t   *cfg_create(void);
void        cfg_free(config_t *c);
int         cfg_load(config_t *c, const char *path);
const char *cfg_get_str(const config_t *c, const char *section, const char *key);
int         cfg_get_int(const config_t *c, const char *section, const char *key, int def);
int         cfg_get_bool(const config_t *c, const char *section, const char *key, int def);
int         cfg_set(config_t *c, const char *section, const char *key, const char *val);

#endif
