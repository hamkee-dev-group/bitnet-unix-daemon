# bitnetd

Production-grade Unix daemon for [BitNet](https://github.com/microsoft/BitNet)
1-bit LLM inference. Exposes an **OpenAI-compatible REST API** over HTTP.

Written in C11 with zero external dependencies beyond libc. Runs on Linux,
OpenBSD, FreeBSD, and macOS.

Published by [Hamkee](https://hamkee.net) under the MIT license.

## Features

- **Proper Unix daemon** &mdash; double-fork, setsid, PID file with flock,
  syslog, privilege dropping, signal handling
- **OpenAI-compatible API** &mdash; `/v1/chat/completions` (streaming & non-streaming),
  `/v1/completions`, `/v1/models`
- **Prometheus metrics** &mdash; `/metrics` endpoint with request counts,
  token throughput, latency histograms
- **Portable I/O** &mdash; epoll (Linux), kqueue (BSD/macOS), poll (fallback)
- **OpenBSD hardened** &mdash; `pledge()` / `unveil()` support
- **Two backends** &mdash; subprocess (wraps `llama-server`) or native
  (links [bitnet-c11](https://github.com/nicholascapo/bitnet-c11) directly)

## Quick Start

```bash
# Build
make

# Create a config file
cp conf/bitnetd.conf.example bitnetd.conf
# Edit bitnetd.conf — set [model] path to your GGUF model file
# and optionally [backend] server_path to your llama-server binary

# Run in foreground (for testing)
./bitnetd -f -c bitnetd.conf

# Test
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/v1/models
curl -X POST http://127.0.0.1:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"bitnet","messages":[{"role":"user","content":"Hello!"}],"max_tokens":64}'
```

## Building

Requirements: a C11 compiler (gcc, clang), make, POSIX system.

```bash
# Standard build (subprocess backend, platform-native poller)
make

# Force portable poll() backend
make POLLER=poll

# Native backend (requires bitnet-c11 built separately)
make BACKEND=native BITNET_C11_DIR=/path/to/bitnet-c11

# Install to /usr/local
sudo make install

# Run tests
make test

# Native backend smoke test (requires model file; skips cleanly if missing)
BITNET_C11_DIR=../bitnet-c11 \
BITNET_MODEL=../bitnet-c11/models/BitNet-b1.58-2B-4T/ggml-model-i2_s.gguf \
  make test-native
```

### Build options

| Variable | Values | Default | Description |
|---|---|---|---|
| `BACKEND` | `subprocess`, `native` | `subprocess` | Inference backend |
| `BITNET_C11_DIR` | path | *(required for native)* | Path to bitnet-c11 source tree |
| `POLLER` | `epoll`, `kqueue`, `poll` | auto-detected | I/O multiplexing backend |
| `SIMD` | `avx2`, `scalar` | `avx2` | SIMD for native backend matmul |
| `PREFIX` | path | `/usr/local` | Install prefix |
| `CC` | compiler | `cc` | C compiler |

### Backends

**Subprocess** (default) &mdash; Spawns `llama-server` as a child process and
proxies inference requests to it over localhost HTTP. Works immediately with any
existing BitNet/llama.cpp installation. Configure the path to `llama-server` via
`[backend] server_path` in the config file, or ensure it is in your `$PATH`.

**Native** &mdash; Links bitnet-c11 for direct in-process inference with no
subprocess overhead. Requires building
[bitnet-c11](https://github.com/nicholascapo/bitnet-c11) first, then passing
its path:

```bash
make BACKEND=native BITNET_C11_DIR=/path/to/bitnet-c11
```

To verify the native build end-to-end, run the smoke test. It builds
bitnetd, starts it with a temporary config, checks `/health`,
`/v1/models`, and `/v1/chat/completions`, then tears down:

```bash
BITNET_C11_DIR=../bitnet-c11 \
BITNET_MODEL=../bitnet-c11/models/BitNet-b1.58-2B-4T/ggml-model-i2_s.gguf \
  make test-native
```

If either `BITNET_C11_DIR` or `BITNET_MODEL` is unset (or the paths
don't exist), the test prints `SKIP` and exits 0 — safe to include in
CI without a model present. Override `NATIVE_SMOKE_TIMEOUT` (default 30s)
for slow machines.

## Configuration

See [`conf/bitnetd.conf.example`](conf/bitnetd.conf.example) for all options
and [`bitnetd.conf(5)`](man/bitnetd.conf.5) for the man page.

Key settings:

```ini
[model]
path = /path/to/ggml-model-i2_s.gguf   # Required: GGUF model file
threads = 4                              # Inference threads
ctx_size = 2048                          # Context window

[server]
listen = 127.0.0.1:8080                 # TCP listen address
# unix_socket = /var/run/bitnetd.sock   # Optional Unix socket

[backend]
# server_path = /usr/local/bin/llama-server  # For subprocess backend
port = 18088                             # Internal llama-server port

[daemon]
pidfile = /var/run/bitnetd.pid
logfile = /var/log/bitnetd.log
loglevel = info                          # debug, info, warn, error
# user = _bitnetd                        # Drop privileges to this user
```

## Usage

```
bitnetd [-c config] [-f] [-t] [-v]

  -c FILE   Configuration file (default: /etc/bitnetd.conf)
  -f        Run in foreground (don't daemonize, log to stderr)
  -t        Test configuration and exit
  -v        Print version and exit
```

### Signals

| Signal | Action |
|---|---|
| `SIGHUP` | Reload configuration |
| `SIGTERM` | Graceful shutdown |
| `SIGINT` | Graceful shutdown |
| `SIGUSR1` | Reopen log files (for logrotate) |
| `SIGUSR2` | Dump status to log |

### Control tool

```bash
bitnetctl status           # Health check
bitnetctl models           # List models
bitnetctl metrics          # Prometheus metrics
bitnetctl reload           # Send SIGHUP
bitnetctl stop             # Send SIGTERM

# Options: -H host  -p port  -P pidfile
```

## API Endpoints

### POST /v1/chat/completions

OpenAI-compatible chat completion.

```bash
curl -X POST http://127.0.0.1:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "bitnet",
    "messages": [
      {"role": "system", "content": "You are a helpful assistant."},
      {"role": "user", "content": "What is 2+2?"}
    ],
    "max_tokens": 64,
    "temperature": 0.8,
    "stream": false
  }'
```

Set `"stream": true` for Server-Sent Events (SSE) streaming.

### POST /v1/completions

Legacy text completion (prompt in, text out).

### GET /v1/models

List loaded models.

### GET /health

Returns `{"status":"ok"}` when ready, `503` when loading.

### GET /metrics

Prometheus-format metrics:

```
bitnetd_requests_total 1234
bitnetd_tokens_generated_total 56789
bitnetd_errors_total 0
bitnetd_active_connections 3
bitnetd_model_loaded 1
bitnetd_tokens_per_second 13.62
bitnetd_inference_seconds_bucket{le="1.00"} 500
```

## Deployment

### systemd (Linux)

```bash
sudo cp conf/bitnetd.conf.example /etc/bitnetd.conf
sudo cp rc.d/bitnetd.service /etc/systemd/system/
sudo systemctl enable --now bitnetd
```

### rc.d (OpenBSD)

```bash
doas cp conf/bitnetd.conf.example /etc/bitnetd.conf
doas cp rc.d/bitnetd.rc /etc/rc.d/bitnetd
doas chmod 755 /etc/rc.d/bitnetd
doas rcctl enable bitnetd
doas rcctl start bitnetd
```

## Project Structure

```
src/
  bitnetd.c             Main daemon (fork, signals, event loop)
  config.c / config.h   INI config parser
  log.c / log.h         syslog + file logging
  json.c / json.h       JSON parser/emitter
  http.c / http.h       HTTP/1.1 server
  api.c / api.h         OpenAI-compatible API handlers
  backend.c / backend.h Subprocess backend (wraps llama-server)
  metrics.c / metrics.h Prometheus metrics (atomic counters)
  poller.h              I/O multiplexing abstraction
  poller_epoll.c        Linux epoll backend
  poller_kqueue.c       BSD/macOS kqueue backend
  poller_poll.c         Portable poll() fallback
include/
  bitnetd.h             Public constants and types
tools/
  bitnetctl.c           CLI control tool
conf/
  bitnetd.conf.example  Example configuration
rc.d/
  bitnetd.service       systemd unit file
  bitnetd.rc            OpenBSD rc.d script
man/
  bitnetd.8             Daemon man page
  bitnetd.conf.5        Configuration man page
  bitnetctl.1           Control tool man page
tests/
  test_config.c         Config parser tests
  test_json.c           JSON parser/emitter tests
```

## License

MIT &mdash; see [LICENSE](LICENSE).
