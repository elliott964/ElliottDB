#include "server.h"

static bool work_queue_push(edb_work_queue_t *q, edb_conn_t *conn, const char *policy, uint32_t max_wait_ms) {
    edb_mutex_lock(&q->mu);
    if (q->count >= q->cap) {
        if (edb_strcasecmp(policy, "shed") == 0) {
            edb_mutex_unlock(&q->mu);
            return false;
        }
        /* backpressure / queue: wait briefly if full */
        uint64_t deadline = edb_now_ms() + max_wait_ms;
        while (q->count >= q->cap && edb_now_ms() < deadline) {
            edb_mutex_unlock(&q->mu);
            edb_sleep_ms(1);
            edb_mutex_lock(&q->mu);
        }
        if (q->count >= q->cap) {
            edb_mutex_unlock(&q->mu);
            return false;
        }
    }
    q->items[q->tail] = conn;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    edb_cond_signal(&q->not_empty);
    edb_mutex_unlock(&q->mu);
    return true;
}

static edb_conn_t *work_queue_pop(edb_work_queue_t *q, const edb_atomic_u32 *stopping) {
    edb_mutex_lock(&q->mu);
    if (q->count == 0 && !edb_atomic_load_u32(stopping)) {
        edb_cond_timedwait_ms(&q->not_empty, &q->mu, 5);
    }
    if (q->count == 0) {
        edb_mutex_unlock(&q->mu);
        return NULL;
    }
    edb_conn_t *c = q->items[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    edb_mutex_unlock(&q->mu);
    return c;
}

static void on_reactor_data(void *server_ctx, edb_conn_t *conn) {
    edb_server_t *srv = (edb_server_t *)server_ctx;
    if (edb_atomic_load_u32(&srv->stopping)) return;
    if (!edb_atomic_cas_u32(&conn->worker_busy, 0, 1)) {
        return; /* Active worker will pick up newly buffered bytes */
    }
    edb_reactor_conn_retain(conn);
    size_t shard = (size_t)(conn->id % srv->queue_shards);
    if (!work_queue_push(&srv->queues[shard], conn,
                         srv->cfg.overload_policy,
                         (uint32_t)srv->cfg.max_queue_wait_ms)) {
        edb_atomic_store_u32(&conn->worker_busy, 0);
        edb_reactor_conn_release(srv->reactor, conn);
    }
}

static void on_reactor_close(void *server_ctx, edb_conn_t *conn) {
    edb_server_t *srv = (edb_server_t *)server_ctx;
    edb_session_destroy(&srv->engine, &conn->sess);
}

typedef struct {
    edb_server_t *srv;
    size_t worker_id;
} worker_arg_t;

static void worker_thread_fn(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    edb_server_t *srv = wa->srv;
    size_t wid = wa->worker_id;
    free(wa);

    size_t home_shard = wid % srv->queue_shards;
    while (!edb_atomic_load_u32(&srv->stopping)) {
        edb_conn_t *conn = NULL;
        /* Work stealing across shards before sleeping on home shard */
        for (size_t off = 0; off < srv->queue_shards; off++) {
            size_t s = (home_shard + off) % srv->queue_shards;
            edb_work_queue_t *q = &srv->queues[s];
            if (q->count > 0) {
                edb_mutex_lock(&q->mu);
                if (q->count > 0) {
                    conn = q->items[q->head];
                    q->head = (q->head + 1) % q->cap;
                    q->count--;
                }
                edb_mutex_unlock(&q->mu);
                if (conn) break;
            }
        }
        if (!conn) {
            conn = work_queue_pop(&srv->queues[home_shard], &srv->stopping);
        }
        if (!conn) continue;

        edb_atomic_add_u64(&srv->active_tasks, 1);
        while (!edb_atomic_load_u32(&conn->closing)) {
            edb_reactor_pull_rx(srv->reactor, conn);
            bool close_after = false;
            int consumed = edb_protocol_process(&srv->engine, conn, &close_after);
            if (consumed < 0) close_after = true;
            if (conn->wbuf.len > 0 || close_after) {
                edb_reactor_push_tx(srv->reactor, conn, close_after);
            }
            if (close_after) break;

            if (edb_reactor_has_rx(srv->reactor, conn)) continue;
            edb_atomic_store_u32(&conn->worker_busy, 0);
            /* Double-check to prevent missed wakeup */
            if (edb_reactor_has_rx(srv->reactor, conn) &&
                edb_atomic_cas_u32(&conn->worker_busy, 0, 1)) {
                continue;
            }
            break;
        }
        if (edb_atomic_load_u32(&conn->closing)) {
            edb_atomic_store_u32(&conn->worker_busy, 0);
        }
        edb_atomic_add_u64(&srv->active_tasks, (uint64_t)-1);
        edb_reactor_conn_release(srv->reactor, conn);
    }
}

static void maint_thread_fn(void *arg) {
    edb_server_t *srv = (edb_server_t *)arg;
    uint64_t last_ckpt_ms = edb_now_ms();
    uint64_t last_metrics_ms = last_ckpt_ms;
    uint64_t last_flush_ms = last_ckpt_ms;

    while (!edb_atomic_load_u32(&srv->stopping)) {
        edb_sleep_ms(250);
        if (edb_atomic_load_u32(&srv->stopping)) break;
        uint64_t now = edb_now_ms();

        /* Background dirty page flush */
        if (srv->cfg.bg_flush_interval_ms > 0 &&
            now - last_flush_ms >= (uint64_t)srv->cfg.bg_flush_interval_ms) {
            edb_pager_flush_all(&srv->db.pager, false);
            last_flush_ms = now;
        }

        /* Periodic or size-triggered WAL checkpoint */
        uint64_t wal_bytes = edb_wal_size_bytes(&srv->db.wal);
        uint64_t wal_limit = (uint64_t)srv->cfg.wal_checkpoint_mb * 1024ULL * 1024ULL;
        bool time_ckpt = (now - last_ckpt_ms >= (uint64_t)srv->cfg.wal_checkpoint_interval_sec * 1000ULL);
        if (time_ckpt || (wal_limit > 0 && wal_bytes >= wal_limit)) {
            edb_db_checkpoint(&srv->db);
            last_ckpt_ms = now;
        }

        /* Idle connection reaping */
        if (srv->cfg.idle_timeout_sec > 0) {
            edb_reactor_reap_idle(srv->reactor, (uint64_t)srv->cfg.idle_timeout_sec * 1000ULL);
        }

        /* Optional periodic metrics log */
        if (srv->cfg.metrics_interval_sec > 0 &&
            now - last_metrics_ms >= (uint64_t)srv->cfg.metrics_interval_sec * 1000ULL) {
            edb_buf_t mb;
            edb_buf_init(&mb);
            edb_metrics_dump_json(&srv->metrics, srv->cfg.node_name, &mb);
            EDB_LOG_I("METRICS %s", mb.data ? (const char *)mb.data : "{}");
            edb_buf_free(&mb);
            last_metrics_ms = now;
        }
    }
}

bool edb_server_init(edb_server_t *srv, const edb_config_t *cfg, char *err, size_t err_cap) {
    memset(srv, 0, sizeof(*srv));
    srv->cfg = *cfg;
    edb_metrics_init(&srv->metrics);

    if (edb_db_open(&srv->db, &srv->cfg, &srv->metrics, err, err_cap) != EDB_OK) {
        return false;
    }
    edb_engine_init(&srv->engine, &srv->db);

    srv->worker_count = (size_t)(srv->cfg.worker_threads > 0 ? srv->cfg.worker_threads : 4);
    srv->queue_shards = (size_t)srv->cfg.queue_shards;
    if (srv->queue_shards == 0) srv->queue_shards = 16;
    if (srv->queue_shards > srv->worker_count) srv->queue_shards = srv->worker_count;
    srv->queue_mask = srv->queue_shards - 1;
    size_t per_q = (size_t)srv->cfg.queue_capacity / srv->queue_shards;
    if (per_q < 16) per_q = 16;

    srv->queues = (edb_work_queue_t *)calloc(srv->queue_shards, sizeof(edb_work_queue_t));
    for (size_t i = 0; i < srv->queue_shards; i++) {
        srv->queues[i].cap = per_q;
        srv->queues[i].items = (edb_conn_t **)calloc(per_q, sizeof(edb_conn_t *));
        edb_mutex_init(&srv->queues[i].mu);
        edb_cond_init(&srv->queues[i].not_empty);
    }

    edb_reactor_opts_t ropts;
    memset(&ropts, 0, sizeof(ropts));
    ropts.cfg = &srv->cfg;
    ropts.metrics = &srv->metrics;
    ropts.server_ctx = srv;
    ropts.on_data = on_reactor_data;
    ropts.on_close = on_reactor_close;

    srv->reactor = edb_reactor_create(&ropts, err, err_cap);
    if (!srv->reactor) {
        edb_server_destroy(srv);
        return false;
    }
    return true;
}

bool edb_server_start(edb_server_t *srv, char *err, size_t err_cap) {
    (void)err;
    (void)err_cap;
    srv->workers = (edb_thread_t *)calloc(srv->worker_count, sizeof(edb_thread_t));
    for (size_t i = 0; i < srv->worker_count; i++) {
        worker_arg_t *wa = (worker_arg_t *)calloc(1, sizeof(worker_arg_t));
        wa->srv = srv;
        wa->worker_id = i;
        edb_thread_create(&srv->workers[i], worker_thread_fn, wa);
    }

    srv->maint_started = edb_thread_create(&srv->maint_thread, maint_thread_fn, srv);
    return edb_reactor_start(srv->reactor);
}

void edb_server_request_stop(edb_server_t *srv) {
    if (!edb_atomic_cas_u32(&srv->stopping, 0, 1)) return;
    for (size_t i = 0; i < srv->queue_shards; i++) {
        edb_mutex_lock(&srv->queues[i].mu);
        edb_cond_broadcast(&srv->queues[i].not_empty);
        edb_mutex_unlock(&srv->queues[i].mu);
    }
}

void edb_server_wait_stop(edb_server_t *srv) {
    edb_server_request_stop(srv);
    if (srv->reactor) edb_reactor_stop(srv->reactor);

    uint64_t deadline = edb_now_ms() + (uint64_t)srv->cfg.shutdown_timeout_ms;
    while (edb_atomic_load_u64(&srv->active_tasks) > 0 && edb_now_ms() < deadline) {
        edb_sleep_ms(5);
    }

    for (size_t i = 0; i < srv->worker_count; i++) {
        edb_thread_join(srv->workers[i]);
    }
    if (srv->maint_started) {
        edb_thread_join(srv->maint_thread);
        srv->maint_started = false;
    }
}

void edb_server_destroy(edb_server_t *srv) {
    if (!srv) return;
    edb_server_wait_stop(srv);
    if (srv->reactor) {
        edb_reactor_destroy(srv->reactor);
        srv->reactor = NULL;
    }
    free(srv->workers);
    srv->workers = NULL;

    if (srv->queues) {
        for (size_t i = 0; i < srv->queue_shards; i++) {
            free(srv->queues[i].items);
            edb_cond_destroy(&srv->queues[i].not_empty);
            edb_mutex_destroy(&srv->queues[i].mu);
        }
        free(srv->queues);
        srv->queues = NULL;
    }

    edb_engine_destroy(&srv->engine);
    edb_db_close(&srv->db);
}
