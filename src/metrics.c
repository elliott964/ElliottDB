#include "metrics.h"

void edb_metrics_init(edb_metrics_t *m) {
    memset(m, 0, sizeof(*m));
    m->start_time_ns = edb_now_ns();
}

void edb_metrics_record_latency(edb_metrics_t *m, uint64_t us) {
    int bucket = 0;
    uint64_t bound = 1;
    while (bucket < EDB_LAT_BUCKETS - 1 && us > bound) {
        bound <<= 1;
        bucket++;
    }
    edb_atomic_add_u64(&m->lat_hist[bucket], 1);
}

uint64_t edb_metrics_percentile_us(const edb_metrics_t *m, double pct) {
    uint64_t counts[EDB_LAT_BUCKETS];
    uint64_t total = 0;
    for (int i = 0; i < EDB_LAT_BUCKETS; i++) {
        counts[i] = edb_atomic_load_u64(&m->lat_hist[i]);
        total += counts[i];
    }
    if (total == 0) return 0;
    uint64_t target = (uint64_t)((double)total * (pct / 100.0));
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (int i = 0; i < EDB_LAT_BUCKETS; i++) {
        cum += counts[i];
        if (cum >= target) {
            return (uint64_t)1 << i;
        }
    }
    return (uint64_t)1 << (EDB_LAT_BUCKETS - 1);
}

void edb_metrics_dump_json(const edb_metrics_t *m, const char *node_name, edb_buf_t *out) {
    uint64_t now = edb_now_ns();
    double uptime_sec = (double)(now - m->start_time_ns) / 1000000000.0;
    uint64_t reqs = edb_atomic_load_u64(&m->total_requests);
    double qps = uptime_sec > 0.001 ? ((double)reqs / uptime_sec) : 0.0;

    edb_buf_printf(out,
        "{\n"
        "  \"node\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
        "  \"uptime_sec\": %.3f,\n"
        "  \"active_connections\": %lld,\n"
        "  \"total_connections\": %llu,\n"
        "  \"rejected_connections\": %llu,\n"
        "  \"total_requests\": %llu,\n"
        "  \"total_queries\": %llu,\n"
        "  \"total_errors\": %llu,\n"
        "  \"qps_avg\": %.2f,\n"
        "  \"bytes_in\": %llu,\n"
        "  \"bytes_out\": %llu,\n"
        "  \"select_count\": %llu,\n"
        "  \"insert_count\": %llu,\n"
        "  \"update_count\": %llu,\n"
        "  \"delete_count\": %llu,\n"
        "  \"ddl_count\": %llu,\n"
        "  \"plan_cache_hits\": %llu,\n"
        "  \"plan_cache_misses\": %llu,\n"
        "  \"result_cache_hits\": %llu,\n"
        "  \"result_cache_misses\": %llu,\n"
        "  \"buffer_pool_hits\": %llu,\n"
        "  \"buffer_pool_misses\": %llu,\n"
        "  \"wal_writes\": %llu,\n"
        "  \"wal_fsyncs\": %llu,\n"
        "  \"wal_bytes\": %llu,\n"
        "  \"checkpoints\": %llu,\n"
        "  \"slow_queries\": %llu,\n"
        "  \"latency_p50_us\": %llu,\n"
        "  \"latency_p95_us\": %llu,\n"
        "  \"latency_p99_us\": %llu\n"
        "}",
        node_name ? node_name : "elliottdb-1",
        ELLIOTTDB_VERSION,
        uptime_sec,
        (long long)edb_atomic_load_i64(&m->active_connections),
        (unsigned long long)edb_atomic_load_u64(&m->total_connections),
        (unsigned long long)edb_atomic_load_u64(&m->rejected_connections),
        (unsigned long long)reqs,
        (unsigned long long)edb_atomic_load_u64(&m->total_queries),
        (unsigned long long)edb_atomic_load_u64(&m->total_errors),
        qps,
        (unsigned long long)edb_atomic_load_u64(&m->bytes_in),
        (unsigned long long)edb_atomic_load_u64(&m->bytes_out),
        (unsigned long long)edb_atomic_load_u64(&m->select_count),
        (unsigned long long)edb_atomic_load_u64(&m->insert_count),
        (unsigned long long)edb_atomic_load_u64(&m->update_count),
        (unsigned long long)edb_atomic_load_u64(&m->delete_count),
        (unsigned long long)edb_atomic_load_u64(&m->ddl_count),
        (unsigned long long)edb_atomic_load_u64(&m->plan_cache_hits),
        (unsigned long long)edb_atomic_load_u64(&m->plan_cache_misses),
        (unsigned long long)edb_atomic_load_u64(&m->result_cache_hits),
        (unsigned long long)edb_atomic_load_u64(&m->result_cache_misses),
        (unsigned long long)edb_atomic_load_u64(&m->buffer_pool_hits),
        (unsigned long long)edb_atomic_load_u64(&m->buffer_pool_misses),
        (unsigned long long)edb_atomic_load_u64(&m->wal_writes),
        (unsigned long long)edb_atomic_load_u64(&m->wal_fsyncs),
        (unsigned long long)edb_atomic_load_u64(&m->wal_bytes),
        (unsigned long long)edb_atomic_load_u64(&m->checkpoints),
        (unsigned long long)edb_atomic_load_u64(&m->slow_queries),
        (unsigned long long)edb_metrics_percentile_us(m, 50.0),
        (unsigned long long)edb_metrics_percentile_us(m, 95.0),
        (unsigned long long)edb_metrics_percentile_us(m, 99.0));
}
