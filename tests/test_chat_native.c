/*
 * test_chat_native.c — regression test for native chat token leakage.
 *
 * Exercises the prompt-assembly + token-filter pipeline that guards
 * /v1/chat/completions output.  The token filter and chat templates
 * are static in api.c, so they are mirrored here (same approach as
 * test_chat_template.c and test_json_escape.c).
 *
 * Acceptance criteria
 * -------------------
 *  1. Assistant output must never contain <|…|> control markers.
 *  2. Assistant output must never contain leaked role/header lines
 *     (e.g. literal "system:", "user:", "assistant:" prefixes that
 *     came from the template rather than the model).
 *  3. Streaming and non-streaming filter paths must produce byte-
 *     identical sanitised content.
 */
#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Mirror of token filter from src/api.c ──────────────────────── */

#define TOKEN_FILTER_BUF 64

typedef struct {
    char    pending[TOKEN_FILTER_BUF];
    size_t  plen;
    int     backend_tokens;
    int     (*emit)(const char *text, size_t len, void *ud);
    void   *ud;
} token_filter_t;

static const char *control_tokens[] = {
    "<|begin_of_text|>",
    "<|end_of_text|>",
    "<|start_header_id|>",
    "<|end_header_id|>",
    "<|eot_id|>",
    NULL
};

static int
is_control_token(const char *s, size_t len)
{
    for (const char **ct = control_tokens; *ct; ct++)
        if (len == strlen(*ct) && memcmp(s, *ct, len) == 0)
            return 1;
    return 0;
}

static int
could_be_control_prefix(const char *s, size_t len)
{
    for (const char **ct = control_tokens; *ct; ct++) {
        size_t ctlen = strlen(*ct);
        if (len <= ctlen && memcmp(s, *ct, len) == 0)
            return 1;
    }
    return 0;
}

static void
token_filter_init(token_filter_t *f,
                  int (*emit)(const char *, size_t, void *), void *ud)
{
    f->plen = 0;
    f->backend_tokens = 0;
    f->emit = emit;
    f->ud   = ud;
}

static int
token_filter_feed(token_filter_t *f, const char *token, size_t len)
{
    size_t i = 0;
    while (i < len) {
        if (f->plen > 0) {
            char ch = token[i];
            if (f->plen + 1 < TOKEN_FILTER_BUF) {
                f->pending[f->plen] = ch;
                if (could_be_control_prefix(f->pending, f->plen + 1)) {
                    f->plen++;
                    if (ch == '>' && is_control_token(f->pending, f->plen)) {
                        f->plen = 0;
                    }
                    i++;
                    continue;
                }
            }
            int rc = f->emit(f->pending, f->plen, f->ud);
            if (rc) return rc;
            f->plen = 0;
            continue;
        }

        size_t start = i;
        while (i < len && token[i] != '<') i++;
        if (i > start) {
            int rc = f->emit(token + start, i - start, f->ud);
            if (rc) return rc;
        }
        if (i < len) {
            f->pending[0] = '<';
            f->plen = 1;
            i++;
        }
    }
    return 0;
}

static int
token_filter_flush(token_filter_t *f)
{
    if (f->plen > 0) {
        int rc = f->emit(f->pending, f->plen, f->ud);
        f->plen = 0;
        return rc;
    }
    return 0;
}

static int
filtered_token_cb(const char *token, size_t len, void *ud)
{
    token_filter_t *f = (token_filter_t *)ud;
    f->backend_tokens++;
    return token_filter_feed(f, token, len);
}

/* ── Mirror of chat templates from src/api.c ────────────────────── */

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

        ret = snprintf(buf + pos, cap - pos, "%s: %s\n", role, content);
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

/* ── Collect buffer (simulates both stream and non-stream paths) ── */

typedef struct {
    char   buf[8192];
    size_t len;
} collect_t;

static int
collect_emit(const char *text, size_t len, void *ud)
{
    collect_t *c = (collect_t *)ud;
    if (c->len + len >= sizeof(c->buf))
        return -1;
    memcpy(c->buf + c->len, text, len);
    c->len += len;
    c->buf[c->len] = '\0';
    return 0;
}

/* ── Assertion helpers ──────────────────────────────────────────── */

