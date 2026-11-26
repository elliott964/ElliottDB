#include "server.h"

int edb_cli_main(int argc, char **argv);
int edb_bench_main(int argc, char **argv);

static edb_server_t *g_srv = NULL;
static edb_atomic_u32 g_stop_flag = {0};

static void on_shutdown_signal(void) {
    edb_atomic_store_u32(&g_stop_flag, 1);
    if (g_srv) edb_server_request_stop(g_srv);
}

static void print_usage(void) {
    printf("ElliottDB v%s - Ultra-Fast Single-Binary Relational Database\n\n", ELLIOTTDB_VERSION);
    printf("Usage:\n");
    printf("  elliottdb [server_flags]        Start database server (zero-config defaults)\n");
    printf("  elliottdb cli [cli_flags]       Connect interactive SQL client or run -c \"SQL\"\n");
    printf("  elliottdb bench [bench_flags]   Run high-concurrency benchmark\n");
    printf("  elliottdb init-config [path]    Generate documented elliottdb.conf\n");
    printf("  elliottdb version               Show version\n");
    printf("  elliottdb help                  Show this help\n\n");
    printf("Server Flags:\n");
    printf("  -c, --config <path>             Load INI configuration file\n");
    printf("  -H, --host <ip>                 Bind address (default 0.0.0.0)\n");
    printf("  -p, --port <port>               TCP port (default 7432)\n");
    printf("  -d, --data-dir <dir>            Data directory (default ./elliottdb_data)\n");
    printf("  -t, --threads <n>               Worker thread count (default auto)\n");
    printf("  --memory                        Pure in-memory mode\n");
    printf("  --no-banner                     Suppress startup banner\n");
    printf("  --<section>.<key>=<value>       Override any of 80+ configuration options\n");
}

static void print_banner(const edb_config_t *cfg) {
    printf(
        "\n"
        "  _____ _ _ _       _   _   ____  ____  \n"
        " | ____| | (_) ___ | |_| |_|  _ \\| __ ) \n"
        " |  _| | | | |/ _ \\| __| __| | | |  _ \\ \n"
        " | |___| | | | (_) | |_| |_| |_| | |_) |\n"
        " |_____|_|_|_|\\___/ \\__|\\__|____/|____/  v%s\n\n",
        ELLIOTTDB_VERSION);
    printf("  Node       : %s (PID %llu)\n", cfg->node_name, (unsigned long long)edb_pid());
    printf("  Listen     : %s:%lld (Binary EDB1 + HTTP/JSON + Text)\n", cfg->host, (long long)cfg->port);
    if (cfg->http_port > 0 && cfg->http_port != cfg->port) {
        printf("  HTTP Port  : %s:%lld\n", cfg->host, (long long)cfg->http_port);
    }
#ifdef _WIN32
    printf("  Reactor    : Windows IOCP (%lld I/O threads, %lld worker threads)\n",
           (long long)cfg->io_threads, (long long)cfg->worker_threads);
#else
    printf("  Reactor    : Linux epoll ET (%lld I/O threads, %lld worker threads)\n",
           (long long)cfg->io_threads, (long long)cfg->worker_threads);
#endif
    printf("  BufferPool : %lld MB (%lld shards, %s)\n",
           (long long)cfg->buffer_pool_size_mb, (long long)cfg->buffer_pool_shards, cfg->eviction_policy);
    printf("  Storage    : %s%s (page=%lld B, WAL=%s/%s)\n",
           cfg->in_memory ? "in-memory" : cfg->data_dir,
           cfg->in_memory ? "" : "",
           (long long)cfg->page_size,
           cfg->wal_enabled && !cfg->in_memory ? "on" : "off",
           cfg->wal_sync_mode);
    if (cfg->config_file_used[0]) {
        printf("  Config     : %s\n", cfg->config_file_used);
    }
    printf("  Ready for connections. Press Ctrl+C to stop.\n\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc >= 2) {
        if (strcmp(argv[1], "cli") == 0) {
            return edb_cli_main(argc - 2, argv + 2);
        }
        if (strcmp(argv[1], "bench") == 0) {
            return edb_bench_main(argc - 2, argv + 2);
        }
        if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            printf("ElliottDB v%s\n", ELLIOTTDB_VERSION);
            return 0;
        }
        if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(argv[1], "init-config") == 0) {
            const char *path = (argc >= 3) ? argv[2] : "elliottdb.conf";
            char err[256] = {0};
            if (!edb_config_write_template(path, err, sizeof(err))) {
                fprintf(stderr, "Error: %s\n", err);
                return 1;
            }
            printf("Generated ElliottDB configuration template: %s\n", path);
            return 0;
        }
    }

    edb_config_t cfg;
    edb_config_defaults(&cfg);

    char err[256] = {0};
    const char *explicit_cfg = NULL;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            explicit_cfg = argv[++i];
        }
    }

    if (explicit_cfg) {
        if (!edb_config_load_file(&cfg, explicit_cfg, err, sizeof(err))) {
            fprintf(stderr, "Config error: %s\n", err);
            return 1;
        }
    } else if (edb_file_exists("elliottdb.conf")) {
        if (!edb_config_load_file(&cfg, "elliottdb.conf", err, sizeof(err))) {
            fprintf(stderr, "Config error: %s\n", err);
            return 1;
        }
    }

    edb_config_load_env(&cfg);
    if (!edb_config_parse_args(&cfg, argc, argv, err, sizeof(err))) {
        fprintf(stderr, "Argument error: %s\n", err);
        return 1;
    }
    if (!edb_config_finalize(&cfg, err, sizeof(err))) {
        fprintf(stderr, "Configuration validation failed: %s\n", err);
        return 1;
    }

    edb_log_init(cfg.log_level, cfg.log_file, cfg.log_format);

    if (cfg.pid_file[0]) {
        FILE *pf = fopen(cfg.pid_file, "w");
        if (pf) {
            fprintf(pf, "%llu\n", (unsigned long long)edb_pid());
            fclose(pf);
        }
    }

    edb_server_t *srv = (edb_server_t *)calloc(1, sizeof(edb_server_t));
    if (!srv) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    if (!edb_server_init(srv, &cfg, err, sizeof(err))) {
        fprintf(stderr, "Startup failed: %s\n", err);
        free(srv);
        return 1;
    }

    g_srv = srv;
    edb_install_signal_handlers(on_shutdown_signal);

    if (!edb_server_start(srv, err, sizeof(err))) {
        fprintf(stderr, "Failed to start reactor: %s\n", err);
        edb_server_destroy(srv);
        free(srv);
        return 1;
    }

    if (cfg.banner_enabled) print_banner(&cfg);
    EDB_LOG_I("ElliottDB v%s started on %s:%lld", ELLIOTTDB_VERSION, cfg.host, (long long)cfg.port);

    while (!edb_atomic_load_u32(&g_stop_flag) && !edb_atomic_load_u32(&srv->stopping)) {
        edb_sleep_ms(100);
    }

    EDB_LOG_I("Shutting down ElliottDB...");
    edb_server_destroy(srv);
    free(srv);
    if (cfg.pid_file[0]) edb_file_remove(cfg.pid_file);
    edb_log_shutdown();
    return 0;
}
