#include "protocol.h"
#include "pal.h"

typedef enum {
    WL_PING = 0,
    WL_READ,
    WL_POINT,
    WL_WRITE,
    WL_MIXED
} workload_t;

typedef struct {
    int tid;
    const char *host;
    uint16_t port;
    int clients;
    int requests;
    int pipeline;
    workload_t wl;
    bool embedded;
    edb_engine_t *eng;

    uint64_t ok_ops;
    uint64_t err_ops;
    uint64_t *latencies_us;
    size_t lat_count;
    size_t lat_cap;
} bench_worker_t;

static uint32_t bench_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void record_lat(bench_worker_t *w, uint64_t us) {
    if (w->lat_count == w->lat_cap) {
        size_t nc = w->lat_cap == 0 ? 4096 : w->lat_cap * 2;
        w->latencies_us = (uint64_t *)realloc(w->latencies_us, nc * sizeof(uint64_t));
        w->lat_cap = nc;
    }
    w->latencies_us[w->lat_count++] = us;
}

static void make_query(workload_t wl, uint64_t seq, char *buf, size_t cap, edb_bin_op_t *op) {
    if (wl == WL_PING) {
        *op = EDB_OP_PING;
        buf[0] = '\0';
    } else if (wl == WL_READ) {
        *op = EDB_OP_QUERY;
        snprintf(buf, cap, "SELECT id, val FROM bench_tbl WHERE id = 1");
    } else if (wl == WL_POINT) {
        *op = EDB_OP_QUERY;
        snprintf(buf, cap, "SELECT id, val FROM bench_tbl WHERE id = %llu", (unsigned long long)((seq % 1000) + 1));
    } else if (wl == WL_WRITE) {
        *op = EDB_OP_QUERY;
        snprintf(buf, cap, "INSERT INTO bench_write (k, v) VALUES (%llu, 'bench_payload_data')", (unsigned long long)seq);
    } else {
        *op = EDB_OP_QUERY;
        if ((seq % 5) == 0) {
            snprintf(buf, cap, "INSERT INTO bench_write (k, v) VALUES (%llu, 'mixed_payload')", (unsigned long long)seq);
        } else {
            snprintf(buf, cap, "SELECT id, val FROM bench_tbl WHERE id = %llu", (unsigned long long)((seq % 1000) + 1));
        }
    }
}

static void bench_worker_fn(void *arg) {
    bench_worker_t *w = (bench_worker_t *)arg;

    if (w->embedded) {
        edb_session_t sess;
        edb_session_init(&sess);
        char sql[256];
        for (int i = 0; i < w->requests; i++) {
            edb_bin_op_t op = EDB_OP_QUERY;
            make_query(w->wl, (uint64_t)w->tid * 1000000ULL + (uint64_t)i, sql, sizeof(sql), &op);
            if (op == EDB_OP_PING) strcpy(sql, "PING");
            uint64_t t0 = edb_now_us();
            edb_result_t r;
            edb_execute_sql(w->eng, &sess, sql, strlen(sql), &r);
            uint64_t dt = edb_now_us() - t0;
            if (r.status == EDB_OK) w->ok_ops++;
            else w->err_ops++;
            record_lat(w, dt);
            edb_result_free(&r);
        }
        edb_session_destroy(w->eng, &sess);
        return;
    }

    int nconns = w->clients > 0 ? w->clients : 1;
    edb_socket_t *socks = (edb_socket_t *)calloc((size_t)nconns, sizeof(edb_socket_t));
    for (int c = 0; c < nconns; c++) {
        socks[c] = edb_sock_connect_tcp(w->host, w->port, 5000);
    }

    edb_buf_t tx;
    edb_buf_init(&tx);
    int pipe = w->pipeline > 0 ? w->pipeline : 1;
    int done = 0;
    uint64_t seq = (uint64_t)w->tid * 10000000ULL;
    char sql[256];
    uint8_t *scratch = (uint8_t *)malloc(65536);

    while (done < w->requests) {
        for (int c = 0; c < nconns && done < w->requests; c++) {
            if (socks[c] == EDB_INVALID_SOCK) {
                w->err_ops += (uint64_t)pipe;
                done += pipe;
                continue;
            }
            int batch = w->requests - done;
            if (batch > pipe) batch = pipe;

            edb_buf_clear(&tx);
            for (int p = 0; p < batch; p++) {
                edb_bin_op_t op = EDB_OP_QUERY;
                make_query(w->wl, seq++, sql, sizeof(sql), &op);
                edb_bin_encode_frame(&tx, op, 0, sql, (uint32_t)strlen(sql));
            }

            uint64_t t0 = edb_now_us();
            if (edb_sock_send_all(socks[c], tx.data, tx.len) != 0) {
                w->err_ops += (uint64_t)batch;
                edb_sock_close(socks[c]);
                socks[c] = EDB_INVALID_SOCK;
                done += batch;
                continue;
            }

            for (int p = 0; p < batch; p++) {
                uint8_t hdr[EDB_BIN_HDR_SIZE];
                if (edb_sock_recv_exact(socks[c], hdr, EDB_BIN_HDR_SIZE) != 0) {
                    w->err_ops += (uint64_t)(batch - p);
                    edb_sock_close(socks[c]);
                    socks[c] = EDB_INVALID_SOCK;
                    break;
                }
                uint32_t rlen = bench_u32(hdr + 4);
                if (rlen > 0) {
                    if (rlen <= 65536) {
                        if (edb_sock_recv_exact(socks[c], scratch, rlen) != 0) { w->err_ops++; break; }
                    } else {
                        uint8_t *tmp = (uint8_t *)malloc(rlen);
                        if (edb_sock_recv_exact(socks[c], tmp, rlen) != 0) { free(tmp); w->err_ops++; break; }
                        free(tmp);
                    }
                }
                if (hdr[1] == EDB_OK) w->ok_ops++;
                else w->err_ops++;
            }
            uint64_t dt = edb_now_us() - t0;
            record_lat(w, dt / (uint64_t)batch);
            done += batch;
        }
    }

    free(scratch);
    edb_buf_free(&tx);
    for (int c = 0; c < nconns; c++) {
        if (socks[c] != EDB_INVALID_SOCK) edb_sock_close(socks[c]);
    }
    free(socks);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub);
}

