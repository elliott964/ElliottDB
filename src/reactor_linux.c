#ifndef _WIN32

#define _GNU_SOURCE
#include "reactor.h"
#include <errno.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#define MAX_EPOLL_EVENTS 512

typedef struct linux_conn_ctx {
    edb_conn_t conn;
    int epoll_fd;
    int wake_fd;
    edb_mutex_t io_mu;
    edb_buf_t rx_buf;
    edb_buf_t tx_buf;
    size_t tx_pos;
    bool close_after_tx;
    bool want_write;
    struct linux_conn_ctx *slab_next;
    struct linux_conn_ctx *active_next;
    struct linux_conn_ctx *active_prev;
    bool in_active_list;
} linux_conn_ctx_t;

typedef struct linux_slab_chunk {
    linux_conn_ctx_t *items;
    size_t count;
    struct linux_slab_chunk *next;
} linux_slab_chunk_t;

typedef struct {
    edb_reactor_t *r;
    int idx;
    int epoll_fd;
    int wake_fd;
    edb_socket_t listen_sock;
    edb_socket_t http_sock;
    edb_thread_t thread;
    edb_mutex_t kick_mu;
    linux_conn_ctx_t **kick_list;
    size_t kick_count;
    size_t kick_cap;
} epoll_loop_t;

struct edb_reactor {
    edb_reactor_opts_t opts;
    epoll_loop_t *loops;
    size_t loop_count;
    edb_atomic_u32 running;
    edb_atomic_u64 next_conn_id;
    edb_atomic_u64 rr_loop;

    edb_mutex_t pool_mu;
    linux_slab_chunk_t *chunks;
    linux_conn_ctx_t *free_list;
    linux_conn_ctx_t *active_head;
};

static linux_conn_ctx_t *alloc_conn(edb_reactor_t *r, int epoll_fd, int wake_fd) {
    edb_mutex_lock(&r->pool_mu);
    if (!r->free_list) {
        size_t chunk_sz = (size_t)r->opts.cfg->conn_slab_chunk;
        if (chunk_sz < 64) chunk_sz = 64;
        linux_slab_chunk_t *ch = (linux_slab_chunk_t *)calloc(1, sizeof(linux_slab_chunk_t));
        ch->items = (linux_conn_ctx_t *)calloc(chunk_sz, sizeof(linux_conn_ctx_t));
        ch->count = chunk_sz;
        for (size_t i = 0; i < chunk_sz; i++) {
            edb_mutex_init(&ch->items[i].io_mu);
            edb_buf_init(&ch->items[i].rx_buf);
            edb_buf_init(&ch->items[i].tx_buf);
            edb_buf_init(&ch->items[i].conn.rbuf);
            edb_buf_init(&ch->items[i].conn.wbuf);
            ch->items[i].slab_next = r->free_list;
            r->free_list = &ch->items[i];
        }
        ch->next = r->chunks;
        r->chunks = ch;
    }
    linux_conn_ctx_t *ctx = r->free_list;
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

    ctx->epoll_fd = epoll_fd;
    ctx->wake_fd = wake_fd;
    edb_buf_clear(&ctx->rx_buf);
    edb_buf_clear(&ctx->tx_buf);
    ctx->tx_pos = 0;
    ctx->close_after_tx = false;
    ctx->want_write = false;

    edb_mutex_lock(&r->pool_mu);
    ctx->active_prev = NULL;
    ctx->active_next = r->active_head;
    if (r->active_head) r->active_head->active_prev = ctx;
    r->active_head = ctx;
    ctx->in_active_list = true;
    edb_mutex_unlock(&r->pool_mu);
    return ctx;
}

static void free_conn_final(edb_reactor_t *r, linux_conn_ctx_t *ctx) {
    if (ctx->conn.sock != EDB_INVALID_SOCK) {
        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, ctx->conn.sock, NULL);
        edb_sock_close(ctx->conn.sock);
        ctx->conn.sock = EDB_INVALID_SOCK;
        if (r->opts.metrics) edb_atomic_add_i64(&r->opts.metrics->active_connections, -1);
    }
    if (r->opts.on_close) r->opts.on_close(r->opts.server_ctx, &ctx->conn);

    edb_buf_free(&ctx->conn.rbuf);
    edb_buf_free(&ctx->conn.wbuf);
    edb_buf_free(&ctx->rx_buf);
    edb_buf_free(&ctx->tx_buf);

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
        free_conn_final(r, (linux_conn_ctx_t *)conn->reactor_ctx);
    }
}