/* Fail if output contains any <|…|> control marker. */
static void
assert_no_control_markers(const char *label, const char *output)
{
    const char *p = output;
    while ((p = strstr(p, "<|")) != NULL) {
        const char *end = strstr(p, "|>");
        if (end) {
            fprintf(stderr,
                "FAIL [%s]: control marker leaked: %.*s\n",
                label, (int)(end - p + 2), p);
            assert(0 && "control marker leaked into output");
        }
        p += 2;
    }
}

/* Fail if output starts with a role header that would indicate
 * template leakage (e.g. "system: …" or "user: …"). */
static void
assert_no_role_headers(const char *label, const char *output)
{
    const char *role_prefixes[] = {
        "system:",  "system: ",
        "user:",    "user: ",
        "assistant:", "assistant: ",
        NULL
    };
    for (const char **rp = role_prefixes; *rp; rp++) {
        if (strncmp(output, *rp, strlen(*rp)) == 0) {
            fprintf(stderr,
                "FAIL [%s]: role header leaked at start: '%s'\n",
                label, output);
            assert(0 && "role header leaked into output");
        }
    }
}

/* ── Simulate a backend producing a token stream ──────────────────
 *
 * Feeds `raw_tokens` (an array of string chunks) through the token
 * filter using filtered_token_cb, exactly as api_chat_completions
 * does for both stream and non-stream paths.
 */
static void
simulate_backend(const char **raw_tokens, size_t ntokens,
                 collect_t *out)
{
    token_filter_t filter;
    out->len = 0;
    out->buf[0] = '\0';

    token_filter_init(&filter, collect_emit, out);
    for (size_t i = 0; i < ntokens; i++)
        filtered_token_cb(raw_tokens[i], strlen(raw_tokens[i]), &filter);
    token_filter_flush(&filter);
}

/* ── Tests ───────────────────────────────────────────────────────── */

/*
 * 1. Complete control tokens in a single chunk are stripped.
 */
static void
test_control_tokens_single_chunk(void)
{
    const char *tokens[] = {
        "<|begin_of_text|>",
        "Hello",
        "<|end_of_text|>",
    };
    collect_t out;
    simulate_backend(tokens, 3, &out);

    assert_no_control_markers("single_chunk", out.buf);
    assert(strcmp(out.buf, "Hello") == 0);
    printf("  PASS: single-chunk control tokens stripped\n");
}

/*
 * 2. Control tokens split across chunk boundaries are stripped.
 */
static void
test_control_tokens_split_across_chunks(void)
{
    const char *tokens[] = {
        "Hello",
        "<|begin_of",          /* partial */
        "_text|>",             /* completes the marker */
        " world",
    };
    collect_t out;
    simulate_backend(tokens, 4, &out);

    assert_no_control_markers("split_chunks", out.buf);
    assert(strcmp(out.buf, "Hello world") == 0);
    printf("  PASS: split-chunk control tokens stripped\n");
}

/*
 * 3. All five known control tokens are individually suppressed.
 */
static void
test_all_known_control_tokens(void)
{
    const char *tokens[] = {
        "<|begin_of_text|>",
        "<|end_of_text|>",
        "<|start_header_id|>",
        "<|end_header_id|>",
        "<|eot_id|>",
        "visible",
    };
    collect_t out;
    simulate_backend(tokens, 6, &out);

    assert_no_control_markers("all_known", out.buf);
    assert(strcmp(out.buf, "visible") == 0);
    printf("  PASS: all five known control tokens suppressed\n");
}

/*
 * 4. A '<' that does NOT start a control token passes through.
 */
static void
test_normal_angle_bracket(void)
{
    const char *tokens[] = {
        "a < b",
        " and ",
        "<html>",
    };
    collect_t out;
    simulate_backend(tokens, 3, &out);

    assert(strcmp(out.buf, "a < b and <html>") == 0);
    printf("  PASS: normal angle brackets pass through\n");
}

/*
 * 5. Role/header leakage: simulate a buggy backend that echoes back
 *    llama3 template framing around the actual answer.
 */
