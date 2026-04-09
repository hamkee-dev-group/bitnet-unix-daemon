#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

static void
test_parse_object(void)
{
    const char *src = "{\"name\":\"test\",\"value\":42,\"flag\":true}";
    json_value_t *v = json_parse(src, strlen(src));
    assert(v);
    assert(v->type == JSON_OBJECT);

    const char *name = json_get_str(v, "name");
    assert(name && strcmp(name, "test") == 0);

    assert(fabs(json_get_num(v, "value", 0) - 42.0) < 0.001);
    assert(json_get_bool_val(v, "flag", 0) == 1);

    json_free(v);
    printf("  PASS: parse object\n");
}

static void
test_parse_array(void)
{
    const char *src = "[1, 2, 3, \"hello\"]";
    json_value_t *v = json_parse(src, strlen(src));
    assert(v);
    assert(v->type == JSON_ARRAY);
    assert(json_array_len(v) == 4);

    json_value_t *e0 = json_array_get(v, 0);
    assert(e0 && e0->type == JSON_NUMBER && fabs(e0->u.number - 1.0) < 0.001);

    json_value_t *e3 = json_array_get(v, 3);
    assert(e3 && e3->type == JSON_STRING);
    assert(strcmp(e3->u.string.data, "hello") == 0);

    json_free(v);
    printf("  PASS: parse array\n");
}

static void
test_parse_nested(void)
{
    const char *src =
        "{\"messages\":[{\"role\":\"user\",\"content\":\"Hello!\"}],"
        "\"max_tokens\":256,\"stream\":false}";
    json_value_t *v = json_parse(src, strlen(src));
    assert(v);

    json_value_t *msgs = json_get(v, "messages");
    assert(msgs && msgs->type == JSON_ARRAY);
    assert(json_array_len(msgs) == 1);

    json_value_t *msg0 = json_array_get(msgs, 0);
    const char *role = json_get_str(msg0, "role");
    assert(role && strcmp(role, "user") == 0);

    const char *content = json_get_str(msg0, "content");
    assert(content && strcmp(content, "Hello!") == 0);

    assert(fabs(json_get_num(v, "max_tokens", 0) - 256.0) < 0.001);
    assert(json_get_bool_val(v, "stream", 1) == 0);

    json_free(v);
    printf("  PASS: parse nested\n");
}

static void
test_parse_escapes(void)
{
    const char *src = "{\"text\":\"hello\\nworld\\t!\"}";
    json_value_t *v = json_parse(src, strlen(src));
    assert(v);

    const char *text = json_get_str(v, "text");
    assert(text && strcmp(text, "hello\nworld\t!") == 0);

    json_free(v);
    printf("  PASS: parse escapes\n");
}

static void
test_parse_null(void)
{
    const char *src = "{\"val\":null}";
    json_value_t *v = json_parse(src, strlen(src));
    assert(v);

    json_value_t *val = json_get(v, "val");
    assert(val && val->type == JSON_NULL);

    json_free(v);
    printf("  PASS: parse null\n");
}

static void
test_emitter(void)
{
    char buf[256];
    int p = json_emit_start(buf, sizeof(buf));
    p = json_emit_obj_open(buf, sizeof(buf), p);
    p = json_emit_key(buf, sizeof(buf), p, "status");
    p = json_emit_str(buf, sizeof(buf), p, "ok");
    p = json_emit_key(buf, sizeof(buf), p, "count");
    p = json_emit_int(buf, sizeof(buf), p, 42);
    p = json_emit_obj_close(buf, sizeof(buf), p);
    assert(p > 0 && (size_t)p < sizeof(buf));
    buf[p] = '\0';

    json_value_t *v = json_parse(buf, (size_t)p);
    assert(v);
    assert(strcmp(json_get_str(v, "status"), "ok") == 0);
    assert(fabs(json_get_num(v, "count", 0) - 42.0) < 0.001);

    json_free(v);
    printf("  PASS: emitter\n");
}

static void
test_parse_invalid(void)
{
    assert(json_parse(NULL, 0) == NULL);
    assert(json_parse("", 0) == NULL);
    assert(json_parse("{bad}", 5) == NULL);
    assert(json_parse("{\"key\"}", 7) == NULL);
    printf("  PASS: parse invalid\n");
}

int
main(void)
{
    printf("test_json:\n");
    test_parse_object();
    test_parse_array();
    test_parse_nested();
    test_parse_escapes();
    test_parse_null();
    test_emitter();
    test_parse_invalid();
    printf("All JSON tests passed.\n");
    return 0;
}
