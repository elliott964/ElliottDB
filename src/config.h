#ifndef EDB_CONFIG_H
#define EDB_CONFIG_H

#include "edb_common.h"

typedef enum {
    EDB_CFG_INT = 0,
    EDB_CFG_BOOL = 1,
    EDB_CFG_STR = 2
} edb_cfg_type_t;

typedef struct {
    /* [server] */
    char host[128];
    int64_t port;
    int64_t http_port;
    char node_name[128];
    char pid_file[EDB_MAX_PATH];
    int64_t shutdown_timeout_ms;
    bool banner_enabled;
    bool daemonize;

    /* [network] */
    int64_t max_connections;
    int64_t listen_backlog;
    int64_t io_threads;
    bool tcp_nodelay;
    bool tcp_keepalive;
    int64_t keepalive_idle_sec;
    int64_t keepalive_interval_sec;
    int64_t socket_recv_buf_kb;
    int64_t socket_send_buf_kb;
    int64_t read_buf_initial_kb;
    int64_t read_buf_max_mb;
    int64_t write_buf_initial_kb;
    int64_t idle_timeout_sec;
    int64_t request_timeout_ms;
    int64_t max_pipeline_depth;
    int64_t max_query_size_mb;
    bool enable_http;
    bool enable_binary_protocol;
    bool enable_text_protocol;
    bool reuse_port;

    /* [concurrency] */
    int64_t worker_threads;
    int64_t queue_capacity;
    int64_t queue_shards;
    int64_t lock_stripes;
    int64_t spin_count;
    int64_t batch_dispatch_size;
    int64_t max_concurrent_writers;
    char overload_policy[32];
    int64_t max_queue_wait_ms;
    int64_t conn_slab_chunk;

    /* [storage] */
    char data_dir[EDB_MAX_PATH];
    char db_filename[256];
    int64_t page_size;
    bool in_memory;
    bool auto_create_dir;
    int64_t preallocate_mb;
    int64_t fill_factor;
    int64_t compact_threshold_pct;
    int64_t max_db_size_gb;

    /* [buffer_pool] */
    int64_t buffer_pool_size_mb;
    int64_t buffer_pool_shards;
    char eviction_policy[32];
    int64_t dirty_flush_pct;
    int64_t bg_flush_interval_ms;
    int64_t prefetch_pages;

    /* [wal] */
    bool wal_enabled;
    char wal_dir[EDB_MAX_PATH];
    char wal_sync_mode[32];
    int64_t wal_group_commit_ms;
    int64_t wal_group_commit_max_ops;
    int64_t wal_segment_size_mb;
    int64_t wal_checkpoint_interval_sec;
    int64_t wal_checkpoint_mb;
    bool wal_checksum_enabled;
    char wal_recovery_mode[32];

    /* [query] */
    bool plan_cache_enabled;
    int64_t plan_cache_entries;
    bool result_cache_enabled;
    int64_t result_cache_mb;
    int64_t result_cache_ttl_ms;
    int64_t max_rows_returned;
    int64_t default_limit;
    int64_t work_mem_mb;
    bool hash_join_enabled;
    bool index_scan_enabled;
    int64_t slow_query_ms;

    /* [index] */
    int64_t btree_order;
    bool auto_index_pk;
    char default_index_type[32];
    int64_t hash_load_factor;

    /* [security] */
    bool auth_enabled;
    char admin_user[128];
    char admin_password[256];
    bool read_only;
    int64_t max_auth_failures;
    char allowed_hosts[512];

    /* [logging] */
    char log_level[32];
    char log_file[EDB_MAX_PATH];
    char log_format[32];
    bool log_queries;
    bool log_slow_queries;

    /* [metrics] */
    bool metrics_enabled;
    int64_t metrics_interval_sec;
    bool track_latency;

    /* Runtime tracking */
    char config_file_used[EDB_MAX_PATH];
} edb_config_t;

typedef struct {
    const char *section;
    const char *key;
    edb_cfg_type_t type;
    size_t offset;
    size_t str_size;
    int64_t min_val;
    int64_t max_val;
    bool runtime_mutable;
    const char *def_str;
    const char *desc;
} edb_config_spec_t;

const edb_config_spec_t *edb_config_specs(size_t *out_count);
void edb_config_defaults(edb_config_t *cfg);
bool edb_config_load_file(edb_config_t *cfg, const char *path, char *err, size_t err_cap);
void edb_config_load_env(edb_config_t *cfg);
bool edb_config_parse_args(edb_config_t *cfg, int argc, char **argv, char *err, size_t err_cap);
bool edb_config_finalize(edb_config_t *cfg, char *err, size_t err_cap);
bool edb_config_set_kv(edb_config_t *cfg, const char *full_key, const char *val, bool runtime_only, char *err, size_t err_cap);
bool edb_config_get_kv_str(const edb_config_t *cfg, const edb_config_spec_t *spec, char *out, size_t out_cap);
bool edb_config_write_template(const char *path, char *err, size_t err_cap);
void edb_config_dump_json(const edb_config_t *cfg, edb_buf_t *out);

#endif /* EDB_CONFIG_H */
