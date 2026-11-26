#ifndef EDB_METRICS_H
#define EDB_METRICS_H

#include "pal.h"

#define EDB_LAT_BUCKETS 16

typedef struct {
    uint64_t start_time_ns;
    edb_atomic_i64 active_connections;
    edb_atomic_u64 total_connections;
    edb_atomic_u64 rejected_connections;
    edb_atomic_u64 total_requests;
    edb_atomic_u64 total_queries;
    edb_atomic_u64 total_errors;
    edb_atomic_u64 bytes_in;
    edb_atomic_u64 bytes_out;

    edb_atomic_u64 select_count;
    edb_atomic_u64 insert_count;
    edb_atomic_u64 update_count;
    edb_atomic_u64 delete_count;
    edb_atomic_u64 ddl_count;

    edb_atomic_u64 plan_cache_hits;
    edb_atomic_u64 plan_cache_misses;
    edb_atomic_u64 result_cache_hits;
    edb_atomic_u64 result_cache_misses;

    edb_atomic_u64 buffer_pool_hits;
    edb_atomic_u64 buffer_pool_misses;

    edb_atomic_u64 wal_writes;
    edb_atomic_u64 wal_fsyncs;
    edb_atomic_u64 wal_bytes;
    edb_atomic_u64 checkpoints;
    edb_atomic_u64 slow_queries;

    /* Bucket i counts latencies <= (1 << i) us (last bucket is > 32768 us) */
    edb_atomic_u64 lat_hist[EDB_LAT_BUCKETS];
} edb_metrics_t;

void edb_metrics_init(edb_metrics_t *m);
void edb_metrics_record_latency(edb_metrics_t *m, uint64_t us);
uint64_t edb_metrics_percentile_us(const edb_metrics_t *m, double pct);
void edb_metrics_dump_json(const edb_metrics_t *m, const char *node_name, edb_buf_t *out);

#endif /* EDB_METRICS_H */
