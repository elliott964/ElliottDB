#ifndef EDB_PAL_H
#define EDB_PAL_H

#include "edb_common.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <intrin.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

/* ---- Atomics (portable across MSVC C11 and GCC/Clang) ---- */
typedef struct { volatile int64_t v; } edb_atomic_i64;
typedef struct { volatile uint64_t v; } edb_atomic_u64;
typedef struct { volatile uint32_t v; } edb_atomic_u32;

#ifdef _MSC_VER
static __forceinline int64_t edb_atomic_load_i64(const edb_atomic_i64 *a) {
    _ReadWriteBarrier();
    return a->v;
}
static __forceinline void edb_atomic_store_i64(edb_atomic_i64 *a, int64_t val) {
    _InterlockedExchange64(&a->v, val);
}
static __forceinline int64_t edb_atomic_add_i64(edb_atomic_i64 *a, int64_t delta) {
    return _InterlockedExchangeAdd64(&a->v, delta) + delta;
}
static __forceinline bool edb_atomic_cas_i64(edb_atomic_i64 *a, int64_t expected, int64_t desired) {
    return _InterlockedCompareExchange64(&a->v, desired, expected) == expected;
}

static __forceinline uint64_t edb_atomic_load_u64(const edb_atomic_u64 *a) {
    _ReadWriteBarrier();
    return a->v;
}
static __forceinline void edb_atomic_store_u64(edb_atomic_u64 *a, uint64_t val) {
    _InterlockedExchange64((volatile int64_t *)&a->v, (int64_t)val);
}
static __forceinline uint64_t edb_atomic_add_u64(edb_atomic_u64 *a, uint64_t delta) {
    return (uint64_t)(_InterlockedExchangeAdd64((volatile int64_t *)&a->v, (int64_t)delta) + (int64_t)delta);
}
static __forceinline bool edb_atomic_cas_u64(edb_atomic_u64 *a, uint64_t expected, uint64_t desired) {
    return (uint64_t)_InterlockedCompareExchange64((volatile int64_t *)&a->v, (int64_t)desired, (int64_t)expected) == expected;
}

