#ifdef _WIN32

#include "reactor.h"

#define IOCP_OP_RECV 1
#define IOCP_OP_SEND 2
#define IOCP_OP_KICK_SEND 3

typedef struct {
    OVERLAPPED ov;
    int op;
    WSABUF wsabuf;
    char buf[16384];
} win_io_op_t;

typedef struct win_conn_ctx {
    edb_conn_t conn;
    edb_mutex_t io_mu;
    edb_buf_t rx_buf;
    edb_buf_t tx_buf;
    edb_buf_t tx_active;
    size_t tx_pos;
    bool close_after_tx;
    bool recv_pending;
    bool send_pending;
    win_io_op_t recv_op;
    win_io_op_t send_op;
    struct win_conn_ctx *slab_next;
    struct win_conn_ctx *active_next;
    struct win_conn_ctx *active_prev;
    bool in_active_list;
} win_conn_ctx_t;

typedef struct win_slab_chunk {
    win_conn_ctx_t *items;
    size_t count;
    struct win_slab_chunk *next;
} win_slab_chunk_t;

struct edb_reactor {
    edb_reactor_opts_t opts;
    HANDLE iocp;
    edb_socket_t listen_sock;
    edb_socket_t http_sock;
    edb_thread_t *io_threads;
    size_t io_thread_count;
    edb_thread_t accept_thread;
    edb_thread_t http_accept_thread;
    bool accept_started;
    bool http_accept_started;
    edb_atomic_u32 running;
    edb_atomic_u64 next_conn_id;

    edb_mutex_t pool_mu;
    win_slab_chunk_t *chunks;
    win_conn_ctx_t *free_list;
    win_conn_ctx_t *active_head;
};

static win_conn_ctx_t *alloc_conn(edb_reactor_t *r) {
    edb_mutex_lock(&r->pool_mu);
    if (!r->free_list) {
        size_t chunk_sz = (size_t)r->opts.cfg->conn_slab_chunk;
        if (chunk_sz < 64) chunk_sz = 64;
        win_slab_chunk_t *ch = (win_slab_chunk_t *)calloc(1, sizeof(win_slab_chunk_t));
        ch->items = (win_conn_ctx_t *)calloc(chunk_sz, sizeof(win_conn_ctx_t));
        ch->count = chunk_sz;
        for (size_t i = 0; i < chunk_sz; i++) {
            edb_mutex_init(&ch->items[i].io_mu);
            edb_buf_init(&ch->items[i].rx_buf);
            edb_buf_init(&ch->items[i].tx_buf);
            edb_buf_init(&ch->items[i].tx_active);
            edb_buf_init(&ch->items[i].conn.rbuf);
            edb_buf_init(&ch->items[i].conn.wbuf);
            ch->items[i].slab_next = r->free_list;
            r->free_list = &ch->items[i];
        }
        ch->next = r->chunks;
        r->chunks = ch;
    }
    win_conn_ctx_t *ctx = r->free_list;
    r->free_list = ctx->slab_next;
    ctx->slab_next = NULL;
    edb_mutex_unlock(&r->pool_mu);

    memset(&ctx->conn, 0, sizeof(ctx->conn));
    ctx->conn.sock = EDB_INVALID_SOCK;
    ctx->conn.reactor_ctx = ctx;
    ctx->conn.id = edb_atomic_add_u64(&r->next_conn_id, 1);
    ctx->conn.last_active_ms = edb_now_ms();
    edb_atomic_store_u32(&ctx->conn.refcount, 1);
    edb_session_init(&ctx->conn.sess);

    edb_buf_clear(&ctx->rx_buf);
    edb_buf_clear(&ctx->tx_buf);
    edb_buf_clear(&ctx->tx_active);
    ctx->tx_pos = 0;
    ctx->close_after_tx = false;
    ctx->recv_pending = false;
    ctx->send_pending = false;

    /* Add to active list only after all fields including last_active_ms are initialized */
    edb_mutex_lock(&r->pool_mu);
    ctx->active_prev = NULL;
    ctx->active_next = r->active_head;
    if (r->active_head) r->active_head->active_prev = ctx;
    r->active_head = ctx;
    ctx->in_active_list = true;
    edb_mutex_unlock(&r->pool_mu);
    return ctx;
}

