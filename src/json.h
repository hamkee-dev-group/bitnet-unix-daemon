#ifndef BITNETD_JSON_H
#define BITNETD_JSON_H

#include <stddef.h>

enum json_type {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
};

typedef struct json_value json_value_t;

struct json_value {
    enum json_type type;
    union {
        int           boolean;
        double        number;
        struct {
            char     *data;
            size_t    len;
        } string;
        struct {
            json_value_t **items;
            size_t         count;
            size_t         cap;
        } array;
        struct {
            char        **keys;
            json_value_t **vals;
            size_t         count;
            size_t         cap;
        } object;
    } u;
};

json_value_t *json_parse(const char *src, size_t len);
void          json_free(json_value_t *v);

json_value_t *json_get(const json_value_t *obj, const char *key);
const char   *json_get_str(const json_value_t *obj, const char *key);
double        json_get_num(const json_value_t *obj, const char *key, double def);
int           json_get_bool_val(const json_value_t *obj, const char *key, int def);
size_t        json_array_len(const json_value_t *arr);
json_value_t *json_array_get(const json_value_t *arr, size_t idx);

int json_emit_start(char *buf, size_t cap);
int json_emit_obj_open(char *buf, size_t cap, int pos);
int json_emit_obj_close(char *buf, size_t cap, int pos);
int json_emit_arr_open(char *buf, size_t cap, int pos);
int json_emit_arr_close(char *buf, size_t cap, int pos);
int json_emit_key(char *buf, size_t cap, int pos, const char *key);
int json_emit_str(char *buf, size_t cap, int pos, const char *val);
int json_emit_int(char *buf, size_t cap, int pos, long val);
int json_emit_dbl(char *buf, size_t cap, int pos, double val);
int json_emit_bool(char *buf, size_t cap, int pos, int val);
int json_emit_null(char *buf, size_t cap, int pos);
int json_emit_raw(char *buf, size_t cap, int pos, const char *raw, size_t len);

#endif