static void close_conn_socket(edb_reactor_t *r, linux_conn_ctx_t *ctx) {
    if (edb_atomic_cas_u32(&ctx->conn.closing, 0, 1)) {
        if (ctx->conn.sock != EDB_INVALID_SOCK) {
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, ctx->conn.sock, NULL);
            edb_sock_close(ctx->conn.sock);
            ctx->conn.sock = EDB_INVALID_SOCK;
            if (r->opts.metrics) edb_atomic_add_i64(&r->opts.metrics->active_connections, -1);
        }
        edb_reactor_conn_release(r, &ctx->conn);
    }
}

void edb_reactor_pull_rx(edb_reactor_t *r, edb_conn_t *conn) {
    (void)r;
    linux_conn_ctx_t *ctx = (linux_conn_ctx_t *)conn->reactor_ctx;
    edb_mutex_lock(&ctx->io_mu);
    if (ctx->rx_buf.len > 0) {
        edb_buf_append(&conn->rbuf, ctx->rx_buf.data, ctx->rx_buf.len);
        edb_buf_clear(&ctx->rx_buf);
    }
    edb_mutex_unlock(&ctx->io_mu);
}

bool edb_reactor_has_rx(edb_reactor_t *r, edb_conn_t *conn) {
    (void)r;
    linux_conn_ctx_t *ctx = (linux_conn_ctx_t *)conn->reactor_ctx;
    edb_mutex_lock(&ctx->io_mu);
    bool has = ctx->rx_buf.len > 0;
    edb_mutex_unlock(&ctx->io_mu);
    return has;
}

static bool flush_tx_locked(edb_reactor_t *r, linux_conn_ctx_t *ctx) {
    if (edb_atomic_load_u32(&ctx->conn.closing)) return false;
    while (ctx->tx_pos < ctx->tx_buf.len) {
        ssize_t n = send(ctx->conn.sock, ctx->tx_buf.data + ctx->tx_pos,
                         ctx->tx_buf.len - ctx->tx_pos, MSG_NOSIGNAL);
        if (n > 0) {
            ctx->tx_pos += (size_t)n;
            if (r->opts.metrics) edb_atomic_add_u64(&r->opts.metrics->bytes_out, (uint64_t)n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!ctx->want_write) {
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
                ev.data.ptr = ctx;
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, ctx->conn.sock, &ev);
                ctx->want_write = true;
            }
            return false;
        } else {
            return true;
        }
    }
    edb_buf_clear(&ctx->tx_buf);
    ctx->tx_pos = 0;
    if (ctx->want_write) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = ctx;
        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, ctx->conn.sock, &ev);
        ctx->want_write = false;
    }
    return ctx->close_after_tx;
}

void edb_reactor_push_tx(edb_reactor_t *r, edb_conn_t *conn, bool close_after_write) {
    linux_conn_ctx_t *ctx = (linux_conn_ctx_t *)conn->reactor_ctx;
    edb_mutex_lock(&ctx->io_mu);
    if (conn->wbuf.len > 0) {
        edb_buf_append(&ctx->tx_buf, conn->wbuf.data, conn->wbuf.len);
        edb_buf_clear(&conn->wbuf);
    }
    if (close_after_write) ctx->close_after_tx = true;
    bool need_close = flush_tx_locked(r, ctx);
    edb_mutex_unlock(&ctx->io_mu);
    if (need_close) close_conn_socket(r, ctx);
}

