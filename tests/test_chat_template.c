/*
 * test_chat_template.c — verify that apply_chat_template returns -1
 * (truncation) instead of overflowing when messages exceed the buffer.
 *
 * The template functions are static in api.c, so they are mirrored here
 * (same pattern as test_json_escape.c) to test without linking the full
 * daemon.
 */
#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Mirrors of apply_chat_template_{llama3,bitnet} from src/api.c ── */

static int
apply_chat_template_llama3(const json_value_t *messages, char *buf, size_t cap)
{
    size_t pos = 0;
    int ret;

    ret = snprintf(buf + pos, cap - pos, "<|begin_of_text|>");
    if (ret < 0 || (size_t)ret >= cap - pos) return -1;
    pos += (size_t)ret;

    size_t n = json_array_len(messages);
    for (size_t i = 0; i < n; i++) {
        json_value_t *msg = json_array_get(messages, i);
        if (!msg) continue;

        const char *role    = json_get_str(msg, "role");
        const char *content = json_get_str(msg, "content");
        if (!role || !content) continue;

        ret = snprintf(buf + pos, cap - pos,
            "<|start_header_id|>%s<|end_header_id|>\n\n%s<|eot_id|>",
            role, content);
        if (ret < 0 || (size_t)ret >= cap - pos) return -1;
        pos += (size_t)ret;
    }

    ret = snprintf(buf + pos, cap - pos,
        "<|start_header_id|>assistant<|end_header_id|>\n\n");
    if (ret < 0 || (size_t)ret >= cap - pos) return -1;
    pos += (size_t)ret;

    return (int)pos;
}

static int
apply_chat_template_bitnet(const json_value_t *messages, char *buf, size_t cap)
{
    size_t pos = 0;
    int ret;

    size_t n = json_array_len(messages);
    for (size_t i = 0; i < n; i++) {
        json_value_t *msg = json_array_get(messages, i);
        if (!msg) continue;

        const char *role    = json_get_str(msg, "role");
        const char *content = json_get_str(msg, "content");
        if (!role || !content) continue;

        ret = snprintf(buf + pos, cap - pos,
            "%s: %s\n", role, content);
        if (ret < 0 || (size_t)ret >= cap - pos) return -1;
        pos += (size_t)ret;
    }

    ret = snprintf(buf + pos, cap - pos, "assistant:");
    if (ret < 0 || (size_t)ret >= cap - pos) return -1;
    pos += (size_t)ret;

    return (int)pos;
}

static int
apply_chat_template(const json_value_t *messages, const char *tmpl,
                    char *buf, size_t cap)
{
    if (strcmp(tmpl, "llama3") == 0)
        return apply_chat_template_llama3(messages, buf, cap);
    return apply_chat_template_bitnet(messages, buf, cap);
}

/* ── Helpers ─────────────────────────────────────────────────────── */

/*
 * Build a JSON string: {"messages":[{"role":"user","content":"AAA..."}]}
 * where content is `content_len` bytes of 'A'.
 */
static char *
build_big_messages_json(size_t content_len)
{
    /* {"messages":[{"role":"user","content":"..."}]} */
    size_t json_len = 50 + content_len;
    char *json = malloc(json_len);
    assert(json);

    size_t pos = 0;
    pos += (size_t)snprintf(json + pos, json_len - pos,
        "{\"messages\":[{\"role\":\"user\",\"content\":\"");
    memset(json + pos, 'A', content_len);
    pos += content_len;
    pos += (size_t)snprintf(json + pos, json_len - pos, "\"}]}");

    return json;
}

/* ── Tests ───────────────────────────────────────────────────────── */

static void
test_small_fits(void)
{
    const char *src = "{\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    json_value_t *root = json_parse(src, strlen(src));
    assert(root);
    json_value_t *msgs = json_get(root, "messages");
    assert(msgs);

    char buf[4096];
    int r;

    r = apply_chat_template(msgs, "llama3", buf, sizeof(buf));
    assert(r > 0);
    printf("  PASS: small message fits (llama3, %d bytes)\n", r);

    r = apply_chat_template(msgs, "bitnet", buf, sizeof(buf));
    assert(r > 0);
    printf("  PASS: small message fits (bitnet, %d bytes)\n", r);

    json_free(root);
}

static void
test_overflow_returns_neg1(const char *tmpl, size_t buf_cap)
{
    /* Content larger than buf_cap to guarantee overflow */
    size_t content_len = buf_cap + 1024;
    char *json = build_big_messages_json(content_len);

    json_value_t *root = json_parse(json, strlen(json));
    assert(root);
    json_value_t *msgs = json_get(root, "messages");
    assert(msgs);

    char *buf = malloc(buf_cap);
    assert(buf);

    int r = apply_chat_template(msgs, tmpl, buf, buf_cap);
    assert(r == -1);

    printf("  PASS: overflow returns -1 (tmpl=%s, buf=%zu, content=%zu)\n",
           tmpl, buf_cap, content_len);

    free(buf);
    json_free(root);
    free(json);
}

static void
test_128kb_overflow(const char *tmpl)
{
    /* Acceptance criteria: >128KB content must be rejected */
    size_t content_len = 131072 + 1024;  /* >128KB */
    size_t buf_cap = 131072;             /* API_BUF_SIZE */
    char *json = build_big_messages_json(content_len);

    json_value_t *root = json_parse(json, strlen(json));
    assert(root);
    json_value_t *msgs = json_get(root, "messages");
    assert(msgs);

    char *buf = malloc(buf_cap);
    assert(buf);

    int r = apply_chat_template(msgs, tmpl, buf, buf_cap);
    assert(r == -1);

    printf("  PASS: >128KB content returns -1 (tmpl=%s)\n", tmpl);

    free(buf);
    json_free(root);
    free(json);
}

int
main(void)
{
    printf("--- test_chat_template ---\n");

    test_small_fits();

    test_overflow_returns_neg1("llama3", 64);
    test_overflow_returns_neg1("bitnet", 64);
    test_overflow_returns_neg1("llama3", 4096);
    test_overflow_returns_neg1("bitnet", 4096);

    test_128kb_overflow("llama3");
    test_128kb_overflow("bitnet");

    printf("All chat_template tests PASSED.\n");
    return 0;
}
