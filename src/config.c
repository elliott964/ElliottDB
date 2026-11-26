#include "config.h"
#include "pal.h"
#include <ctype.h>

#define CFG_OFF(field) offsetof(edb_config_t, field)
#define CFG_STR(field) offsetof(edb_config_t, field), sizeof(((edb_config_t *)0)->field)

static const edb_config_spec_t g_specs[] = {
    /* [server] */
    {"server", "host", EDB_CFG_STR, CFG_STR(host), 0, 0, false, "0.0.0.0", "Bind IP address (0.0.0.0 for all interfaces)"},
    {"server", "port", EDB_CFG_INT, CFG_OFF(port), 0, 1, 65535, false, "7432", "Primary TCP port (multiplexes Binary, HTTP/JSON, and Text protocols)"},
    {"server", "http_port", EDB_CFG_INT, CFG_OFF(http_port), 0, 0, 65535, false, "0", "Optional dedicated HTTP port (0 = multiplexed on main port)"},
    {"server", "node_name", EDB_CFG_STR, CFG_STR(node_name), 0, 0, true, "elliottdb-1", "Logical node identifier"},
    {"server", "pid_file", EDB_CFG_STR, CFG_STR(pid_file), 0, 0, false, "", "Optional PID file path"},
    {"server", "shutdown_timeout_ms", EDB_CFG_INT, CFG_OFF(shutdown_timeout_ms), 0, 100, 300000, true, "5000", "Graceful shutdown drain timeout in ms"},
    {"server", "banner_enabled", EDB_CFG_BOOL, CFG_OFF(banner_enabled), 0, 0, 1, false, "true", "Print startup banner"},
    {"server", "daemonize", EDB_CFG_BOOL, CFG_OFF(daemonize), 0, 0, 1, false, "false", "Run as background daemon (Linux)"},

    /* [network] */
    {"network", "max_connections", EDB_CFG_INT, CFG_OFF(max_connections), 0, 0, 100000000, true, "0", "Max concurrent connections (0 = unlimited dynamically growing slab)"},
    {"network", "listen_backlog", EDB_CFG_INT, CFG_OFF(listen_backlog), 0, 16, 1048576, false, "65535", "TCP listen backlog"},
    {"network", "io_threads", EDB_CFG_INT, CFG_OFF(io_threads), 0, 0, 256, false, "0", "Reactor I/O threads (0 = auto)"},
    {"network", "tcp_nodelay", EDB_CFG_BOOL, CFG_OFF(tcp_nodelay), 0, 0, 1, true, "true", "Disable Nagle's algorithm for low latency"},
    {"network", "tcp_keepalive", EDB_CFG_BOOL, CFG_OFF(tcp_keepalive), 0, 0, 1, true, "true", "Enable TCP keepalive probes"},
    {"network", "keepalive_idle_sec", EDB_CFG_INT, CFG_OFF(keepalive_idle_sec), 0, 1, 86400, true, "60", "TCP keepalive idle seconds"},
    {"network", "keepalive_interval_sec", EDB_CFG_INT, CFG_OFF(keepalive_interval_sec), 0, 1, 3600, true, "10", "TCP keepalive probe interval seconds"},
    {"network", "socket_recv_buf_kb", EDB_CFG_INT, CFG_OFF(socket_recv_buf_kb), 0, 0, 65536, true, "256", "SO_RCVBUF size in KB (0 = OS default)"},
    {"network", "socket_send_buf_kb", EDB_CFG_INT, CFG_OFF(socket_send_buf_kb), 0, 0, 65536, true, "256", "SO_SNDBUF size in KB (0 = OS default)"},
    {"network", "read_buf_initial_kb", EDB_CFG_INT, CFG_OFF(read_buf_initial_kb), 0, 1, 4096, true, "8", "Initial per-connection read buffer in KB"},
    {"network", "read_buf_max_mb", EDB_CFG_INT, CFG_OFF(read_buf_max_mb), 0, 1, 1024, true, "64", "Maximum per-connection read buffer in MB"},
    {"network", "write_buf_initial_kb", EDB_CFG_INT, CFG_OFF(write_buf_initial_kb), 0, 1, 4096, true, "16", "Initial per-connection write buffer in KB"},
    {"network", "idle_timeout_sec", EDB_CFG_INT, CFG_OFF(idle_timeout_sec), 0, 0, 864000, true, "300", "Close idle connections after N seconds (0 = disabled)"},
    {"network", "request_timeout_ms", EDB_CFG_INT, CFG_OFF(request_timeout_ms), 0, 0, 3600000, true, "30000", "Request execution timeout in ms"},
    {"network", "max_pipeline_depth", EDB_CFG_INT, CFG_OFF(max_pipeline_depth), 0, 1, 65536, true, "1024", "Max pipelined frames processed per connection batch"},
    {"network", "max_query_size_mb", EDB_CFG_INT, CFG_OFF(max_query_size_mb), 0, 1, 512, true, "16", "Maximum SQL query payload in MB"},
    {"network", "enable_http", EDB_CFG_BOOL, CFG_OFF(enable_http), 0, 0, 1, true, "true", "Enable HTTP/1.1 JSON API"},
    {"network", "enable_binary_protocol", EDB_CFG_BOOL, CFG_OFF(enable_binary_protocol), 0, 0, 1, true, "true", "Enable high-speed EDB1 binary protocol"},
    {"network", "enable_text_protocol", EDB_CFG_BOOL, CFG_OFF(enable_text_protocol), 0, 0, 1, true, "true", "Enable text line SQL protocol"},
    {"network", "reuse_port", EDB_CFG_BOOL, CFG_OFF(reuse_port), 0, 0, 1, false, "true", "Enable SO_REUSEPORT on Linux"},

    /* [concurrency] */
    {"concurrency", "worker_threads", EDB_CFG_INT, CFG_OFF(worker_threads), 0, 0, 1024, false, "0", "Query worker threads (0 = auto CPU count)"},
    {"concurrency", "queue_capacity", EDB_CFG_INT, CFG_OFF(queue_capacity), 0, 64, 16777216, false, "65536", "Total request queue capacity across shards"},
    {"concurrency", "queue_shards", EDB_CFG_INT, CFG_OFF(queue_shards), 0, 1, 256, false, "16", "Number of work queue shards (power of 2 recommended)"},
    {"concurrency", "lock_stripes", EDB_CFG_INT, CFG_OFF(lock_stripes), 0, 1, 1024, false, "64", "Lock stripe count for sharded subsystems"},
    {"concurrency", "spin_count", EDB_CFG_INT, CFG_OFF(spin_count), 0, 0, 100000, true, "4000", "Spin count before sleeping"},
    {"concurrency", "batch_dispatch_size", EDB_CFG_INT, CFG_OFF(batch_dispatch_size), 0, 1, 4096, true, "64", "Batch size for queue dispatch"},
    {"concurrency", "max_concurrent_writers", EDB_CFG_INT, CFG_OFF(max_concurrent_writers), 0, 0, 1024, true, "0", "Max concurrent writers (0 = auto)"},
    {"concurrency", "overload_policy", EDB_CFG_STR, CFG_STR(overload_policy), 0, 0, true, "queue", "Overload handling: queue | shed | backpressure"},
    {"concurrency", "max_queue_wait_ms", EDB_CFG_INT, CFG_OFF(max_queue_wait_ms), 0, 1, 600000, true, "5000", "Max time a request waits in queue"},
    {"concurrency", "conn_slab_chunk", EDB_CFG_INT, CFG_OFF(conn_slab_chunk), 0, 64, 65536, false, "1024", "Connections allocated per slab chunk"},

    /* [storage] */
    {"storage", "data_dir", EDB_CFG_STR, CFG_STR(data_dir), 0, 0, false, "./elliottdb_data", "Database data directory"},
    {"storage", "db_filename", EDB_CFG_STR, CFG_STR(db_filename), 0, 0, false, "elliottdb.edb", "Primary database file name"},
    {"storage", "page_size", EDB_CFG_INT, CFG_OFF(page_size), 0, 4096, 65536, false, "8192", "Page size in bytes (power of 2: 4096..65536)"},
    {"storage", "in_memory", EDB_CFG_BOOL, CFG_OFF(in_memory), 0, 0, 1, false, "false", "Run purely in memory with no disk persistence"},
    {"storage", "auto_create_dir", EDB_CFG_BOOL, CFG_OFF(auto_create_dir), 0, 0, 1, false, "true", "Automatically create data directory"},
    {"storage", "preallocate_mb", EDB_CFG_INT, CFG_OFF(preallocate_mb), 0, 0, 1048576, false, "0", "Preallocate database file size in MB"},
    {"storage", "fill_factor", EDB_CFG_INT, CFG_OFF(fill_factor), 0, 50, 100, true, "90", "Target page fill factor percentage"},
    {"storage", "compact_threshold_pct", EDB_CFG_INT, CFG_OFF(compact_threshold_pct), 0, 5, 90, true, "30", "Dead-space percentage triggering page compaction"},
    {"storage", "max_db_size_gb", EDB_CFG_INT, CFG_OFF(max_db_size_gb), 0, 0, 1048576, true, "0", "Maximum database size in GB (0 = unlimited)"},

    /* [buffer_pool] */
    {"buffer_pool", "size_mb", EDB_CFG_INT, CFG_OFF(buffer_pool_size_mb), 0, 0, 1048576, false, "0", "Buffer pool memory in MB (0 = auto 25% RAM, min 32MB, max 2048MB)"},
    {"buffer_pool", "shards", EDB_CFG_INT, CFG_OFF(buffer_pool_shards), 0, 1, 256, false, "64", "Buffer pool partition shards (power of 2)"},
    {"buffer_pool", "eviction_policy", EDB_CFG_STR, CFG_STR(eviction_policy), 0, 0, false, "clock", "Eviction policy: clock | lru"},
    {"buffer_pool", "dirty_flush_pct", EDB_CFG_INT, CFG_OFF(dirty_flush_pct), 0, 10, 100, true, "75", "Dirty page threshold percentage"},
    {"buffer_pool", "bg_flush_interval_ms", EDB_CFG_INT, CFG_OFF(bg_flush_interval_ms), 0, 50, 60000, true, "1000", "Background flush interval in ms"},
    {"buffer_pool", "prefetch_pages", EDB_CFG_INT, CFG_OFF(prefetch_pages), 0, 0, 128, true, "4", "Sequential scan prefetch pages"},

    /* [wal] */
    {"wal", "enabled", EDB_CFG_BOOL, CFG_OFF(wal_enabled), 0, 0, 1, false, "true", "Enable Write-Ahead Logging"},
    {"wal", "dir", EDB_CFG_STR, CFG_STR(wal_dir), 0, 0, false, "", "WAL directory (empty = same as data_dir)"},
    {"wal", "sync_mode", EDB_CFG_STR, CFG_STR(wal_sync_mode), 0, 0, true, "normal", "Durability mode: off | normal | full"},
    {"wal", "group_commit_ms", EDB_CFG_INT, CFG_OFF(wal_group_commit_ms), 0, 0, 1000, true, "1", "Group commit coalescing window in ms"},
    {"wal", "group_commit_max_ops", EDB_CFG_INT, CFG_OFF(wal_group_commit_max_ops), 0, 1, 65536, true, "1024", "Max operations per group commit batch"},
    {"wal", "segment_size_mb", EDB_CFG_INT, CFG_OFF(wal_segment_size_mb), 0, 1, 4096, false, "64", "WAL segment rotation size in MB"},
    {"wal", "checkpoint_interval_sec", EDB_CFG_INT, CFG_OFF(wal_checkpoint_interval_sec), 0, 1, 86400, true, "60", "Automatic checkpoint interval in seconds"},
    {"wal", "checkpoint_wal_mb", EDB_CFG_INT, CFG_OFF(wal_checkpoint_mb), 0, 1, 16384, true, "32", "Trigger checkpoint when WAL exceeds N MB"},
    {"wal", "checksum_enabled", EDB_CFG_BOOL, CFG_OFF(wal_checksum_enabled), 0, 0, 1, true, "true", "Verify CRC32C checksums on WAL records"},
    {"wal", "recovery_mode", EDB_CFG_STR, CFG_STR(wal_recovery_mode), 0, 0, false, "strict", "WAL recovery mode: strict | lenient"},

    /* [query] */
    {"query", "plan_cache_enabled", EDB_CFG_BOOL, CFG_OFF(plan_cache_enabled), 0, 0, 1, true, "true", "Enable sharded parsed SQL plan cache"},
    {"query", "plan_cache_entries", EDB_CFG_INT, CFG_OFF(plan_cache_entries), 0, 0, 1000000, true, "4096", "Max cached SQL plans"},
    {"query", "result_cache_enabled", EDB_CFG_BOOL, CFG_OFF(result_cache_enabled), 0, 0, 1, true, "true", "Enable sharded query result cache"},
    {"query", "result_cache_mb", EDB_CFG_INT, CFG_OFF(result_cache_mb), 0, 0, 65536, true, "64", "Max result cache memory in MB"},
    {"query", "result_cache_ttl_ms", EDB_CFG_INT, CFG_OFF(result_cache_ttl_ms), 0, 0, 3600000, true, "5000", "Result cache entry TTL in ms"},
    {"query", "max_rows_returned", EDB_CFG_INT, CFG_OFF(max_rows_returned), 0, 0, 100000000, true, "1000000", "Safety cap on rows returned per query (0 = unlimited)"},
    {"query", "default_limit", EDB_CFG_INT, CFG_OFF(default_limit), 0, 0, 10000000, true, "0", "Default SELECT LIMIT if omitted (0 = none)"},
    {"query", "work_mem_mb", EDB_CFG_INT, CFG_OFF(work_mem_mb), 0, 1, 16384, true, "16", "Per-query sort/hash work memory in MB"},
    {"query", "hash_join_enabled", EDB_CFG_BOOL, CFG_OFF(hash_join_enabled), 0, 0, 1, true, "true", "Use hash join for equi-joins"},
    {"query", "index_scan_enabled", EDB_CFG_BOOL, CFG_OFF(index_scan_enabled), 0, 0, 1, true, "true", "Use B+Tree/Hash indexes for WHERE predicates"},
    {"query", "slow_query_ms", EDB_CFG_INT, CFG_OFF(slow_query_ms), 0, 0, 3600000, true, "100", "Slow query threshold in ms"},

    /* [index] */
    {"index", "btree_order", EDB_CFG_INT, CFG_OFF(btree_order), 0, 8, 512, false, "64", "B+Tree branching order"},
    {"index", "auto_index_pk", EDB_CFG_BOOL, CFG_OFF(auto_index_pk), 0, 0, 1, true, "true", "Automatically create index on PRIMARY KEY"},
    {"index", "default_index_type", EDB_CFG_STR, CFG_STR(default_index_type), 0, 0, true, "btree", "Default index type: btree | hash"},
    {"index", "hash_load_factor", EDB_CFG_INT, CFG_OFF(hash_load_factor), 0, 25, 95, true, "75", "Hash index resize load factor percentage"},

    /* [security] */
    {"security", "auth_enabled", EDB_CFG_BOOL, CFG_OFF(auth_enabled), 0, 0, 1, true, "false", "Require authentication on connections"},
    {"security", "admin_user", EDB_CFG_STR, CFG_STR(admin_user), 0, 0, true, "admin", "Default admin username"},
    {"security", "admin_password", EDB_CFG_STR, CFG_STR(admin_password), 0, 0, true, "", "Admin password when auth_enabled=true"},
    {"security", "read_only", EDB_CFG_BOOL, CFG_OFF(read_only), 0, 0, 1, true, "false", "Reject all write/DDL queries"},
    {"security", "max_auth_failures", EDB_CFG_INT, CFG_OFF(max_auth_failures), 0, 1, 1000, true, "5", "Disconnect after N failed auth attempts"},
    {"security", "allowed_hosts", EDB_CFG_STR, CFG_STR(allowed_hosts), 0, 0, true, "*", "Allowed host pattern (* for any)"},

    /* [logging] */
    {"logging", "level", EDB_CFG_STR, CFG_STR(log_level), 0, 0, true, "info", "Log level: trace | debug | info | warn | error | fatal"},
    {"logging", "file", EDB_CFG_STR, CFG_STR(log_file), 0, 0, false, "", "Log file path (empty = stderr)"},
    {"logging", "format", EDB_CFG_STR, CFG_STR(log_format), 0, 0, false, "text", "Log format: text | json"},
    {"logging", "log_queries", EDB_CFG_BOOL, CFG_OFF(log_queries), 0, 0, 1, true, "false", "Log every executed SQL query"},
    {"logging", "log_slow_queries", EDB_CFG_BOOL, CFG_OFF(log_slow_queries), 0, 0, 1, true, "true", "Log queries slower than slow_query_ms"},

    /* [metrics] */
    {"metrics", "enabled", EDB_CFG_BOOL, CFG_OFF(metrics_enabled), 0, 0, 1, true, "true", "Collect atomic runtime metrics"},
    {"metrics", "interval_sec", EDB_CFG_INT, CFG_OFF(metrics_interval_sec), 0, 0, 86400, true, "0", "Periodic metrics log interval in sec (0 = disabled)"},
    {"metrics", "track_latency", EDB_CFG_BOOL, CFG_OFF(track_latency), 0, 0, 1, true, "true", "Track microsecond latency histogram"}
};