static void free_conn_final(edb_reactor_t *r, win_conn_ctx_t *ctx) {
    if (ctx->conn.sock != EDB_INVALID_SOCK) {
        edb_sock_close(ctx->conn.sock);
        ctx->conn.sock = EDB_INVALID_SOCK;
        if (r->opts.metrics) edb_atomic_add_i64(&r->opts.metrics->active_connections, -1);
    }
    if (r->opts.on_close) r->opts.on_close(r->opts.server_ctx, &ctx->conn);

    edb_buf_free(&ctx->conn.rbuf);
    edb_buf_free(&ctx->conn.wbuf);
    edb_buf_free(&ctx->rx_buf);
    edb_buf_free(&ctx->tx_buf);
    edb_buf_free(&ctx->tx_active);

    edb_mutex_lock(&r->pool_mu);
    if (ctx->in_active_list) {
        if (ctx->active_prev) ctx->active_prev->active_next = ctx->active_next;
        else r->active_head = ctx->active_next;
        if (ctx->active_next) ctx->active_next->active_prev = ctx->active_prev;
        ctx->in_active_list = false;
    }
    ctx->slab_next = r->free_list;
    r->free_list = ctx;
    edb_mutex_unlock(&r->pool_mu);
}

void edb_reactor_conn_retain(edb_conn_t *conn) {
    edb_atomic_add_u32(&conn->refcount, 1);
}

void edb_reactor_conn_release(edb_reactor_t *r, edb_conn_t *conn) {
    uint32_t rem = edb_atomic_add_u32(&conn->refcount, (uint32_t)-1);
    if (rem == 0) {
        free_conn_final(r, (win_conn_ctx_t *)conn->reactor_ctx);
    }
}

static void close_conn_socket(edb_reactor_t *r, win_conn_ctx_t *ctx) {
    edb_socket_t sock = EDB_INVALID_SOCK;
    bool did_close = false;
    edb_mutex_lock(&ctx->io_mu);
    if (edb_atomic_cas_u32(&ctx->conn.closing, 0, 1)) {
        did_close = true;
        sock = ctx->conn.sock;
        ctx->conn.sock = EDB_INVALID_SOCK;
    }
    edb_mutex_unlock(&ctx->io_mu);

    if (did_close) {
        if (sock != EDB_INVALID_SOCK) {
            shutdown(sock, SD_SEND);
            edb_sock_close(sock);
            if (r->opts.metrics) edb_atomic_add_i64(&r->opts.metrics->active_connections, -1);
        }
        /* Drop initial accept reference */
        edb_reactor_conn_release(r, &ctx->conn);
    }
}

static bool post_recv(edb_reactor_t *r, win_conn_ctx_t *ctx) {
    edb_mutex_lock(&ctx->io_mu);
    if (edb_atomic_load_u32(&ctx->conn.closing) || ctx->conn.sock == EDB_INVALID_SOCK) {
        edb_mutex_unlock(&ctx->io_mu);
        return false;
    }
    memset(&ctx->recv_op.ov, 0, sizeof(ctx->recv_op.ov));
    ctx->recv_op.op = IOCP_OP_RECV;
    ctx->recv_op.wsabuf.buf = ctx->recv_op.buf;
    ctx->recv_op.wsabuf.len = sizeof(ctx->recv_op.buf);
    DWORD flags = 0;
    edb_reactor_conn_retain(&ctx->conn);
    int rc = WSARecv(ctx->conn.sock, &ctx->recv_op.wsabuf, 1, NULL, &flags, &ctx->recv_op.ov, NULL);
    int werr = (rc == SOCKET_ERROR) ? WSAGetLastError() : 0;
    edb_mutex_unlock(&ctx->io_mu);

    if (rc == SOCKET_ERROR && werr != WSA_IO_PENDING) {
        edb_reactor_conn_release(r, &ctx->conn);
        close_conn_socket(r, ctx);
        return false;
    }
    return true;
}

