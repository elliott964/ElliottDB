#ifndef EDB_SERVER_H
#define EDB_SERVER_H

#include "reactor.h"
#include "sql_exec.h"

typedef struct {
    edb_conn_t **items;
    size_t head;
    size_t tail;
    size_t count;
    size_t cap;
    edb_mutex_t mu;
    edb_cond_t not_empty;
} edb_work_queue_t;

typedef struct edb_server {
    edb_config_t cfg;
    edb_metrics_t metrics;
    edb_db_t db;
    edb_engine_t engine;
    edb_reactor_t *reactor;

    edb_work_queue_t *queues;
    size_t queue_shards;
    size_t queue_mask;
    edb_atomic_u64 rr_dispatch;

    edb_thread_t *workers;
    size_t worker_count;
    edb_thread_t maint_thread;
    bool maint_started;

    edb_atomic_u32 stopping;
    edb_atomic_u64 active_tasks;
} edb_server_t;

bool edb_server_init(edb_server_t *srv, const edb_config_t *cfg, char *err, size_t err_cap);
bool edb_server_start(edb_server_t *srv, char *err, size_t err_cap);
void edb_server_request_stop(edb_server_t *srv);
void edb_server_wait_stop(edb_server_t *srv);
void edb_server_destroy(edb_server_t *srv);

#endif /* EDB_SERVER_H */