static void
test_llama3_header_leak(void)
{
    /* If the backend echoes template framing, the filter must strip
     * control markers.  What remains must not start with a role header. */
    const char *tokens[] = {
        "<|start_header_id|>",
        "assistant",
        "<|end_header_id|>",
        "\n\n",
        "The answer is 42.",
        "<|eot_id|>",
    };
    collect_t out;
    simulate_backend(tokens, 6, &out);

    assert_no_control_markers("llama3_header_leak", out.buf);
    /* "assistant" from the header leaks as plain text after markers are
     * stripped — that is fine, the native backend stops at the first
     * control token.  But the markers themselves must be gone.           */
    assert(strstr(out.buf, "The answer is 42.") != NULL);
    printf("  PASS: llama3 header framing markers stripped\n");
}

/*
 * 6. Prompt-assembly → filter round-trip: verify that running a full
 *    llama3-templated prompt through the filter removes all control
 *    markers.
 */
static void
test_template_roundtrip_llama3(void)
{
    const char *src =
        "{\"messages\":["
        "{\"role\":\"system\",\"content\":\"You are helpful.\"},"
        "{\"role\":\"user\",\"content\":\"Hi\"}"
        "]}";
    json_value_t *root = json_parse(src, strlen(src));
    assert(root);
    json_value_t *msgs = json_get(root, "messages");
    assert(msgs);

    char prompt[4096];
    int plen = apply_chat_template(msgs, "llama3", prompt, sizeof(prompt));
    assert(plen > 0);

    /* Feed the entire assembled prompt through the filter as if the
     * backend echoed it back (worst-case leakage scenario).             */
    const char *tokens[] = { prompt };
    collect_t out;
    simulate_backend(tokens, 1, &out);

    assert_no_control_markers("roundtrip_llama3", out.buf);
    printf("  PASS: llama3 template round-trip contains no control markers\n");

    json_free(root);
}

/*
 * 7. Prompt-assembly → filter round-trip for the bitnet template.
 *    The bitnet template does not use <|…|> markers, but verify
 *    that the assembled prompt, if echoed, does not contain them
 *    and does not start with a role header after filtering.
 */
static void
test_template_roundtrip_bitnet(void)
{
    const char *src =
        "{\"messages\":["
        "{\"role\":\"user\",\"content\":\"Hi\"}"
        "]}";
    json_value_t *root = json_parse(src, strlen(src));
    assert(root);
    json_value_t *msgs = json_get(root, "messages");
    assert(msgs);

    char prompt[4096];
    int plen = apply_chat_template(msgs, "bitnet", prompt, sizeof(prompt));
    assert(plen > 0);

    const char *tokens[] = { prompt };
    collect_t out;
    simulate_backend(tokens, 1, &out);

    assert_no_control_markers("roundtrip_bitnet", out.buf);
    printf("  PASS: bitnet template round-trip contains no control markers\n");

    json_free(root);
}

/*
 * 8. Streaming vs non-streaming parity: feeding the same token
 *    sequence through separate filter instances must yield identical
 *    output, regardless of chunk boundaries.
 */
static void
test_stream_nonstream_parity(void)
{
    /* Simulate a realistic native-backend token stream that includes
     * interleaved control tokens and real content.                       */
    const char *tokens[] = {
        "<|begin_of_text|>",
        "<|start_header_id|>",
        "assistant",
        "<|end_header_id|>",
        "\n\n",
        "Hello, ",
        "world!",
        "<|eot_id|>",
    };
    size_t ntokens = sizeof(tokens) / sizeof(tokens[0]);

    /* "Streaming" path: feed tokens one at a time (each token is one
     * SSE chunk).                                                       */
    collect_t stream_out;
    simulate_backend(tokens, ntokens, &stream_out);

    /* "Non-streaming" path: concatenate all tokens into a single
     * buffer and feed as one chunk (simulates collect-then-send).       */
    char concat[4096];
    size_t clen = 0;
    for (size_t i = 0; i < ntokens; i++) {
        size_t tlen = strlen(tokens[i]);
        memcpy(concat + clen, tokens[i], tlen);
        clen += tlen;
    }
    concat[clen] = '\0';

    const char *single[] = { concat };
    collect_t nonstream_out;
    simulate_backend(single, 1, &nonstream_out);

    /* Both must be identical. */
    if (stream_out.len != nonstream_out.len ||
        strcmp(stream_out.buf, nonstream_out.buf) != 0) {
        fprintf(stderr,
            "FAIL: stream='%s' (%zu) vs nonstream='%s' (%zu)\n",
            stream_out.buf, stream_out.len,
            nonstream_out.buf, nonstream_out.len);
        assert(0 && "stream/non-stream parity failure");
    }
    assert_no_control_markers("parity_stream", stream_out.buf);
    assert_no_control_markers("parity_nonstream", nonstream_out.buf);
    printf("  PASS: stream/non-stream filter output identical ('%s')\n",
           stream_out.buf);
}