static bool try_start_send_locked(win_conn_ctx_t *ctx, bool *out_send_failed) {
    *out_send_failed = false;
    if (ctx->send_pending || edb_atomic_load_u32(&ctx->conn.closing) || ctx->conn.sock == EDB_INVALID_SOCK) {
        return false;
    }
    if (ctx->tx_pos >= ctx->tx_active.len) {
        edb_buf_clear(&ctx->tx_active);
        ctx->tx_pos = 0;
        if (ctx->tx_buf.len == 0) {
            return ctx->close_after_tx;
        }
        /* Swap tx_buf -> tx_active so worker appends to tx_buf never realloc an in-flight WSASend buffer */
        edb_buf_t tmp = ctx->tx_active;
        ctx->tx_active = ctx->tx_buf;
        ctx->tx_buf = tmp;
    }
    size_t rem = ctx->tx_active.len - ctx->tx_pos;
    DWORD chunk = (DWORD)(rem > 65536 ? 65536 : rem);
    memset(&ctx->send_op.ov, 0, sizeof(ctx->send_op.ov));
    ctx->send_op.op = IOCP_OP_SEND;
    ctx->send_op.wsabuf.buf = (CHAR *)(ctx->tx_active.data + ctx->tx_pos);
    ctx->send_op.wsabuf.len = chunk;
    ctx->send_pending = true;

    edb_reactor_conn_retain(&ctx->conn);
    int rc = WSASend(ctx->conn.sock, &ctx->send_op.wsabuf, 1, NULL, 0, &ctx->send_op.ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        ctx->send_pending = false;
        *out_send_failed = true;
        return true;
    }
    return false;
}

void edb_reactor_pull_rx(edb_reactor_t *r, edb_conn_t *conn) {
    (void)r;
    win_conn_ctx_t *ctx = (win_conn_ctx_t *)conn->reactor_ctx;
    edb_mutex_lock(&ctx->io_mu);
    if (ctx->rx_buf.len > 0) {
        edb_buf_append(&conn->rbuf, ctx->rx_buf.data, ctx->rx_buf.len);
        edb_buf_clear(&ctx->rx_buf);
    }
    edb_mutex_unlock(&ctx->io_mu);
}

bool edb_reactor_has_rx(edb_reactor_t *r, edb_conn_t *conn) {
    (void)r;
    win_conn_ctx_t *ctx = (win_conn_ctx_t *)conn->reactor_ctx;
    edb_mutex_lock(&ctx->io_mu);
    bool has = ctx->rx_buf.len > 0;
    edb_mutex_unlock(&ctx->io_mu);
    return has;
}

void edb_reactor_push_tx(edb_reactor_t *r, edb_conn_t *conn, bool close_after_write) {
    win_conn_ctx_t *ctx = (win_conn_ctx_t *)conn->reactor_ctx;
    bool need_kick = false;
    edb_mutex_lock(&ctx->io_mu);
    if (conn->wbuf.len > 0) {
        edb_buf_append(&ctx->tx_buf, conn->wbuf.data, conn->wbuf.len);
        edb_buf_clear(&conn->wbuf);
    }
    if (close_after_write) ctx->close_after_tx = true;
    if (!ctx->send_pending && (ctx->tx_pos < ctx->tx_active.len || ctx->tx_buf.len > 0 || ctx->close_after_tx)) {
        need_kick = true;
    }
    edb_mutex_unlock(&ctx->io_mu);

    if (need_kick && !edb_atomic_load_u32(&conn->closing)) {
        win_io_op_t *kick = (win_io_op_t *)calloc(1, sizeof(win_io_op_t));
        if (kick) {
            kick->op = IOCP_OP_KICK_SEND;
            edb_reactor_conn_retain(conn);
            if (!PostQueuedCompletionStatus(r->iocp, 0, (ULONG_PTR)ctx, &kick->ov)) {
                free(kick);
                edb_reactor_conn_release(r, conn);
            }
        }
    }
}