static void seed_bench_tables_tcp(const char *host, uint16_t port) {
    edb_socket_t s = edb_sock_connect_tcp(host, port, 5000);
    if (s == EDB_INVALID_SOCK) return;
    edb_buf_t f;
    edb_buf_init(&f);
    const char *ddl1 = "CREATE TABLE IF NOT EXISTS bench_tbl (id INT PRIMARY KEY, val TEXT)";
    const char *ddl2 = "CREATE TABLE IF NOT EXISTS bench_write (id INT PRIMARY KEY AUTO_INCREMENT, k INT, v TEXT)";
    edb_bin_encode_frame(&f, EDB_OP_QUERY, 0, ddl1, (uint32_t)strlen(ddl1));
    edb_bin_encode_frame(&f, EDB_OP_QUERY, 0, ddl2, (uint32_t)strlen(ddl2));
    edb_sock_send_all(s, f.data, f.len);
    for (int i = 0; i < 2; i++) {
        uint8_t hdr[EDB_BIN_HDR_SIZE];
        if (edb_sock_recv_exact(s, hdr, EDB_BIN_HDR_SIZE) == 0) {
            uint32_t rl = bench_u32(hdr + 4);
            if (rl > 0) { uint8_t *t = (uint8_t *)malloc(rl); edb_sock_recv_exact(s, t, rl); free(t); }
        }
    }
    edb_buf_clear(&f);
    edb_buf_t ins;
    edb_buf_init(&ins);
    edb_buf_append_str(&ins, "INSERT INTO bench_tbl (id, val) VALUES ");
    for (int i = 1; i <= 1000; i++) {
        edb_buf_printf(&ins, "%s(%d, 'row_%d')", i > 1 ? "," : "", i, i);
    }
    edb_bin_encode_frame(&f, EDB_OP_QUERY, 0, ins.data, (uint32_t)ins.len);
    edb_sock_send_all(s, f.data, f.len);
    uint8_t hdr[EDB_BIN_HDR_SIZE];
    if (edb_sock_recv_exact(s, hdr, EDB_BIN_HDR_SIZE) == 0) {
        uint32_t rl = bench_u32(hdr + 4);
        if (rl > 0) { uint8_t *t = (uint8_t *)malloc(rl); edb_sock_recv_exact(s, t, rl); free(t); }
    }
    edb_buf_free(&ins);
    edb_buf_free(&f);
    edb_sock_close(s);
}