static const size_t g_spec_count = sizeof(g_specs) / sizeof(g_specs[0]);

const edb_config_spec_t *edb_config_specs(size_t *out_count) {
    if (out_count) *out_count = g_spec_count;
    return g_specs;
}

static bool parse_bool(const char *val, bool *out) {
    if (!val) return false;
    if (edb_strcasecmp(val, "true") == 0 || edb_strcasecmp(val, "1") == 0 ||
        edb_strcasecmp(val, "yes") == 0 || edb_strcasecmp(val, "on") == 0) {
        *out = true;
        return true;
    }
    if (edb_strcasecmp(val, "false") == 0 || edb_strcasecmp(val, "0") == 0 ||
        edb_strcasecmp(val, "no") == 0 || edb_strcasecmp(val, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_i64(const char *val, int64_t *out) {
    if (!val || !*val) return false;
    char *end = NULL;
    long long v = strtoll(val, &end, 10);
    if (end == val) return false;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

static const edb_config_spec_t *find_spec(const char *section, const char *key) {
    for (size_t i = 0; i < g_spec_count; i++) {
        if (edb_strcasecmp(g_specs[i].section, section) == 0 &&
            edb_strcasecmp(g_specs[i].key, key) == 0) {
            return &g_specs[i];
        }
    }
    return NULL;
}

static bool apply_spec(edb_config_t *cfg, const edb_config_spec_t *sp, const char *val, char *err, size_t err_cap) {
    uint8_t *base = (uint8_t *)cfg;
    if (sp->type == EDB_CFG_INT) {
        int64_t v = 0;
        if (!parse_i64(val, &v)) {
            if (err) snprintf(err, err_cap, "Invalid integer for %s.%s: '%s'", sp->section, sp->key, val);
            return false;
        }
        if (v < sp->min_val || v > sp->max_val) {
            if (err) snprintf(err, err_cap, "Value %lld out of range [%lld, %lld] for %s.%s",
                              (long long)v, (long long)sp->min_val, (long long)sp->max_val, sp->section, sp->key);
            return false;
        }
        memcpy(base + sp->offset, &v, sizeof(int64_t));
    } else if (sp->type == EDB_CFG_BOOL) {
        bool b = false;
        if (!parse_bool(val, &b)) {
            if (err) snprintf(err, err_cap, "Invalid boolean for %s.%s: '%s'", sp->section, sp->key, val);
            return false;
        }
        memcpy(base + sp->offset, &b, sizeof(bool));
    } else {
        char *dst = (char *)(base + sp->offset);
        size_t n = strlen(val);
        if (n >= sp->str_size) n = sp->str_size - 1;
        memcpy(dst, val, n);
        dst[n] = '\0';
    }
    return true;
}

void edb_config_defaults(edb_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    for (size_t i = 0; i < g_spec_count; i++) {
        apply_spec(cfg, &g_specs[i], g_specs[i].def_str, NULL, 0);
    }
}

bool edb_config_set_kv(edb_config_t *cfg, const char *full_key, const char *val, bool runtime_only, char *err, size_t err_cap) {
    if (!full_key || !val) return false;
    const char *dot = strchr(full_key, '.');
    const edb_config_spec_t *sp = NULL;
    if (dot) {
        size_t slen = (size_t)(dot - full_key);
        char sec[64];
        if (slen >= sizeof(sec)) slen = sizeof(sec) - 1;
        memcpy(sec, full_key, slen);
        sec[slen] = '\0';
        sp = find_spec(sec, dot + 1);
    } else {
        /* Try unique key match without section prefix */
        for (size_t i = 0; i < g_spec_count; i++) {
            if (edb_strcasecmp(g_specs[i].key, full_key) == 0) {
                sp = &g_specs[i];
                break;
            }
        }
    }
    if (!sp) {
        if (err) snprintf(err, err_cap, "Unknown config key: '%s'", full_key);
        return false;
    }
    if (runtime_only && !sp->runtime_mutable) {
        if (err) snprintf(err, err_cap, "Config key '%s.%s' requires restart", sp->section, sp->key);
        return false;
    }
    return apply_spec(cfg, sp, val, err, err_cap);
}

bool edb_config_get_kv_str(const edb_config_t *cfg, const edb_config_spec_t *sp, char *out, size_t out_cap) {
    const uint8_t *base = (const uint8_t *)cfg;
    if (sp->type == EDB_CFG_INT) {
        int64_t v = 0;
        memcpy(&v, base + sp->offset, sizeof(int64_t));
        snprintf(out, out_cap, "%lld", (long long)v);
    } else if (sp->type == EDB_CFG_BOOL) {
        bool b = false;
        memcpy(&b, base + sp->offset, sizeof(bool));
        snprintf(out, out_cap, "%s", b ? "true" : "false");
    } else {
        const char *s = (const char *)(base + sp->offset);
        snprintf(out, out_cap, "%s", s);
    }
    return true;
}

static void strip_inline_comment(char *s) {
    bool in_quote = false;
    char qchar = 0;
    for (size_t i = 0; s[i]; i++) {
        if (!in_quote && (s[i] == '"' || s[i] == '\'')) {
            in_quote = true;
            qchar = s[i];
        } else if (in_quote && s[i] == qchar) {
            in_quote = false;
        } else if (!in_quote && (s[i] == '#' || s[i] == ';')) {
            s[i] = '\0';
            break;
        }
    }
}

static void unquote_str(char *s) {
    edb_str_trim(s);
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

bool edb_config_load_file(edb_config_t *cfg, const char *path, char *err, size_t err_cap) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (err) snprintf(err, err_cap, "Cannot open config file: %s", path);
        return false;
    }
    snprintf(cfg->config_file_used, sizeof(cfg->config_file_used), "%s", path);
    char line[2048];
    char section[64] = "server";
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        strip_inline_comment(line);
        edb_str_trim(line);
        if (line[0] == '\0') continue;

        size_t len = strlen(line);
        if (line[0] == '[' && line[len - 1] == ']') {
            line[len - 1] = '\0';
            snprintf(section, sizeof(section), "%s", line + 1);
            edb_str_trim(section);
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            fclose(fp);
            if (err) snprintf(err, err_cap, "%s:%d: expected key = value", path, lineno);
            return false;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        edb_str_trim(key);
        unquote_str(val);

        char full[160];
        if (strchr(key, '.')) {
            snprintf(full, sizeof(full), "%s", key);
        } else {
            snprintf(full, sizeof(full), "%s.%s", section, key);
        }
        if (!edb_config_set_kv(cfg, full, val, false, err, err_cap)) {
            fclose(fp);
            return false;
        }
    }
    fclose(fp);
    return true;
}

void edb_config_load_env(edb_config_t *cfg) {
    for (size_t i = 0; i < g_spec_count; i++) {
        char env_name[192];
        snprintf(env_name, sizeof(env_name), "ELLIOTTDB_%s_%s", g_specs[i].section, g_specs[i].key);
        for (size_t j = 0; env_name[j]; j++) {
            env_name[j] = (char)toupper((unsigned char)env_name[j]);
        }
        const char *v = getenv(env_name);
        if (v && *v) {
            apply_spec(cfg, &g_specs[i], v, NULL, 0);
        }
    }
}

bool edb_config_parse_args(edb_config_t *cfg, int argc, char **argv, char *err, size_t err_cap) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-p") == 0 || strcmp(a, "--port") == 0) {
            if (i + 1 >= argc) { snprintf(err, err_cap, "Missing value for %s", a); return false; }
            if (!edb_config_set_kv(cfg, "server.port", argv[++i], false, err, err_cap)) return false;
        } else if (strcmp(a, "-H") == 0 || strcmp(a, "--host") == 0) {
            if (i + 1 >= argc) { snprintf(err, err_cap, "Missing value for %s", a); return false; }
            if (!edb_config_set_kv(cfg, "server.host", argv[++i], false, err, err_cap)) return false;
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--data-dir") == 0) {
            if (i + 1 >= argc) { snprintf(err, err_cap, "Missing value for %s", a); return false; }
            if (!edb_config_set_kv(cfg, "storage.data_dir", argv[++i], false, err, err_cap)) return false;
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "--threads") == 0) {
            if (i + 1 >= argc) { snprintf(err, err_cap, "Missing value for %s", a); return false; }
            if (!edb_config_set_kv(cfg, "concurrency.worker_threads", argv[++i], false, err, err_cap)) return false;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--config") == 0) {
            if (i + 1 >= argc) { snprintf(err, err_cap, "Missing value for %s", a); return false; }
            i++; /* already loaded in pre-pass */
        } else if (strcmp(a, "--memory") == 0 || strcmp(a, "--in-memory") == 0) {
            cfg->in_memory = true;
        } else if (strcmp(a, "--no-banner") == 0) {
            cfg->banner_enabled = false;
        } else if (strncmp(a, "--", 2) == 0) {
            const char *kv = a + 2;
            const char *eq = strchr(kv, '=');
            if (eq) {
                char key[160];
                size_t klen = (size_t)(eq - kv);
                if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                memcpy(key, kv, klen);
                key[klen] = '\0';
                if (!edb_config_set_kv(cfg, key, eq + 1, false, err, err_cap)) return false;
            } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                /* Support --section.key val */
                if (strchr(kv, '.')) {
                    if (!edb_config_set_kv(cfg, kv, argv[++i], false, err, err_cap)) return false;
                }
            }
        }
    }
    return true;
}