static void iocp_worker_fn(void *arg) {
    edb_reactor_t *r = (edb_reactor_t *)arg;
    while (true) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(r->iocp, &bytes, &key, &ov, INFINITE);
        if (key == 0 && ov == NULL) break; /* shutdown sentinel */

        win_conn_ctx_t *ctx = (win_conn_ctx_t *)key;
        win_io_op_t *iop = CONTAINING_RECORD(ov, win_io_op_t, ov);

        if (iop->op == IOCP_OP_KICK_SEND) {
            free(iop);
            bool send_failed = false;
            edb_mutex_lock(&ctx->io_mu);
            bool need_close = try_start_send_locked(ctx, &send_failed);
            edb_mutex_unlock(&ctx->io_mu);
            if (send_failed) edb_reactor_conn_release(r, &ctx->conn);
            if (need_close) close_conn_socket(r, ctx);
            edb_reactor_conn_release(r, &ctx->conn);
            continue;
        }

        if (!ok || (bytes == 0 && iop->op == IOCP_OP_RECV)) {
            if (iop->op == IOCP_OP_SEND) {
                edb_mutex_lock(&ctx->io_mu);
                ctx->send_pending = false;
                edb_mutex_unlock(&ctx->io_mu);
            }
            close_conn_socket(r, ctx);
            edb_reactor_conn_release(r, &ctx->conn);
            continue;
        }

        if (iop->op == IOCP_OP_RECV) {
            if (r->opts.metrics) edb_atomic_add_u64(&r->opts.metrics->bytes_in, bytes);
            bool overflow = false;
            size_t max_rb = (size_t)r->opts.cfg->read_buf_max_mb * 1024ULL * 1024ULL;
            edb_mutex_lock(&ctx->io_mu);
            if (ctx->rx_buf.len + bytes > max_rb) {
                overflow = true;
            } else {
                edb_buf_append(&ctx->rx_buf, iop->buf, bytes);
                ctx->conn.last_active_ms = edb_now_ms();
            }
            edb_mutex_unlock(&ctx->io_mu);

            if (overflow) {
                close_conn_socket(r, ctx);
            } else {
                r->opts.on_data(r->opts.server_ctx, &ctx->conn);
                post_recv(r, ctx);
            }
            edb_reactor_conn_release(r, &ctx->conn);
        } else if (iop->op == IOCP_OP_SEND) {
            if (r->opts.metrics) edb_atomic_add_u64(&r->opts.metrics->bytes_out, bytes);
            bool send_failed = false;
            edb_mutex_lock(&ctx->io_mu);
            ctx->send_pending = false;
            ctx->tx_pos += bytes;
            bool need_close = try_start_send_locked(ctx, &send_failed);
            edb_mutex_unlock(&ctx->io_mu);
            if (send_failed) edb_reactor_conn_release(r, &ctx->conn);
            if (need_close) close_conn_socket(r, ctx);
            edb_reactor_conn_release(r, &ctx->conn);
        }
    }
}

typedef struct {
    edb_reactor_t *r;
    edb_socket_t sock;
    bool is_http;
} accept_arg_t;

static void accept_loop_fn(void *arg) {
    accept_arg_t *aa = (accept_arg_t *)arg;
    edb_reactor_t *r = aa->r;
    edb_socket_t lsock = aa->sock;
    bool is_http = aa->is_http;
    free(aa);

    while (edb_atomic_load_u32(&r->running)) {
        struct sockaddr_storage ss;
        int slen = sizeof(ss);
        edb_socket_t cs = accept(lsock, (struct sockaddr *)&ss, &slen);
        if (cs == EDB_INVALID_SOCK) {
            if (!edb_atomic_load_u32(&r->running)) break;
            edb_sleep_ms(1);
            continue;
        }
        int64_t maxc = r->opts.cfg->max_connections;
        if (maxc > 0 && r->opts.metrics &&
            edb_atomic_load_i64(&r->opts.metrics->active_connections) >= maxc) {
            edb_atomic_add_u64(&r->opts.metrics->rejected_connections, 1);
            edb_sock_close(cs);
            continue;
        }

        edb_sock_set_nodelay(cs, r->opts.cfg->tcp_nodelay);
        edb_sock_set_keepalive(cs, r->opts.cfg->tcp_keepalive,
                               (int)r->opts.cfg->keepalive_idle_sec,
                               (int)r->opts.cfg->keepalive_interval_sec);
        edb_sock_set_bufs(cs, (int)r->opts.cfg->socket_recv_buf_kb, (int)r->opts.cfg->socket_send_buf_kb);

        win_conn_ctx_t *ctx = alloc_conn(r);
        ctx->conn.sock = cs;
        if (is_http) ctx->conn.proto = PROTO_HTTP;

        if (r->opts.metrics) {
            edb_atomic_add_i64(&r->opts.metrics->active_connections, 1);
            edb_atomic_add_u64(&r->opts.metrics->total_connections, 1);
        }

        if (!CreateIoCompletionPort((HANDLE)cs, r->iocp, (ULONG_PTR)ctx, 0)) {
            close_conn_socket(r, ctx);
            continue;
        }
        post_recv(r, ctx);
    }
}

