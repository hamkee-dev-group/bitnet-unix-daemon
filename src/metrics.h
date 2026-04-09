#ifndef BITNETD_METRICS_H
#define BITNETD_METRICS_H

#include <stdatomic.h>
#include <stddef.h>

#define METRICS_HIST_BUCKETS 10

typedef struct metrics {
    atomic_long requests_total;
    atomic_long tokens_generated;
    atomic_long errors_total;
    atomic_long active_connections;
    atomic_long model_loaded;

    atomic_long hist_buckets[METRICS_HIST_BUCKETS];
    atomic_long hist_count;
    atomic_long hist_sum_us;

    atomic_long tps_num;
} metrics_t;

static const double metrics_bucket_bounds[METRICS_HIST_BUCKETS] = {
    0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 5.0, 10.0, 30.0, 60.0
};

void    metrics_init(metrics_t *m);
void    metrics_inc_requests(metrics_t *m);
void    metrics_inc_errors(metrics_t *m);
void    metrics_add_tokens(metrics_t *m, long n);
void    metrics_observe_latency(metrics_t *m, double seconds);
void    metrics_set_connections(metrics_t *m, long n);
void    metrics_set_model_loaded(metrics_t *m, int loaded);
void    metrics_set_tps(metrics_t *m, double tps);
int     metrics_render(const metrics_t *m, char *buf, size_t cap);

#endif
