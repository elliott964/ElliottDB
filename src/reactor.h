#ifndef EDB_REACTOR_H
#define EDB_REACTOR_H

#include "config.h"
#include "metrics.h"
#include "protocol.h"

typedef struct edb_reactor edb_reactor_t;

typedef void (*edb_on_data_fn)(void *server_ctx, edb_conn_t *conn);
typedef void (*edb_on_close_fn)(void *server_ctx, edb_conn_t *conn);

typedef struct {
    const edb_config_t *cfg;
    edb_metrics_t *metrics;
    void *server_ctx;
    edb_on_data_fn on_data;
    edb_on_close_fn on_close;
} edb_reactor_opts_t;

edb_reactor_t *edb_reactor_create(const edb_reactor_opts_t *opts, char *err, size_t err_cap);
bool edb_reactor_start(edb_reactor_t *r);
void edb_reactor_stop(edb_reactor_t *r);
void edb_reactor_destroy(edb_reactor_t *r);

/* Thread-safe RX/TX handoff between reactor I/O threads and worker threads */
void edb_reactor_pull_rx(edb_reactor_t *r, edb_conn_t *conn);
bool edb_reactor_has_rx(edb_reactor_t *r, edb_conn_t *conn);
void edb_reactor_push_tx(edb_reactor_t *r, edb_conn_t *conn, bool close_after_write);

void edb_reactor_conn_retain(edb_conn_t *conn);
void edb_reactor_conn_release(edb_reactor_t *r, edb_conn_t *conn);
void edb_reactor_reap_idle(edb_reactor_t *r, uint64_t idle_timeout_ms);

#endif /* EDB_REACTOR_H */