static edb_socket_t bind_listen_socket(const char *host, uint16_t port, int backlog, char *err, size_t err_cap) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char pstr[16];
    snprintf(pstr, sizeof(pstr), "%u", (unsigned)port);
    if (getaddrinfo(host, pstr, &hints, &res) != 0 || !res) {
        if (err) snprintf(err, err_cap, "getaddrinfo failed for %s:%u", host, (unsigned)port);
        return EDB_INVALID_SOCK;
    }
    edb_socket_t s = WSASocketW(res->ai_family, res->ai_socktype, res->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s == EDB_INVALID_SOCK) {
        freeaddrinfo(res);
        if (err) snprintf(err, err_cap, "WSASocket failed");
        return EDB_INVALID_SOCK;
    }
    edb_sock_set_reuseaddr(s, true);
    if (bind(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        edb_sock_close(s);
        if (err) snprintf(err, err_cap, "Failed to bind %s:%u (err=%d)", host, (unsigned)port, WSAGetLastError());
        return EDB_INVALID_SOCK;
    }
    freeaddrinfo(res);
    if (listen(s, backlog) != 0) {
        edb_sock_close(s);
        if (err) snprintf(err, err_cap, "listen() failed on port %u", (unsigned)port);
        return EDB_INVALID_SOCK;
    }
    return s;
}

edb_reactor_t *edb_reactor_create(const edb_reactor_opts_t *opts, char *err, size_t err_cap) {
    edb_net_init();
    edb_reactor_t *r = (edb_reactor_t *)calloc(1, sizeof(edb_reactor_t));
    if (!r) return NULL;
    r->opts = *opts;
    r->listen_sock = EDB_INVALID_SOCK;
    r->http_sock = EDB_INVALID_SOCK;
    edb_mutex_init(&r->pool_mu);
    edb_atomic_store_u64(&r->next_conn_id, 1);

    r->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, (DWORD)opts->cfg->io_threads);
    if (!r->iocp) {
        if (err) snprintf(err, err_cap, "CreateIoCompletionPort failed");
        free(r);
        return NULL;
    }

    r->listen_sock = bind_listen_socket(opts->cfg->host, (uint16_t)opts->cfg->port,
                                        (int)opts->cfg->listen_backlog, err, err_cap);
    if (r->listen_sock == EDB_INVALID_SOCK) {
        CloseHandle(r->iocp);
        free(r);
        return NULL;
    }

    if (opts->cfg->http_port > 0 && opts->cfg->http_port != opts->cfg->port) {
        r->http_sock = bind_listen_socket(opts->cfg->host, (uint16_t)opts->cfg->http_port,
                                          (int)opts->cfg->listen_backlog, err, err_cap);
        if (r->http_sock == EDB_INVALID_SOCK) {
            edb_sock_close(r->listen_sock);
            CloseHandle(r->iocp);
            free(r);
            return NULL;
        }
    }

    r->io_thread_count = (size_t)opts->cfg->io_threads;
    if (r->io_thread_count < 1) r->io_thread_count = 2;
    r->io_threads = (edb_thread_t *)calloc(r->io_thread_count, sizeof(edb_thread_t));
    return r;
}

