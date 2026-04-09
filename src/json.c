#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
} jctx_t;

static void skip_ws(jctx_t *j)
{
    while (j->pos < j->len && isspace((unsigned char)j->src[j->pos]))
        j->pos++;
}

static int peek(jctx_t *j)
{
    skip_ws(j);
    return (j->pos < j->len) ? j->src[j->pos] : -1;
}

static json_value_t *alloc_val(enum json_type t)
{
    json_value_t *v = calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

static json_value_t *parse_value(jctx_t *j);

static json_value_t *
parse_string(jctx_t *j)
{
    if (j->src[j->pos] != '"')
        return NULL;
    j->pos++;

    size_t start = j->pos;
    size_t out_len = 0;
    size_t scan = j->pos;
    while (scan < j->len && j->src[scan] != '"') {
        if (j->src[scan] == '\\') {
            scan++;
            if (scan >= j->len)
                return NULL;
            if (j->src[scan] == 'u') {
                scan += 4;
                out_len += 4;
            } else {
                out_len++;
            }
            scan++;
        } else {
            out_len++;
            scan++;
        }
    }
    if (scan >= j->len)
        return NULL;

    char *buf = malloc(out_len + 1);
    if (!buf)
        return NULL;

    size_t w = 0;
    j->pos = start;
    while (j->pos < j->len && j->src[j->pos] != '"') {
        if (j->src[j->pos] == '\\') {
            j->pos++;
            switch (j->src[j->pos]) {
            case '"':  buf[w++] = '"';  break;
            case '\\': buf[w++] = '\\'; break;
            case '/':  buf[w++] = '/';  break;
            case 'b':  buf[w++] = '\b'; break;
            case 'f':  buf[w++] = '\f'; break;
            case 'n':  buf[w++] = '\n'; break;
            case 'r':  buf[w++] = '\r'; break;
            case 't':  buf[w++] = '\t'; break;
            case 'u':
                j->pos++;
                {
                    char hex[5] = {0};
                    for (int i = 0; i < 4 && j->pos < j->len; i++)
                        hex[i] = j->src[j->pos++];
                    unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                    if (cp < 0x80) {
                        buf[w++] = (char)cp;
                    } else if (cp < 0x800) {
                        buf[w++] = (char)(0xC0 | (cp >> 6));
                        buf[w++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[w++] = (char)(0xE0 | (cp >> 12));
                        buf[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[w++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                continue;
            default:
                buf[w++] = j->src[j->pos];
                break;
            }
            j->pos++;
        } else {
            buf[w++] = j->src[j->pos++];
        }
    }
    buf[w] = '\0';
    j->pos++;

    json_value_t *v = alloc_val(JSON_STRING);
    if (!v) { free(buf); return NULL; }
    v->u.string.data = buf;
    v->u.string.len  = w;
    return v;
}

static json_value_t *
parse_number(jctx_t *j)
{
    const char *start = j->src + j->pos;
    char *end;
    double d = strtod(start, &end);
    if (end == start)
        return NULL;
    j->pos += (size_t)(end - start);
    json_value_t *v = alloc_val(JSON_NUMBER);
    if (v) v->u.number = d;
    return v;
}

static json_value_t *
parse_object(jctx_t *j)
{
    j->pos++;
    json_value_t *obj = alloc_val(JSON_OBJECT);
    if (!obj) return NULL;

    size_t cap = 8;
    obj->u.object.keys = malloc(cap * sizeof(char *));
    obj->u.object.vals = malloc(cap * sizeof(json_value_t *));
    if (!obj->u.object.keys || !obj->u.object.vals) {
        json_free(obj);
        return NULL;
    }
    obj->u.object.cap = cap;

    if (peek(j) == '}') { j->pos++; return obj; }

    for (;;) {
        skip_ws(j);
        json_value_t *ks = parse_string(j);
        if (!ks) { json_free(obj); return NULL; }

        skip_ws(j);
        if (j->pos >= j->len || j->src[j->pos] != ':') {
            json_free(ks); json_free(obj); return NULL;
        }
        j->pos++;

        json_value_t *val = parse_value(j);
        if (!val) { json_free(ks); json_free(obj); return NULL; }

        if (obj->u.object.count >= obj->u.object.cap) {
            size_t nc = obj->u.object.cap * 2;
            char **nk = realloc(obj->u.object.keys, nc * sizeof(char *));
            json_value_t **nv = realloc(obj->u.object.vals, nc * sizeof(json_value_t *));
            if (!nk || !nv) {
                json_free(ks); json_free(val); json_free(obj);
                return NULL;
            }
            obj->u.object.keys = nk;
            obj->u.object.vals = nv;
            obj->u.object.cap  = nc;
        }

        size_t idx = obj->u.object.count++;
        obj->u.object.keys[idx] = ks->u.string.data;
        ks->u.string.data = NULL;
        json_free(ks);
        obj->u.object.vals[idx] = val;

        skip_ws(j);
        if (j->pos < j->len && j->src[j->pos] == ',') {
            j->pos++;
            continue;
        }
        break;
    }

    skip_ws(j);
    if (j->pos >= j->len || j->src[j->pos] != '}') {
        json_free(obj);
        return NULL;
    }
    j->pos++;
    return obj;
}

static json_value_t *
parse_array(jctx_t *j)
{
    j->pos++;
    json_value_t *arr = alloc_val(JSON_ARRAY);
    if (!arr) return NULL;

    size_t cap = 8;
    arr->u.array.items = malloc(cap * sizeof(json_value_t *));
    if (!arr->u.array.items) { json_free(arr); return NULL; }
    arr->u.array.cap = cap;

    if (peek(j) == ']') { j->pos++; return arr; }

    for (;;) {
        json_value_t *val = parse_value(j);
        if (!val) { json_free(arr); return NULL; }

        if (arr->u.array.count >= arr->u.array.cap) {
            size_t nc = arr->u.array.cap * 2;
            json_value_t **ni = realloc(arr->u.array.items, nc * sizeof(json_value_t *));
            if (!ni) { json_free(val); json_free(arr); return NULL; }
            arr->u.array.items = ni;
            arr->u.array.cap   = nc;
        }
        arr->u.array.items[arr->u.array.count++] = val;

        skip_ws(j);
        if (j->pos < j->len && j->src[j->pos] == ',') {
            j->pos++;
            continue;
        }
        break;
    }

    skip_ws(j);
    if (j->pos >= j->len || j->src[j->pos] != ']') {
        json_free(arr);
        return NULL;
    }
    j->pos++;
    return arr;
}

static json_value_t *
parse_value(jctx_t *j)
{
    int c = peek(j);
    if (c < 0) return NULL;

    switch (c) {
    case '"': return parse_string(j);
    case '{': return parse_object(j);
    case '[': return parse_array(j);
    case 't':
        if (j->pos + 4 <= j->len && memcmp(j->src + j->pos, "true", 4) == 0) {
            j->pos += 4;
            json_value_t *v = alloc_val(JSON_BOOL);
            if (v) v->u.boolean = 1;
            return v;
        }
        return NULL;
    case 'f':
        if (j->pos + 5 <= j->len && memcmp(j->src + j->pos, "false", 5) == 0) {
            j->pos += 5;
            json_value_t *v = alloc_val(JSON_BOOL);
            if (v) v->u.boolean = 0;
            return v;
        }
        return NULL;
    case 'n':
        if (j->pos + 4 <= j->len && memcmp(j->src + j->pos, "null", 4) == 0) {
            j->pos += 4;
            return alloc_val(JSON_NULL);
        }
        return NULL;
    default:
        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number(j);
        return NULL;
    }
}

json_value_t *
json_parse(const char *src, size_t len)
{
    if (!src || len == 0)
        return NULL;
    jctx_t j = { .src = src, .len = len, .pos = 0 };
    return parse_value(&j);
}

void
json_free(json_value_t *v)
{
    if (!v) return;
    switch (v->type) {
    case JSON_STRING:
        free(v->u.string.data);
        break;
    case JSON_ARRAY:
        for (size_t i = 0; i < v->u.array.count; i++)
            json_free(v->u.array.items[i]);
        free(v->u.array.items);
        break;
    case JSON_OBJECT:
        for (size_t i = 0; i < v->u.object.count; i++) {
            free(v->u.object.keys[i]);
            json_free(v->u.object.vals[i]);
        }
        free(v->u.object.keys);
        free(v->u.object.vals);
        break;
    default:
        break;
    }
    free(v);
}

json_value_t *
json_get(const json_value_t *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.keys[i], key) == 0)
            return obj->u.object.vals[i];
    }
    return NULL;
}

const char *
json_get_str(const json_value_t *obj, const char *key)
{
    json_value_t *v = json_get(obj, key);
    if (!v || v->type != JSON_STRING)
        return NULL;
    return v->u.string.data;
}

double
json_get_num(const json_value_t *obj, const char *key, double def)
{
    json_value_t *v = json_get(obj, key);
    if (!v || v->type != JSON_NUMBER)
        return def;
    return v->u.number;
}

int
json_get_bool_val(const json_value_t *obj, const char *key, int def)
{
    json_value_t *v = json_get(obj, key);
    if (!v || v->type != JSON_BOOL)
        return def;
    return v->u.boolean;
}

size_t
json_array_len(const json_value_t *arr)
{
    if (!arr || arr->type != JSON_ARRAY)
        return 0;
    return arr->u.array.count;
}

json_value_t *
json_array_get(const json_value_t *arr, size_t idx)
{
    if (!arr || arr->type != JSON_ARRAY || idx >= arr->u.array.count)
        return NULL;
    return arr->u.array.items[idx];
}

static int
emit_char(char *buf, size_t cap, int pos, char c)
{
    if (pos < 0) return -1;
    if ((size_t)pos < cap) buf[pos] = c;
    return pos + 1;
}

static int
emit_mem(char *buf, size_t cap, int pos, const char *s, size_t n)
{
    if (pos < 0) return -1;
    for (size_t i = 0; i < n; i++) {
        if ((size_t)(pos + (int)i) < cap)
            buf[pos + (int)i] = s[i];
    }
    return pos + (int)n;
}

int json_emit_start(char *buf, size_t cap)
{
    (void)buf; (void)cap;
    return 0;
}

int json_emit_obj_open(char *buf, size_t cap, int pos)
{
    return emit_char(buf, cap, pos, '{');
}

int json_emit_obj_close(char *buf, size_t cap, int pos)
{
    if (pos > 0 && (size_t)(pos - 1) < cap && buf[pos - 1] == ',')
        pos--;
    return emit_char(buf, cap, pos, '}');
}

int json_emit_arr_open(char *buf, size_t cap, int pos)
{
    return emit_char(buf, cap, pos, '[');
}

int json_emit_arr_close(char *buf, size_t cap, int pos)
{
    if (pos > 0 && (size_t)(pos - 1) < cap && buf[pos - 1] == ',')
        pos--;
    return emit_char(buf, cap, pos, ']');
}

int json_emit_key(char *buf, size_t cap, int pos, const char *key)
{
    pos = emit_char(buf, cap, pos, '"');
    pos = emit_mem(buf, cap, pos, key, strlen(key));
    pos = emit_char(buf, cap, pos, '"');
    pos = emit_char(buf, cap, pos, ':');
    return pos;
}

int json_emit_str(char *buf, size_t cap, int pos, const char *val)
{
    pos = emit_char(buf, cap, pos, '"');
    if (val) {
        for (const char *p = val; *p; p++) {
            switch (*p) {
            case '"':  pos = emit_mem(buf, cap, pos, "\\\"", 2); break;
            case '\\': pos = emit_mem(buf, cap, pos, "\\\\", 2); break;
            case '\n': pos = emit_mem(buf, cap, pos, "\\n", 2);  break;
            case '\r': pos = emit_mem(buf, cap, pos, "\\r", 2);  break;
            case '\t': pos = emit_mem(buf, cap, pos, "\\t", 2);  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                    pos = emit_mem(buf, cap, pos, esc, 6);
                } else {
                    pos = emit_char(buf, cap, pos, *p);
                }
                break;
            }
        }
    }
    pos = emit_char(buf, cap, pos, '"');
    pos = emit_char(buf, cap, pos, ',');
    return pos;
}

int json_emit_int(char *buf, size_t cap, int pos, long val)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%ld", val);
    pos = emit_mem(buf, cap, pos, tmp, (size_t)n);
    pos = emit_char(buf, cap, pos, ',');
    return pos;
}

int json_emit_dbl(char *buf, size_t cap, int pos, double val)
{
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%.2f", val);
    pos = emit_mem(buf, cap, pos, tmp, (size_t)n);
    pos = emit_char(buf, cap, pos, ',');
    return pos;
}

int json_emit_bool(char *buf, size_t cap, int pos, int val)
{
    if (val)
        pos = emit_mem(buf, cap, pos, "true,", 5);
    else
        pos = emit_mem(buf, cap, pos, "false,", 6);
    return pos;
}

int json_emit_null(char *buf, size_t cap, int pos)
{
    pos = emit_mem(buf, cap, pos, "null,", 5);
    return pos;
}

int json_emit_raw(char *buf, size_t cap, int pos, const char *raw, size_t len)
{
    return emit_mem(buf, cap, pos, raw, len);
}