static edb_socket_t bind_listen_socket(const char *host, uint16_t port, int backlog, bool reuseport, char *err, size_t err_cap) {
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
    edb_socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) {
        freeaddrinfo(res);
        if (err) snprintf(err, err_cap, "socket() failed");
        return EDB_INVALID_SOCK;
    }
    edb_sock_set_reuseaddr(s, true);
    if (reuseport) edb_sock_set_reuseport(s, true);
    edb_sock_set_nonblocking(s, true);
    if (bind(s, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        edb_sock_close(s);
        if (err) snprintf(err, err_cap, "Failed to bind %s:%u (errno=%d)", host, (unsigned)port, errno);
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

static void accept_all(epoll_loop_t *lp, edb_socket_t lsock, bool is_http) {
    edb_reactor_t *r = lp->r;
    while (true) {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        edb_socket_t cs = accept4(lsock, (struct sockaddr *)&ss, &slen, SOCK_NONBLOCK);
        if (cs < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
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

        linux_conn_ctx_t *ctx = alloc_conn(r, lp->epoll_fd, lp->wake_fd);
        ctx->conn.sock = cs;
        if (is_http) ctx->conn.proto = PROTO_HTTP;

        if (r->opts.metrics) {
            edb_atomic_add_i64(&r->opts.metrics->active_connections, 1);
            edb_atomic_add_u64(&r->opts.metrics->total_connections, 1);
        }

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = ctx;
        if (epoll_ctl(lp->epoll_fd, EPOLL_CTL_ADD, cs, &ev) != 0) {
            close_conn_socket(r, ctx);
        }
    }
}

static void epoll_loop_fn(void *arg) {
    epoll_loop_t *lp = (epoll_loop_t *)arg;
    edb_reactor_t *r = lp->r;
    struct epoll_event events[MAX_EPOLL_EVENTS];
    uint8_t rdbuf[16384];

    while (edb_atomic_load_u32(&r->running)) {
        int n = epoll_wait(lp->epoll_fd, events, MAX_EPOLL_EVENTS, 100);
        for (int i = 0; i < n; i++) {
            void *ptr = events[i].data.ptr;
            uint32_t evs = events[i].events;

            if (ptr == &lp->wake_fd) {
                uint64_t val = 0;
                ssize_t rd = read(lp->wake_fd, &val, sizeof(val));
                (void)rd;
                continue;
            }
            if (ptr == &lp->listen_sock) {
                accept_all(lp, lp->listen_sock, false);
                continue;
            }
            if (ptr == &lp->http_sock) {
                accept_all(lp, lp->http_sock, true);
                continue;
            }

            linux_conn_ctx_t *ctx = (linux_conn_ctx_t *)ptr;
            edb_reactor_conn_retain(&ctx->conn);

            if (evs & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                close_conn_socket(r, ctx);
                edb_reactor_conn_release(r, &ctx->conn);
                continue;
            }

            if (evs & EPOLLIN) {
                bool got_data = false;
                bool closed = false;
                size_t max_rb = (size_t)r->opts.cfg->read_buf_max_mb * 1024ULL * 1024ULL;
                while (true) {
                    ssize_t rd = recv(ctx->conn.sock, rdbuf, sizeof(rdbuf), 0);
                    if (rd > 0) {
                        got_data = true;
                        if (r->opts.metrics) edb_atomic_add_u64(&r->opts.metrics->bytes_in, (uint64_t)rd);
                        edb_mutex_lock(&ctx->io_mu);
                        if (ctx->rx_buf.len + (size_t)rd > max_rb) {
                            closed = true;
                        } else {
                            edb_buf_append(&ctx->rx_buf, rdbuf, (size_t)rd);
                            ctx->conn.last_active_ms = edb_now_ms();
                        }
                        edb_mutex_unlock(&ctx->io_mu);
                        if (closed) break;
                    } else if (rd == 0) {
                        closed = true;
                        break;
                    } else {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        closed = true;
                        break;
                    }
                }
                if (got_data && !closed) {
                    r->opts.on_data(r->opts.server_ctx, &ctx->conn);
                }
                if (closed) {
                    close_conn_socket(r, ctx);
                    edb_reactor_conn_release(r, &ctx->conn);
                    continue;
                }
            }

            if (evs & EPOLLOUT) {
                edb_mutex_lock(&ctx->io_mu);
                bool need_close = flush_tx_locked(r, ctx);
                edb_mutex_unlock(&ctx->io_mu);
                if (need_close) close_conn_socket(r, ctx);
            }

            edb_reactor_conn_release(r, &ctx->conn);
        }
    }
}

edb_reactor_t *edb_reactor_create(const edb_reactor_opts_t *opts, char *err, size_t err_cap) {
    edb_net_init();
    edb_reactor_t *r = (edb_reactor_t *)calloc(1, sizeof(edb_reactor_t));
    if (!r) return NULL;
    r->opts = *opts;
    edb_mutex_init(&r->pool_mu);
    edb_atomic_store_u64(&r->next_conn_id, 1);

    r->loop_count = (size_t)opts->cfg->io_threads;
    if (r->loop_count < 1) r->loop_count = 2;
    r->loops = (epoll_loop_t *)calloc(r->loop_count, sizeof(epoll_loop_t));

    for (size_t i = 0; i < r->loop_count; i++) {
        epoll_loop_t *lp = &r->loops[i];
        lp->r = r;
        lp->idx = (int)i;
        lp->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        lp->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        lp->listen_sock = EDB_INVALID_SOCK;
        lp->http_sock = EDB_INVALID_SOCK;
        edb_mutex_init(&lp->kick_mu);

        if (lp->epoll_fd < 0 || lp->wake_fd < 0) {
            if (err) snprintf(err, err_cap, "epoll_create1/eventfd failed");
            edb_reactor_destroy(r);
            return NULL;
        }

        struct epoll_event wev;
        wev.events = EPOLLIN;
        wev.data.ptr = &lp->wake_fd;
        epoll_ctl(lp->epoll_fd, EPOLL_CTL_ADD, lp->wake_fd, &wev);

        if (opts->cfg->reuse_port || i == 0) {
            lp->listen_sock = bind_listen_socket(opts->cfg->host, (uint16_t)opts->cfg->port,
                                                 (int)opts->cfg->listen_backlog,
                                                 opts->cfg->reuse_port, err, err_cap);
            if (lp->listen_sock == EDB_INVALID_SOCK) {
                edb_reactor_destroy(r);
                return NULL;
            }
            struct epoll_event lev;
            lev.events = EPOLLIN | EPOLLET;
            lev.data.ptr = &lp->listen_sock;
            epoll_ctl(lp->epoll_fd, EPOLL_CTL_ADD, lp->listen_sock, &lev);

            if (opts->cfg->http_port > 0 && opts->cfg->http_port != opts->cfg->port) {
                lp->http_sock = bind_listen_socket(opts->cfg->host, (uint16_t)opts->cfg->http_port,
                                                   (int)opts->cfg->listen_backlog,
                                                   opts->cfg->reuse_port, err, err_cap);
                if (lp->http_sock == EDB_INVALID_SOCK) {
                    edb_reactor_destroy(r);
                    return NULL;
                }
                struct epoll_event hev;
                hev.events = EPOLLIN | EPOLLET;
                hev.data.ptr = &lp->http_sock;
                epoll_ctl(lp->epoll_fd, EPOLL_CTL_ADD, lp->http_sock, &hev);
            }
        }
    }
    return r;
}

bool edb_reactor_start(edb_reactor_t *r) {
    edb_atomic_store_u32(&r->running, 1);
    for (size_t i = 0; i < r->loop_count; i++) {
        edb_thread_create(&r->loops[i].thread, epoll_loop_fn, &r->loops[i]);
    }
    return true;
}

void edb_reactor_stop(edb_reactor_t *r) {
    if (!edb_atomic_cas_u32(&r->running, 1, 0)) return;
    for (size_t i = 0; i < r->loop_count; i++) {
        uint64_t one = 1;
        ssize_t wr = write(r->loops[i].wake_fd, &one, sizeof(one));
        (void)wr;
    }
    for (size_t i = 0; i < r->loop_count; i++) {
        edb_thread_join(r->loops[i].thread);
        if (r->loops[i].listen_sock != EDB_INVALID_SOCK) {
            edb_sock_close(r->loops[i].listen_sock);
            r->loops[i].listen_sock = EDB_INVALID_SOCK;
        }
        if (r->loops[i].http_sock != EDB_INVALID_SOCK) {
            edb_sock_close(r->loops[i].http_sock);
            r->loops[i].http_sock = EDB_INVALID_SOCK;
        }
    }

    edb_mutex_lock(&r->pool_mu);
    linux_conn_ctx_t *cur = r->active_head;
    while (cur) {
        linux_conn_ctx_t *nxt = cur->active_next;
        edb_reactor_conn_retain(&cur->conn);
        edb_mutex_unlock(&r->pool_mu);
        close_conn_socket(r, cur);
        edb_reactor_conn_release(r, &cur->conn);
        edb_mutex_lock(&r->pool_mu);
        cur = nxt;
    }
    edb_mutex_unlock(&r->pool_mu);
}

void edb_reactor_destroy(edb_reactor_t *r) {
    if (!r) return;
    edb_reactor_stop(r);
    for (size_t i = 0; i < r->loop_count; i++) {
        if (r->loops[i].epoll_fd >= 0) close(r->loops[i].epoll_fd);
        if (r->loops[i].wake_fd >= 0) close(r->loops[i].wake_fd);
        edb_mutex_destroy(&r->loops[i].kick_mu);
        free(r->loops[i].kick_list);
    }
    free(r->loops);

    linux_slab_chunk_t *ch = r->chunks;
    while (ch) {
        linux_slab_chunk_t *nxt = ch->next;
        for (size_t i = 0; i < ch->count; i++) {
            edb_mutex_destroy(&ch->items[i].io_mu);
            edb_buf_free(&ch->items[i].rx_buf);
            edb_buf_free(&ch->items[i].tx_buf);
            edb_buf_free(&ch->items[i].conn.rbuf);
            edb_buf_free(&ch->items[i].conn.wbuf);
        }
        free(ch->items);
        free(ch);
        ch = nxt;
    }
    edb_mutex_destroy(&r->pool_mu);
    free(r);
}

void edb_reactor_reap_idle(edb_reactor_t *r, uint64_t idle_timeout_ms) {
    if (idle_timeout_ms == 0) return;
    uint64_t now = edb_now_ms();
    edb_mutex_lock(&r->pool_mu);
    linux_conn_ctx_t *cur = r->active_head;
    while (cur) {
        linux_conn_ctx_t *nxt = cur->active_next;
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

#endif /* !_WIN32 */