static bool is_pow2_i64(int64_t v) {
    return v > 0 && (v & (v - 1)) == 0;
}

bool edb_config_finalize(edb_config_t *cfg, char *err, size_t err_cap) {
    int cpus = edb_cpu_count();
    uint64_t ram_mb = edb_total_ram_mb();

    if (cfg->worker_threads == 0) cfg->worker_threads = cpus > 0 ? cpus : 4;
    if (cfg->io_threads == 0) {
#ifdef _WIN32
        cfg->io_threads = cpus >= 4 ? (cpus / 2) : 2;
        if (cfg->io_threads > 16) cfg->io_threads = 16;
#else
        cfg->io_threads = cpus >= 4 ? (cpus / 2) : 2;
        if (cfg->io_threads > 16) cfg->io_threads = 16;
#endif
    }
    if (cfg->max_concurrent_writers == 0) cfg->max_concurrent_writers = cfg->worker_threads;

    if (cfg->buffer_pool_size_mb == 0) {
        int64_t auto_mb = (int64_t)(ram_mb / 4);
        if (auto_mb < 32) auto_mb = 32;
        if (auto_mb > 2048) auto_mb = 2048;
        cfg->buffer_pool_size_mb = auto_mb;
    }

    if (!is_pow2_i64(cfg->page_size)) {
        if (err) snprintf(err, err_cap, "storage.page_size must be a power of 2 (4096..65536)");
        return false;
    }
    if (!is_pow2_i64(cfg->buffer_pool_shards)) {
        if (err) snprintf(err, err_cap, "buffer_pool.shards must be a power of 2");
        return false;
    }
    if (!is_pow2_i64(cfg->queue_shards)) {
        if (err) snprintf(err, err_cap, "concurrency.queue_shards must be a power of 2");
        return false;
    }

    if (cfg->wal_dir[0] == '\0') {
        snprintf(cfg->wal_dir, sizeof(cfg->wal_dir), "%s", cfg->data_dir);
    }

    if (edb_strcasecmp(cfg->wal_sync_mode, "off") != 0 &&
        edb_strcasecmp(cfg->wal_sync_mode, "normal") != 0 &&
        edb_strcasecmp(cfg->wal_sync_mode, "full") != 0) {
        if (err) snprintf(err, err_cap, "wal.sync_mode must be off, normal, or full");
        return false;
    }
    if (edb_strcasecmp(cfg->overload_policy, "queue") != 0 &&
        edb_strcasecmp(cfg->overload_policy, "shed") != 0 &&
        edb_strcasecmp(cfg->overload_policy, "backpressure") != 0) {
        if (err) snprintf(err, err_cap, "concurrency.overload_policy must be queue, shed, or backpressure");
        return false;
    }
    if (edb_strcasecmp(cfg->default_index_type, "btree") != 0 &&
        edb_strcasecmp(cfg->default_index_type, "hash") != 0) {
        if (err) snprintf(err, err_cap, "index.default_index_type must be btree or hash");
        return false;
    }
    return true;
}

