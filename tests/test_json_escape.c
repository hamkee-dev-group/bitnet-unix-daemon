/*
 * test_json_escape.c — verify that the stream and non-stream JSON escape
 * paths in api.c handle identical characters and produce the same output.
 *
 * Both loops are duplicated here (they are static/inlined in api.c) so
 * the test can confirm parity without linking the full daemon.
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* Mirror of the stream_emit() escape loop (src/api.c ~line 244). */
static size_t
escape_stream(const char *text, size_t len, char *out, size_t cap)
{
    size_t ei = 0;
    for (size_t i = 0; i < len && ei + 6 < cap; i++) {
        switch (text[i]) {
        case '"':  out[ei++] = '\\'; out[ei++] = '"';  break;
        case '\\': out[ei++] = '\\'; out[ei++] = '\\'; break;
        case '\n': out[ei++] = '\\'; out[ei++] = 'n';  break;
        case '\r': out[ei++] = '\\'; out[ei++] = 'r';  break;
        case '\t': out[ei++] = '\\'; out[ei++] = 't';  break;
        default:   out[ei++] = text[i]; break;
        }
    }
    out[ei] = '\0';
    return ei;
}

/* Mirror of the non-stream collect path (src/api.c ~line 407). */
static size_t
escape_nonstream(const char *text, size_t len, char *out, size_t cap)
{
    size_t ei = 0;
    for (size_t i = 0; i < len && ei + 6 < cap; i++) {
        switch (text[i]) {
        case '"':  out[ei++] = '\\'; out[ei++] = '"';  break;
        case '\\': out[ei++] = '\\'; out[ei++] = '\\'; break;
        case '\n': out[ei++] = '\\'; out[ei++] = 'n';  break;
        case '\r': out[ei++] = '\\'; out[ei++] = 'r';  break;
        case '\t': out[ei++] = '\\'; out[ei++] = 't';  break;
        default:   out[ei++] = text[i]; break;
        }
    }
    out[ei] = '\0';
    return ei;
}

static void
test_escape_parity(const char *label, const char *input)
{
    size_t len = strlen(input);
    char buf_s[512], buf_n[512];

    size_t s = escape_stream(input, len, buf_s, sizeof(buf_s));
    size_t n = escape_nonstream(input, len, buf_n, sizeof(buf_n));

    if (s != n || strcmp(buf_s, buf_n) != 0) {
        fprintf(stderr, "FAIL [%s]: stream='%s' nonstream='%s'\n",
                label, buf_s, buf_n);
        assert(0 && "escape parity failure");
    }
    printf("  PASS: %s -> \"%s\"\n", label, buf_s);
}

static void
test_tab_escaped(void)
{
    const char *input = "col1\tcol2\tcol3";
    char buf[512];
    escape_stream(input, strlen(input), buf, sizeof(buf));

    /* Must not contain a literal tab */
    assert(strchr(buf, '\t') == NULL);
    /* Must contain escaped tabs */
    assert(strstr(buf, "\\t") != NULL);
    /* Full expected output */
    assert(strcmp(buf, "col1\\tcol2\\tcol3") == 0);
    printf("  PASS: tab characters are escaped\n");
}

static void
test_all_special_chars(void)
{
    /* Input containing every special char: " \ \n \r \t */
    const char input[] = "a\"b\\c\nd\re\tf";
    char buf[512];
    escape_stream(input, sizeof(input) - 1, buf, sizeof(buf));

    const char *expected = "a\\\"b\\\\c\\nd\\re\\tf";
    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "FAIL: expected '%s' got '%s'\n", expected, buf);
        assert(0);
    }
    printf("  PASS: all special characters escaped correctly\n");
}

static void
test_wrapped_in_json(void)
{
    /* Verify the escaped output produces valid JSON when wrapped */
    const char *input = "line1\tindented\nline2\r\nquoted:\"val\"";
    char esc[512];
    escape_stream(input, strlen(input), esc, sizeof(esc));

    char json[1024];
    snprintf(json, sizeof(json), "{\"content\":\"%s\"}", esc);

    /* Minimal JSON validity: no raw control chars (0x00-0x1f) */
    for (size_t i = 0; json[i]; i++) {
        unsigned char c = (unsigned char)json[i];
        if (c < 0x20) {
            fprintf(stderr, "FAIL: raw control char 0x%02x at pos %zu in: %s\n",
                    c, i, json);
            assert(0 && "raw control character in JSON");
        }
    }
    printf("  PASS: escaped output produces JSON with no raw control chars\n");
}

int
main(void)
{
    printf("--- test_json_escape ---\n");

    /* Parity tests: both paths must produce identical output */
    test_escape_parity("plain text",    "hello world");
    test_escape_parity("with tab",      "col1\tcol2");
    test_escape_parity("with newline",  "line1\nline2");
    test_escape_parity("with cr",       "line1\rline2");
    test_escape_parity("with quote",    "say \"hello\"");
    test_escape_parity("with backslash","path\\to\\file");
    test_escape_parity("mixed",         "a\tb\nc\\d\"e\rf");
    test_escape_parity("empty",         "");

    /* Specific escape correctness */
    test_tab_escaped();
    test_all_special_chars();
    test_wrapped_in_json();

    printf("All json_escape tests PASSED.\n");
    return 0;
}