static __forceinline uint32_t edb_atomic_load_u32(const edb_atomic_u32 *a) {
    _ReadWriteBarrier();
    return a->v;
}
static __forceinline void edb_atomic_store_u32(edb_atomic_u32 *a, uint32_t val) {
    _InterlockedExchange((volatile long *)&a->v, (long)val);
}
static __forceinline uint32_t edb_atomic_add_u32(edb_atomic_u32 *a, uint32_t delta) {
    return (uint32_t)(_InterlockedExchangeAdd((volatile long *)&a->v, (long)delta) + (long)delta);
}
static __forceinline bool edb_atomic_cas_u32(edb_atomic_u32 *a, uint32_t expected, uint32_t desired) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)&a->v, (long)desired, (long)expected) == expected;
}
static __forceinline void edb_cpu_pause(void) { _mm_pause(); }
#else
static inline int64_t edb_atomic_load_i64(const edb_atomic_i64 *a) {
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
}
static inline void edb_atomic_store_i64(edb_atomic_i64 *a, int64_t val) {
    __atomic_store_n(&a->v, val, __ATOMIC_RELEASE);
}
static inline int64_t edb_atomic_add_i64(edb_atomic_i64 *a, int64_t delta) {
    return __atomic_add_fetch(&a->v, delta, __ATOMIC_ACQ_REL);
}
static inline bool edb_atomic_cas_i64(edb_atomic_i64 *a, int64_t expected, int64_t desired) {
    return __atomic_compare_exchange_n(&a->v, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline uint64_t edb_atomic_load_u64(const edb_atomic_u64 *a) {
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
}
static inline void edb_atomic_store_u64(edb_atomic_u64 *a, uint64_t val) {
    __atomic_store_n(&a->v, val, __ATOMIC_RELEASE);
}
static inline uint64_t edb_atomic_add_u64(edb_atomic_u64 *a, uint64_t delta) {
    return __atomic_add_fetch(&a->v, delta, __ATOMIC_ACQ_REL);
}
static inline bool edb_atomic_cas_u64(edb_atomic_u64 *a, uint64_t expected, uint64_t desired) {
    return __atomic_compare_exchange_n(&a->v, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline uint32_t edb_atomic_load_u32(const edb_atomic_u32 *a) {
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
}
static inline void edb_atomic_store_u32(edb_atomic_u32 *a, uint32_t val) {
    __atomic_store_n(&a->v, val, __ATOMIC_RELEASE);
}
static inline uint32_t edb_atomic_add_u32(edb_atomic_u32 *a, uint32_t delta) {
    return __atomic_add_fetch(&a->v, delta, __ATOMIC_ACQ_REL);
}
static inline bool edb_atomic_cas_u32(edb_atomic_u32 *a, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(&a->v, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static inline void edb_cpu_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}
#endif

/* ---- Synchronization Primitives ---- */
#ifdef _WIN32
typedef CRITICAL_SECTION edb_mutex_t;
typedef SRWLOCK edb_rwlock_t;
typedef CONDITION_VARIABLE edb_cond_t;
typedef HANDLE edb_thread_t;
typedef SOCKET edb_socket_t;
#define EDB_INVALID_SOCK INVALID_SOCKET
#else
typedef pthread_mutex_t edb_mutex_t;
typedef pthread_rwlock_t edb_rwlock_t;
typedef pthread_cond_t edb_cond_t;
typedef pthread_t edb_thread_t;
typedef int edb_socket_t;
#define EDB_INVALID_SOCK (-1)
#endif

void edb_mutex_init(edb_mutex_t *m);
void edb_mutex_destroy(edb_mutex_t *m);
void edb_mutex_lock(edb_mutex_t *m);
void edb_mutex_unlock(edb_mutex_t *m);

void edb_rwlock_init(edb_rwlock_t *rw);
void edb_rwlock_destroy(edb_rwlock_t *rw);
void edb_rwlock_rdlock(edb_rwlock_t *rw);
void edb_rwlock_rdunlock(edb_rwlock_t *rw);
void edb_rwlock_wrlock(edb_rwlock_t *rw);
void edb_rwlock_wrunlock(edb_rwlock_t *rw);

void edb_cond_init(edb_cond_t *c);
void edb_cond_destroy(edb_cond_t *c);
void edb_cond_wait(edb_cond_t *c, edb_mutex_t *m);
bool edb_cond_timedwait_ms(edb_cond_t *c, edb_mutex_t *m, uint32_t timeout_ms);
void edb_cond_signal(edb_cond_t *c);
void edb_cond_broadcast(edb_cond_t *c);

typedef void (*edb_thread_fn)(void *arg);
bool edb_thread_create(edb_thread_t *t, edb_thread_fn fn, void *arg);
void edb_thread_join(edb_thread_t t);

/* ---- Time & System ---- */
uint64_t edb_now_ns(void);
uint64_t edb_now_us(void);
uint64_t edb_now_ms(void);
uint64_t edb_wall_sec(void);
void edb_sleep_ms(uint32_t ms);
int edb_cpu_count(void);
uint64_t edb_total_ram_mb(void);
uint64_t edb_pid(void);

/* ---- File I/O ---- */
typedef struct {
#ifdef _WIN32
    HANDLE handle;
    edb_mutex_t io_mu;
#else
    int fd;
#endif
    bool is_open;
} edb_file_t;

bool edb_file_open(edb_file_t *f, const char *path, bool create, bool truncate);
void edb_file_close(edb_file_t *f);
bool edb_file_pread(edb_file_t *f, void *buf, size_t len, uint64_t offset, size_t *out_read);
bool edb_file_pwrite(edb_file_t *f, const void *buf, size_t len, uint64_t offset);
bool edb_file_append(edb_file_t *f, const void *buf, size_t len, uint64_t *out_offset);
bool edb_file_sync(edb_file_t *f);
bool edb_file_size(edb_file_t *f, uint64_t *out_size);
bool edb_file_truncate(edb_file_t *f, uint64_t size);
bool edb_mkdir_p(const char *path);
bool edb_file_exists(const char *path);
bool edb_file_remove(const char *path);

/* ---- Signals ---- */
void edb_install_signal_handlers(void (*on_shutdown)(void));

/* ---- Networking Helpers ---- */
bool edb_net_init(void);
void edb_net_cleanup(void);
void edb_sock_close(edb_socket_t s);
bool edb_sock_set_nonblocking(edb_socket_t s, bool nb);
bool edb_sock_set_nodelay(edb_socket_t s, bool nodelay);
bool edb_sock_set_reuseaddr(edb_socket_t s, bool reuse);
bool edb_sock_set_reuseport(edb_socket_t s, bool reuse);
bool edb_sock_set_keepalive(edb_socket_t s, bool ka, int idle_sec, int interval_sec);
bool edb_sock_set_bufs(edb_socket_t s, int recv_kb, int send_kb);
int edb_sock_last_error(void);
bool edb_sock_would_block(int err);
edb_socket_t edb_sock_connect_tcp(const char *host, uint16_t port, int timeout_ms);
int edb_sock_send_all(edb_socket_t s, const void *buf, size_t len);
int edb_sock_recv_exact(edb_socket_t s, void *buf, size_t len);

#endif /* EDB_PAL_H */
