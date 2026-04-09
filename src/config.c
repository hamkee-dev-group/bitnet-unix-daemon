#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CFG_MAX_ENTRIES 256
#define CFG_MAX_LINE    1024

typedef struct {
    char section[64];
    char key[64];
    char value[512];
} cfg_entry_t;

struct config {
    cfg_entry_t entries[CFG_MAX_ENTRIES];
    size_t      count;
};

config_t *
cfg_create(void)
{
    config_t *c = calloc(1, sizeof(*c));
    return c;
}

void
cfg_free(config_t *c)
{
    free(c);
}

static char *
strip(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

int
cfg_load(config_t *c, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[CFG_MAX_LINE];
    char section[64] = "global";

    while (fgets(line, sizeof(line), fp)) {
        char *s = strip(line);

        if (*s == '\0' || *s == '#' || *s == ';')
            continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", s + 1);
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq)
            continue;

        *eq = '\0';
        char *key = strip(s);
        char *val = strip(eq + 1);

        if (c->count < CFG_MAX_ENTRIES) {
            cfg_entry_t *e = &c->entries[c->count++];
            snprintf(e->section, sizeof(e->section), "%s", section);
            snprintf(e->key, sizeof(e->key), "%s", key);
            snprintf(e->value, sizeof(e->value), "%s", val);
        }
    }

    fclose(fp);
    return 0;
}

const char *
cfg_get_str(const config_t *c, const char *section, const char *key)
{
    if (!c)
        return NULL;
    for (size_t i = 0; i < c->count; i++) {
        if (strcmp(c->entries[i].section, section) == 0 &&
            strcmp(c->entries[i].key, key) == 0)
            return c->entries[i].value;
    }
    return NULL;
}

int
cfg_get_int(const config_t *c, const char *section, const char *key, int def)
{
    const char *v = cfg_get_str(c, section, key);
    if (!v)
        return def;
    char *end;
    long val = strtol(v, &end, 10);
    if (end == v)
        return def;
    return (int)val;
}

int
cfg_get_bool(const config_t *c, const char *section, const char *key, int def)
{
    const char *v = cfg_get_str(c, section, key);
    if (!v)
        return def;
    if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0)
        return 1;
    if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "0") == 0)
        return 0;
    return def;
}

int
cfg_set(config_t *c, const char *section, const char *key, const char *val)
{
    for (size_t i = 0; i < c->count; i++) {
        if (strcmp(c->entries[i].section, section) == 0 &&
            strcmp(c->entries[i].key, key) == 0) {
            snprintf(c->entries[i].value, sizeof(c->entries[i].value), "%s", val);
            return 0;
        }
    }
    if (c->count >= CFG_MAX_ENTRIES)
        return -1;
    cfg_entry_t *e = &c->entries[c->count++];
    snprintf(e->section, sizeof(e->section), "%s", section);
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->value, sizeof(e->value), "%s", val);
    return 0;
}