bool edb_config_write_template(const char *path, char *err, size_t err_cap) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        if (err) snprintf(err, err_cap, "Cannot write config template to %s", path);
        return false;
    }
    fprintf(fp, "# ElliottDB v%s Configuration File\n", ELLIOTTDB_VERSION);
    fprintf(fp, "# All settings have sensible auto-tuned defaults.\n");
    fprintf(fp, "# Override via file, ELLIOTTDB_<SECTION>_<KEY> env vars, or --section.key=value CLI flags.\n\n");
    const char *cur_sec = "";
    for (size_t i = 0; i < g_spec_count; i++) {
        const edb_config_spec_t *sp = &g_specs[i];
        if (strcmp(cur_sec, sp->section) != 0) {
            if (*cur_sec) fprintf(fp, "\n");
            cur_sec = sp->section;
            fprintf(fp, "[%s]\n", cur_sec);
        }
        fprintf(fp, "# %s\n", sp->desc);
        if (sp->type == EDB_CFG_STR) {
            fprintf(fp, "%s = \"%s\"\n", sp->key, sp->def_str);
        } else {
            fprintf(fp, "%s = %s\n", sp->key, sp->def_str);
        }
    }
    fclose(fp);
    return true;
}

void edb_config_dump_json(const edb_config_t *cfg, edb_buf_t *out) {
    edb_buf_append_str(out, "{\n");
    for (size_t i = 0; i < g_spec_count; i++) {
        const edb_config_spec_t *sp = &g_specs[i];
        char keybuf[192];
        snprintf(keybuf, sizeof(keybuf), "%s.%s", sp->section, sp->key);
        edb_buf_append_str(out, "  ");
        edb_json_append_escaped(out, keybuf, strlen(keybuf));
        edb_buf_append_str(out, ": ");
        char valbuf[EDB_MAX_PATH];
        edb_config_get_kv_str(cfg, sp, valbuf, sizeof(valbuf));
        if (sp->type == EDB_CFG_STR) {
            if (strcmp(sp->key, "admin_password") == 0 && valbuf[0] != '\0') {
                edb_buf_append_str(out, "\"********\"");
            } else {
                edb_json_append_escaped(out, valbuf, strlen(valbuf));
            }
        } else {
            edb_buf_append_str(out, valbuf);
        }
        edb_buf_append_str(out, (i + 1 < g_spec_count) ? ",\n" : "\n");
    }
    edb_buf_append_str(out, "}");
}