int edb_bench_main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    uint16_t port = 7432;
    int clients = 64;
    int threads = 8;
    int requests = 200000;
    int pipeline = 16;
    const char *wl_str = "ping";
    bool embedded = false;

    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) host = argv[++i];
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--clients") == 0) && i + 1 < argc) clients = atoi(argv[++i]);
        else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) && i + 1 < argc) threads = atoi(argv[++i]);
        else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--requests") == 0) && i + 1 < argc) requests = atoi(argv[++i]);
        else if (strcmp(argv[i], "--pipeline") == 0 && i + 1 < argc) pipeline = atoi(argv[++i]);
        else if (strcmp(argv[i], "--workload") == 0 && i + 1 < argc) wl_str = argv[++i];
        else if (strcmp(argv[i], "--embedded") == 0) embedded = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: elliottdb bench [-H host] [-p port] [-c clients] [-t threads] [-n requests] [--pipeline P] [--workload ping|read|point|write|mixed] [--embedded]\n");
            return 0;
        }
    }

    workload_t wl = WL_PING;
    if (edb_strcasecmp(wl_str, "read") == 0) wl = WL_READ;
    else if (edb_strcasecmp(wl_str, "point") == 0) wl = WL_POINT;
    else if (edb_strcasecmp(wl_str, "write") == 0) wl = WL_WRITE;
    else if (edb_strcasecmp(wl_str, "mixed") == 0) wl = WL_MIXED;

    if (threads < 1) threads = 1;
    if (clients < threads) clients = threads;

    edb_net_init();
    edb_config_t cfg;
    edb_metrics_t metrics;
    edb_db_t db;
    edb_engine_t eng;

    if (embedded) {
        edb_config_defaults(&cfg);
        cfg.in_memory = true;
        edb_config_finalize(&cfg, NULL, 0);
        edb_metrics_init(&metrics);
        edb_db_open(&db, &cfg, &metrics, NULL, 0);
        edb_engine_init(&eng, &db);
        edb_session_t s;
        edb_session_init(&s);
        edb_result_t r;
        const char *d1 = "CREATE TABLE bench_tbl (id INT PRIMARY KEY, val TEXT)";
        const char *d2 = "CREATE TABLE bench_write (id INT PRIMARY KEY AUTO_INCREMENT, k INT, v TEXT)";
        edb_execute_sql(&eng, &s, d1, strlen(d1), &r); edb_result_free(&r);
        edb_execute_sql(&eng, &s, d2, strlen(d2), &r); edb_result_free(&r);
        edb_buf_t ins;
        edb_buf_init(&ins);
        edb_buf_append_str(&ins, "INSERT INTO bench_tbl (id, val) VALUES ");
        for (int i = 1; i <= 1000; i++) edb_buf_printf(&ins, "%s(%d,'row_%d')", i > 1 ? "," : "", i, i);
        edb_execute_sql(&eng, &s, (const char *)ins.data, ins.len, &r); edb_result_free(&r);
        edb_buf_free(&ins);
        edb_session_destroy(&eng, &s);
    } else if (wl != WL_PING) {
        seed_bench_tables_tcp(host, port);
    }

    bench_worker_t *workers = (bench_worker_t *)calloc((size_t)threads, sizeof(bench_worker_t));
    edb_thread_t *tids = (edb_thread_t *)calloc((size_t)threads, sizeof(edb_thread_t));

    int req_per_t = requests / threads;
    int cli_per_t = clients / threads;

    printf("ElliottDB Benchmark: mode=%s workload=%s threads=%d clients=%d requests=%d pipeline=%d\n",
           embedded ? "embedded" : "tcp", wl_str, threads, clients, requests, pipeline);

    uint64_t t0 = edb_now_ns();
    for (int i = 0; i < threads; i++) {
        workers[i].tid = i;
        workers[i].host = host;
        workers[i].port = port;
        workers[i].clients = cli_per_t;
        workers[i].requests = (i == threads - 1) ? (requests - req_per_t * (threads - 1)) : req_per_t;
        workers[i].pipeline = pipeline;
        workers[i].wl = wl;
        workers[i].embedded = embedded;
        workers[i].eng = &eng;
        edb_thread_create(&tids[i], bench_worker_fn, &workers[i]);
    }
    for (int i = 0; i < threads; i++) edb_thread_join(tids[i]);
    uint64_t dt_ns = edb_now_ns() - t0;

    uint64_t total_ok = 0, total_err = 0;
    size_t total_lats = 0;
    for (int i = 0; i < threads; i++) {
        total_ok += workers[i].ok_ops;
        total_err += workers[i].err_ops;
        total_lats += workers[i].lat_count;
    }

    uint64_t *all_lats = total_lats > 0 ? (uint64_t *)malloc(total_lats * sizeof(uint64_t)) : NULL;
    size_t pos = 0;
    for (int i = 0; i < threads; i++) {
        if (workers[i].lat_count > 0) {
            memcpy(all_lats + pos, workers[i].latencies_us, workers[i].lat_count * sizeof(uint64_t));
            pos += workers[i].lat_count;
        }
        free(workers[i].latencies_us);
    }
    if (all_lats && total_lats > 1) qsort(all_lats, total_lats, sizeof(uint64_t), cmp_u64);

    double sec = (double)dt_ns / 1000000000.0;
    double qps = sec > 0.0001 ? ((double)total_ok / sec) : 0.0;
    uint64_t p50 = all_lats && total_lats > 0 ? all_lats[(size_t)(total_lats * 0.50)] : 0;
    uint64_t p95 = all_lats && total_lats > 0 ? all_lats[(size_t)(total_lats * 0.95)] : 0;
    uint64_t p99 = all_lats && total_lats > 0 ? all_lats[(size_t)(total_lats * 0.99)] : 0;

    printf("Results: ok=%llu err=%llu elapsed=%.3fs throughput=%.0f req/s | lat p50=%lluus p95=%lluus p99=%lluus\n",
           (unsigned long long)total_ok, (unsigned long long)total_err, sec, qps,
           (unsigned long long)p50, (unsigned long long)p95, (unsigned long long)p99);

    free(all_lats);
    free(workers);
    free(tids);
    if (embedded) {
        edb_engine_destroy(&eng);
        edb_db_close(&db);
    }
    edb_net_cleanup();
    return total_err == 0 ? 0 : 1;
}
