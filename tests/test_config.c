#include "../src/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static void
write_test_conf(const char *path)
{
    FILE *fp = fopen(path, "w");
    assert(fp);
    fprintf(fp,
        "# test config\n"
        "[daemon]\n"
        "pidfile = /tmp/test.pid\n"
        "loglevel = debug\n"
        "\n"
        "[model]\n"
        "path = /tmp/model.gguf\n"
        "threads = 8\n"
        "ctx_size = 2048\n"
        "\n"
        "[server]\n"
        "listen = 0.0.0.0:9090\n"
        "max_connections = 32\n"
        "\n"
        "[metrics]\n"
        "enabled = true\n"
    );
    fclose(fp);
}

static void
test_basic_parsing(void)
{
    const char *path = "/tmp/test_bitnetd.conf";
    write_test_conf(path);

    config_t *c = cfg_create();
    assert(c);
    assert(cfg_load(c, path) == 0);

    const char *pidfile = cfg_get_str(c, "daemon", "pidfile");
    assert(pidfile && strcmp(pidfile, "/tmp/test.pid") == 0);

    const char *model = cfg_get_str(c, "model", "path");
    assert(model && strcmp(model, "/tmp/model.gguf") == 0);

    assert(cfg_get_int(c, "model", "threads", 0) == 8);
    assert(cfg_get_int(c, "model", "ctx_size", 0) == 2048);
    assert(cfg_get_int(c, "server", "max_connections", 0) == 32);

    assert(cfg_get_int(c, "model", "nonexistent", 42) == 42);

    assert(cfg_get_bool(c, "metrics", "enabled", 0) == 1);

    assert(cfg_get_bool(c, "metrics", "missing", 0) == 0);

    assert(cfg_get_str(c, "nosection", "nokey") == NULL);

    cfg_free(c);
    unlink(path);
    printf("  PASS: basic parsing\n");
}

static void
test_cfg_set(void)
{
    config_t *c = cfg_create();
    assert(c);

    assert(cfg_set(c, "test", "key", "value") == 0);
    const char *v = cfg_get_str(c, "test", "key");
    assert(v && strcmp(v, "value") == 0);

    assert(cfg_set(c, "test", "key", "new") == 0);
    v = cfg_get_str(c, "test", "key");
    assert(v && strcmp(v, "new") == 0);

    cfg_free(c);
    printf("  PASS: cfg_set\n");
}

static void
test_bad_file(void)
{
    config_t *c = cfg_create();
    assert(c);
    assert(cfg_load(c, "/nonexistent/path.conf") == -1);
    cfg_free(c);
    printf("  PASS: bad file\n");
}

int
main(void)
{
    printf("test_config:\n");
    test_basic_parsing();
    test_cfg_set();
    test_bad_file();
    printf("All config tests passed.\n");
    return 0;
}