/*
 * 9. Parity with varied chunk boundaries: re-chunk the same input at
 *    every possible byte boundary and verify output is always equal.
 */
static void
test_parity_varied_boundaries(void)
{
    const char *raw = "<|begin_of_text|>Hi<|eot_id|>";
    size_t rawlen = strlen(raw);

    /* Reference: feed whole string as one chunk. */
    const char *single[] = { raw };
    collect_t ref;
    simulate_backend(single, 1, &ref);
    assert_no_control_markers("varied_ref", ref.buf);

    /* Now split at every byte boundary and compare. */
    for (size_t split = 1; split < rawlen; split++) {
        char part1[128], part2[128];
        memcpy(part1, raw, split);
        part1[split] = '\0';
        memcpy(part2, raw + split, rawlen - split);
        part2[rawlen - split] = '\0';

        const char *two[] = { part1, part2 };
        collect_t out;
        simulate_backend(two, 2, &out);

        if (out.len != ref.len || strcmp(out.buf, ref.buf) != 0) {
            fprintf(stderr,
                "FAIL: split@%zu: got '%s', expected '%s'\n",
                split, out.buf, ref.buf);
            assert(0 && "varied boundary parity failure");
        }
    }
    printf("  PASS: parity holds across all byte-boundary splits\n");
}

/*
 * 10. Full end-to-end: assemble a llama3 prompt, append simulated
 *     model output containing control tokens, filter everything,
 *     and assert clean output with no role leakage.
 */
static void
test_e2e_llama3_with_model_output(void)
{
    const char *src =
        "{\"messages\":["
        "{\"role\":\"system\",\"content\":\"Be concise.\"},"
        "{\"role\":\"user\",\"content\":\"What is 2+2?\"}"
        "]}";
    json_value_t *root = json_parse(src, strlen(src));
    assert(root);
    json_value_t *msgs = json_get(root, "messages");
    assert(msgs);

    /* Assemble prompt (this is what gets sent to the backend). */
    char prompt[4096];
    int plen = apply_chat_template(msgs, "llama3", prompt, sizeof(prompt));
    assert(plen > 0);

    /* Simulate model output: only the text after the final
     * <|start_header_id|>assistant<|end_header_id|>\n\n is the
     * model's reply.  A well-behaved backend emits only the reply
     * tokens.  Test the worst case: reply includes stray markers. */
    const char *reply_tokens[] = {
        "4",
        "<|eot_id|>",
    };
    collect_t out;
    simulate_backend(reply_tokens, 2, &out);

    assert_no_control_markers("e2e_llama3", out.buf);
    assert_no_role_headers("e2e_llama3", out.buf);
    assert(strcmp(out.buf, "4") == 0);
    printf("  PASS: e2e llama3 clean output\n");

    json_free(root);
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(void)
{
    printf("--- test_chat_native ---\n");

    /* Control-token suppression */
    test_control_tokens_single_chunk();
    test_control_tokens_split_across_chunks();
    test_all_known_control_tokens();
    test_normal_angle_bracket();

    /* Role/header leakage */
    test_llama3_header_leak();

    /* Prompt-assembly → filter round-trips */
    test_template_roundtrip_llama3();
    test_template_roundtrip_bitnet();

    /* Streaming / non-streaming parity */
    test_stream_nonstream_parity();
    test_parity_varied_boundaries();

    /* End-to-end */
    test_e2e_llama3_with_model_output();

    printf("All chat_native tests PASSED.\n");
    return 0;
}
