#include "metrics.h"
#include <stdio.h>
#include <string.h>

void
metrics_init(metrics_t *m)
{
    memset(m, 0, sizeof(*m));
}

void
metrics_inc_requests(metrics_t *m)
{
    atomic_fetch_add(&m->requests_total, 1);
}

void
metrics_inc_errors(metrics_t *m)
{
    atomic_fetch_add(&m->errors_total, 1);
}

void
metrics_add_tokens(metrics_t *m, long n)
{
    atomic_fetch_add(&m->tokens_generated, n);
}

void
metrics_observe_latency(metrics_t *m, double seconds)
{
    for (int i = 0; i < METRICS_HIST_BUCKETS; i++) {
        if (seconds <= metrics_bucket_bounds[i]) {
            atomic_fetch_add(&m->hist_buckets[i], 1);
            break;
        }
    }
    atomic_fetch_add(&m->hist_count, 1);
    atomic_fetch_add(&m->hist_sum_us, (long)(seconds * 1000000.0));
}

void
metrics_set_connections(metrics_t *m, long n)
{
    atomic_store(&m->active_connections, n);
}

void
metrics_set_model_loaded(metrics_t *m, int loaded)
{
    atomic_store(&m->model_loaded, loaded ? 1 : 0);
}

void
metrics_set_tps(metrics_t *m, double tps)
{
    atomic_store(&m->tps_num, (long)(tps * 100.0));
}

int
metrics_render(const metrics_t *m, char *buf, size_t cap)
{
    int pos = 0;

    pos += snprintf(buf + pos, cap - (size_t)pos,
        "# HELP bitnetd_requests_total Total HTTP requests.\n"
        "# TYPE bitnetd_requests_total counter\n"
        "bitnetd_requests_total %ld\n\n",
        atomic_load(&m->requests_total));

    pos += snprintf(buf + pos, cap - (size_t)pos,
        "# HELP bitnetd_tokens_generated_total Total tokens generated.\n"
        "# TYPE bitnetd_tokens_generated_total counter\n"
        "bitnetd_tokens_generated_total %ld\n\n",
        atomic_load(&m->tokens_generated));

    pos += snprintf(buf + pos, cap - (size_t)pos,
        "# HELP bitnetd_errors_total Total errors.\n"
        "# TYPE bitnetd_errors_total counter\n"
        "bitnetd_errors_total %ld\n\n",
        atomic_load(&m->errors_total));

    pos += snprintf(buf + pos, cap - (size_t)pos,
        "# HELP bitnetd_active_connections Current active connections.\n"
        "# TYPE bitnetd_active_connections gauge\n"
        "bitnetd_active_connections %ld\n\n",
        atomic_load(&m->active_connections));

    pos += snprintf(buf + pos, cap - (size_t)pos,
        "# HELP bitnetd_model_loaded Whether model is loaded.\n"
        "# TYPE bitnetd_model_loaded gauge\n"
        "bitnetd_model_loaded %ld\n\n",
        atomic_load(&m->model_loaded));

    long tps_x100 = atomic_load(&m->tps_num);
    pos += snprintf(buf + pos, cap - (size_t)pos,
        "# HELP bitnetd_tokens_per_second Current tokens per second.\n"
        "# TYPE bitnetd_tokens_per_second gauge\n"
        "bitnetd_tokens_per_second %.2f\n\n",
        (double)tps_x100 / 100.0);

    pos += snprintf(buf + pos, cap - (size_t)pos,
        "# HELP bitnetd_inference_seconds Inference latency.\n"
        "# TYPE bitnetd_inference_seconds histogram\n");

    long cumulative = 0;
    for (int i = 0; i < METRICS_HIST_BUCKETS; i++) {
        cumulative += atomic_load(&m->hist_buckets[i]);
        pos += snprintf(buf + pos, cap - (size_t)pos,
            "bitnetd_inference_seconds_bucket{le=\"%.2f\"} %ld\n",
            metrics_bucket_bounds[i], cumulative);
    }

    long total_count = atomic_load(&m->hist_count);
    long sum_us = atomic_load(&m->hist_sum_us);
    pos += snprintf(buf + pos, cap - (size_t)pos,
        "bitnetd_inference_seconds_bucket{le=\"+Inf\"} %ld\n"
        "bitnetd_inference_seconds_sum %.6f\n"
        "bitnetd_inference_seconds_count %ld\n",
        total_count, (double)sum_us / 1000000.0, total_count);

    return pos;
}
