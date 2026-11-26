#ifndef _WIN32

#define _GNU_SOURCE
#include "pal.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>

static void (*g_shutdown_cb)(void) = NULL;

void edb_mutex_init(edb_mutex_t *m) {
    pthread_mutex_init(m, NULL);
}
void edb_mutex_destroy(edb_mutex_t *m) {
    pthread_mutex_destroy(m);
}
void edb_mutex_lock(edb_mutex_t *m) {
    pthread_mutex_lock(m);
}
void edb_mutex_unlock(edb_mutex_t *m) {
    pthread_mutex_unlock(m);
}

void edb_rwlock_init(edb_rwlock_t *rw) {
    pthread_rwlock_init(rw, NULL);
}
void edb_rwlock_destroy(edb_rwlock_t *rw) {
    pthread_rwlock_destroy(rw);
}
void edb_rwlock_rdlock(edb_rwlock_t *rw) {
    pthread_rwlock_rdlock(rw);
}
void edb_rwlock_rdunlock(edb_rwlock_t *rw) {
    pthread_rwlock_unlock(rw);
}
void edb_rwlock_wrlock(edb_rwlock_t *rw) {
    pthread_rwlock_wrlock(rw);
}
void edb_rwlock_wrunlock(edb_rwlock_t *rw) {
    pthread_rwlock_unlock(rw);
}

void edb_cond_init(edb_cond_t *c) {
    pthread_cond_init(c, NULL);
}
void edb_cond_destroy(edb_cond_t *c) {
    pthread_cond_destroy(c);
}
void edb_cond_wait(edb_cond_t *c, edb_mutex_t *m) {
    pthread_cond_wait(c, m);
}
bool edb_cond_timedwait_ms(edb_cond_t *c, edb_mutex_t *m, uint32_t timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts) == 0;
}
void edb_cond_signal(edb_cond_t *c) {
    pthread_cond_signal(c);
}
void edb_cond_broadcast(edb_cond_t *c) {
    pthread_cond_broadcast(c);
}

typedef struct {
    edb_thread_fn fn;
    void *arg;
} posix_thread_wrap_t;

static void *posix_thread_entry(void *p) {
    posix_thread_wrap_t *w = (posix_thread_wrap_t *)p;
    edb_thread_fn fn = w->fn;
    void *arg = w->arg;
    free(w);
    fn(arg);
    return NULL;
}

bool edb_thread_create(edb_thread_t *t, edb_thread_fn fn, void *arg) {
    posix_thread_wrap_t *w = (posix_thread_wrap_t *)malloc(sizeof(*w));
    if (!w) return false;
    w->fn = fn;
    w->arg = arg;
    if (pthread_create(t, NULL, posix_thread_entry, w) != 0) {
        free(w);
        return false;
    }
    return true;
}

void edb_thread_join(edb_thread_t t) {
    pthread_join(t, NULL);
}

uint64_t edb_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t edb_now_us(void) {
    return edb_now_ns() / 1000ULL;
}

uint64_t edb_now_ms(void) {
    return edb_now_ns() / 1000000ULL;
}

uint64_t edb_wall_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec;
}

void edb_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int edb_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
}

uint64_t edb_total_ram_mb(void) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        return ((uint64_t)si.totalram * (uint64_t)si.mem_unit) / (1024ULL * 1024ULL);
    }
    return 4096;
}

uint64_t edb_pid(void) {
    return (uint64_t)getpid();
}

bool edb_file_open(edb_file_t *f, const char *path, bool create, bool truncate) {
    memset(f, 0, sizeof(*f));
    int flags = O_RDWR;
    if (create) flags |= O_CREAT;
    if (truncate) flags |= O_TRUNC;
    int fd = open(path, flags, 0644);
    if (fd < 0) return false;
    f->fd = fd;
    f->is_open = true;
    return true;
}

void edb_file_close(edb_file_t *f) {
    if (f && f->is_open) {
        close(f->fd);
        f->fd = -1;
        f->is_open = false;
    }
}

