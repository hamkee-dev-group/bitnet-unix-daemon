PREFIX  ?= /usr/local
DESTDIR ?=

CC      ?= cc
CFLAGS  += -std=c11 -Wall -Wextra -Werror -pedantic
CFLAGS  += -D_POSIX_C_SOURCE=200809L
CFLAGS  += -Isrc -Iinclude
LDFLAGS +=

# Platform detection
OS := $(shell uname -s)
ifeq ($(OS),Linux)
  POLLER ?= epoll
else ifeq ($(OS),OpenBSD)
  POLLER ?= kqueue
else ifeq ($(OS),FreeBSD)
  POLLER ?= kqueue
else ifeq ($(OS),Darwin)
  POLLER ?= kqueue
else
  POLLER ?= poll
endif

# Override: make POLLER=poll
ifeq ($(POLLER),poll)
  CFLAGS += -DUSE_POLL_BACKEND
endif

# ── Backend selection ──────────────────────────────────────────────
#   make BACKEND=subprocess   (default) — wraps llama-server process
#   make BACKEND=native       — links bitnet-c11 for in-process inference
BACKEND ?= subprocess

# Path to the bitnet-c11 project (must be set by the user for native backend)
# Example: make BACKEND=native BITNET_C11_DIR=/path/to/bitnet-c11
BITNET_C11_DIR ?=

ifeq ($(BACKEND),native)
  ifeq ($(BITNET_C11_DIR),)
    $(error BITNET_C11_DIR must be set for native backend: make BACKEND=native BITNET_C11_DIR=/path/to/bitnet-c11)
  endif
  CFLAGS      += -I$(BITNET_C11_DIR)/include
  BACKEND_SRC  = src/backend_native.c
  LDFLAGS     += -lm -lpthread

  # SIMD selection for bitnet-c11 (must match how bitnet-c11 was built)
  SIMD ?= avx2
  ifeq ($(SIMD),avx2)
    BITNET_MATMUL = $(BITNET_C11_DIR)/src/bitnet_matmul_avx2.o \
                    $(BITNET_C11_DIR)/src/bitnet_matmul_scalar.o
  else
    BITNET_MATMUL = $(BITNET_C11_DIR)/src/bitnet_matmul_scalar.o
  endif

  BITNET_OBJS = $(BITNET_C11_DIR)/src/bitnet_gguf.o     \
                $(BITNET_C11_DIR)/src/bitnet_arena.o     \
                $(BITNET_C11_DIR)/src/bitnet_quant.o     \
                $(BITNET_MATMUL)                         \
                $(BITNET_C11_DIR)/src/bitnet_sampler.o   \
                $(BITNET_C11_DIR)/src/bitnet_tokenizer.o \
                $(BITNET_C11_DIR)/src/bitnet_core.o
else
  BACKEND_SRC = src/backend.c
  BITNET_OBJS =
endif

# ── Source files ───────────────────────────────────────────────────
SRCS = src/bitnetd.c \
       src/config.c  \
       src/log.c     \
       src/json.c    \
       src/http.c    \
       src/api.c     \
       src/metrics.c \
       $(BACKEND_SRC)

# Poller source (only one compiled based on platform)
ifeq ($(POLLER),epoll)
  SRCS += src/poller_epoll.c
else ifeq ($(POLLER),kqueue)
  SRCS += src/poller_kqueue.c
else
  SRCS += src/poller_poll.c
endif

OBJS = $(SRCS:.c=.o)

# Control tool
CTL_SRCS = tools/bitnetctl.c
CTL_OBJS = $(CTL_SRCS:.c=.o)

# Test sources
TEST_CONFIG_SRCS = tests/test_config.c src/config.c
TEST_JSON_SRCS   = tests/test_json.c src/json.c

# ── Targets ────────────────────────────────────────────────────────
all: bitnetd bitnetctl

bitnetd: $(OBJS) $(BITNET_OBJS)
	$(CC) -o $@ $(OBJS) $(BITNET_OBJS) $(LDFLAGS)

bitnetctl: $(CTL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CTL_OBJS)

# Special rule for poller_epoll.c: needs _GNU_SOURCE
src/poller_epoll.o: src/poller_epoll.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Build bitnet-c11 dependency (convenience target) ──────────────
bitnet-c11:
	$(MAKE) -C $(BITNET_C11_DIR)

# ── Tests ──────────────────────────────────────────────────────────
test: test_config test_json
	@echo "--- test_config ---"
	./test_config
	@echo "--- test_json ---"
	./test_json
	@echo "All tests passed."

test_config: $(TEST_CONFIG_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_CONFIG_SRCS)

test_json: $(TEST_JSON_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_JSON_SRCS)

# Native backend smoke test — skips cleanly when BITNET_C11_DIR or
# BITNET_MODEL are not set.  Override NATIVE_SMOKE_TIMEOUT for slow
# machines.
test_native_smoke:
	@sh tests/test_native_smoke.sh

# ── Install ────────────────────────────────────────────────────────
install: bitnetd bitnetctl
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/share/man/man8
	install -d $(DESTDIR)$(PREFIX)/share/man/man5
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 755 bitnetd    $(DESTDIR)$(PREFIX)/sbin/bitnetd
	install -m 755 bitnetctl  $(DESTDIR)$(PREFIX)/bin/bitnetctl
	install -m 644 man/bitnetd.8     $(DESTDIR)$(PREFIX)/share/man/man8/
	install -m 644 man/bitnetd.conf.5 $(DESTDIR)$(PREFIX)/share/man/man5/
	install -m 644 man/bitnetctl.1   $(DESTDIR)$(PREFIX)/share/man/man1/

clean:
	rm -f bitnetd bitnetctl test_config test_json
	rm -f src/*.o tools/*.o tests/*.o

# Portability check: ensure only poller_epoll.c includes sys/epoll.h
portcheck:
	@! grep -rn 'sys/epoll\|sys/sendfile\|linux/' src/ \
		--include='*.c' --include='*.h' \
		| grep -v 'poller_epoll.c' \
		&& echo "PASS: no Linux-only APIs outside poller_epoll.c" \
		|| echo "FAIL: Linux-only APIs found outside poller_epoll.c"

.PHONY: all clean test test_native_smoke install portcheck bitnet-c11