bool edb_reactor_start(edb_reactor_t *r) {
    edb_atomic_store_u32(&r->running, 1);
    for (size_t i = 0; i < r->io_thread_count; i++) {
        edb_thread_create(&r->io_threads[i], iocp_worker_fn, r);
    }
    accept_arg_t *a1 = (accept_arg_t *)calloc(1, sizeof(accept_arg_t));
    a1->r = r; a1->sock = r->listen_sock; a1->is_http = false;
    r->accept_started = edb_thread_create(&r->accept_thread, accept_loop_fn, a1);

    if (r->http_sock != EDB_INVALID_SOCK) {
        accept_arg_t *a2 = (accept_arg_t *)calloc(1, sizeof(accept_arg_t));
        a2->r = r; a2->sock = r->http_sock; a2->is_http = true;
        r->http_accept_started = edb_thread_create(&r->http_accept_thread, accept_loop_fn, a2);
    }
    return true;
}

void edb_reactor_stop(edb_reactor_t *r) {
    if (!edb_atomic_cas_u32(&r->running, 1, 0)) return;
    if (r->listen_sock != EDB_INVALID_SOCK) {
        edb_sock_close(r->listen_sock);
        r->listen_sock = EDB_INVALID_SOCK;
    }
    if (r->http_sock != EDB_INVALID_SOCK) {
        edb_sock_close(r->http_sock);
        r->http_sock = EDB_INVALID_SOCK;
    }
    if (r->accept_started) edb_thread_join(r->accept_thread);
    if (r->http_accept_started) edb_thread_join(r->http_accept_thread);

    /* Close all active connections */
    edb_mutex_lock(&r->pool_mu);
    win_conn_ctx_t *cur = r->active_head;
    while (cur) {
        win_conn_ctx_t *nxt = cur->active_next;
        edb_reactor_conn_retain(&cur->conn);
        edb_mutex_unlock(&r->pool_mu);
        close_conn_socket(r, cur);
        edb_reactor_conn_release(r, &cur->conn);
        edb_mutex_lock(&r->pool_mu);
        cur = nxt;
    }
    edb_mutex_unlock(&r->pool_mu);

    for (size_t i = 0; i < r->io_thread_count; i++) {
        PostQueuedCompletionStatus(r->iocp, 0, 0, NULL);
    }
    for (size_t i = 0; i < r->io_thread_count; i++) {
        edb_thread_join(r->io_threads[i]);
    }
}

void edb_reactor_destroy(edb_reactor_t *r) {
    if (!r) return;
    edb_reactor_stop(r);
    free(r->io_threads);
    if (r->iocp) CloseHandle(r->iocp);

    win_slab_chunk_t *ch = r->chunks;
    while (ch) {
        win_slab_chunk_t *nxt = ch->next;
        for (size_t i = 0; i < ch->count; i++) {
            edb_mutex_destroy(&ch->items[i].io_mu);
            edb_buf_free(&ch->items[i].rx_buf);
            edb_buf_free(&ch->items[i].tx_buf);
            edb_buf_free(&ch->items[i].tx_active);
            edb_buf_free(&ch->items[i].conn.rbuf);
            edb_buf_free(&ch->items[i].conn.wbuf);
        }
        free(ch->items);
        free(ch);
        ch = nxt;
    }
    edb_mutex_destroy(&r->pool_mu);
    edb_net_cleanup();
    free(r);
}

void edb_reactor_reap_idle(edb_reactor_t *r, uint64_t idle_timeout_ms) {
    if (idle_timeout_ms == 0) return;
    uint64_t now = edb_now_ms();
    edb_mutex_lock(&r->pool_mu);
    win_conn_ctx_t *cur = r->active_head;
    while (cur) {
        win_conn_ctx_t *nxt = cur->active_next;
        if (now - cur->conn.last_active_ms > idle_timeout_ms &&
            edb_atomic_load_u32(&cur->conn.worker_busy) == 0) {
            edb_reactor_conn_retain(&cur->conn);
            edb_mutex_unlock(&r->pool_mu);
            close_conn_socket(r, cur);
            edb_reactor_conn_release(r, &cur->conn);
            edb_mutex_lock(&r->pool_mu);
        }
        cur = nxt;
    }
    edb_mutex_unlock(&r->pool_mu);
}

#endif /* _WIN32 */