bool edb_file_pread(edb_file_t *f, void *buf, size_t len, uint64_t offset, size_t *out_read) {
    if (!f || !f->is_open) return false;
    size_t total = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (total < len) {
        ssize_t n = pread(f->fd, dst + total, len - total, (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    if (out_read) *out_read = total;
    return true;
}

bool edb_file_pwrite(edb_file_t *f, const void *buf, size_t len, uint64_t offset) {
    if (!f || !f->is_open) return false;
    size_t total = 0;
    const uint8_t *src = (const uint8_t *)buf;
    while (total < len) {
        ssize_t n = pwrite(f->fd, src + total, len - total, (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

bool edb_file_append(edb_file_t *f, const void *buf, size_t len, uint64_t *out_offset) {
    if (!f || !f->is_open) return false;
    off_t end = lseek(f->fd, 0, SEEK_END);
    if (end < 0) return false;
    if (out_offset) *out_offset = (uint64_t)end;
    size_t total = 0;
    const uint8_t *src = (const uint8_t *)buf;
    while (total < len) {
        ssize_t n = write(f->fd, src + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

bool edb_file_sync(edb_file_t *f) {
    if (!f || !f->is_open) return false;
    return fdatasync(f->fd) == 0;
}

bool edb_file_size(edb_file_t *f, uint64_t *out_size) {
    if (!f || !f->is_open) return false;
    struct stat st;
    if (fstat(f->fd, &st) != 0) return false;
    *out_size = (uint64_t)st.st_size;
    return true;
}

bool edb_file_truncate(edb_file_t *f, uint64_t size) {
    if (!f || !f->is_open) return false;
    return ftruncate(f->fd, (off_t)size) == 0;
}

bool edb_mkdir_p(const char *path) {
    if (!path || !*path) return false;
    char tmp[EDB_MAX_PATH];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) == 0) return true;
    return errno == EEXIST;
}

bool edb_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool edb_file_remove(const char *path) {
    return unlink(path) == 0;
}

static void posix_sig_handler(int sig) {
    (void)sig;
    if (g_shutdown_cb) g_shutdown_cb();
}

void edb_install_signal_handlers(void (*on_shutdown)(void)) {
    g_shutdown_cb = on_shutdown;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = posix_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

bool edb_net_init(void) {
    signal(SIGPIPE, SIG_IGN);
    return true;
}

void edb_net_cleanup(void) {}

void edb_sock_close(edb_socket_t s) {
    if (s >= 0) close(s);
}

bool edb_sock_set_nonblocking(edb_socket_t s, bool nb) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    if (nb) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(s, F_SETFL, flags) == 0;
}

bool edb_sock_set_nodelay(edb_socket_t s, bool nodelay) {
    int v = nodelay ? 1 : 0;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0;
}

bool edb_sock_set_reuseaddr(edb_socket_t s, bool reuse) {
    int v = reuse ? 1 : 0;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == 0;
}

bool edb_sock_set_reuseport(edb_socket_t s, bool reuse) {
#ifdef SO_REUSEPORT
    int v = reuse ? 1 : 0;
    return setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &v, sizeof(v)) == 0;
#else
    (void)s;
    (void)reuse;
    return true;
#endif
}

bool edb_sock_set_keepalive(edb_socket_t s, bool ka, int idle_sec, int interval_sec) {
    int v = ka ? 1 : 0;
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof(v)) != 0) return false;
#ifdef TCP_KEEPIDLE
    if (ka && idle_sec > 0) setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle_sec, sizeof(idle_sec));
#endif
#ifdef TCP_KEEPINTVL
    if (ka && interval_sec > 0) setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &interval_sec, sizeof(interval_sec));
#endif
    return true;
}

bool edb_sock_set_bufs(edb_socket_t s, int recv_kb, int send_kb) {
    if (recv_kb > 0) {
        int v = recv_kb * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    }
    if (send_kb > 0) {
        int v = send_kb * 1024;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
    }
    return true;
}

int edb_sock_last_error(void) {
    return errno;
}

bool edb_sock_would_block(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS;
}

edb_socket_t edb_sock_connect_tcp(const char *host, uint16_t port, int timeout_ms) {
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return EDB_INVALID_SOCK;

    edb_socket_t sock = EDB_INVALID_SOCK;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (timeout_ms > 0) {
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            edb_sock_set_nodelay(sock, true);
            break;
        }
        close(sock);
        sock = EDB_INVALID_SOCK;
    }
    freeaddrinfo(res);
    return sock;
}

int edb_sock_send_all(edb_socket_t s, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(s, p + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int edb_sock_recv_exact(edb_socket_t s, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(s, p + got, len - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

#endif /* !_WIN32 */
