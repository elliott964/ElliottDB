#ifdef _WIN32

#include "pal.h"
#include <mstcpip.h>
#include <process.h>

static void (*g_shutdown_cb)(void) = NULL;
static LARGE_INTEGER g_qpc_freq;
static volatile LONG g_qpc_init = 0;

static void ensure_qpc(void) {
    if (InterlockedCompareExchange(&g_qpc_init, 1, 0) == 0) {
        QueryPerformanceFrequency(&g_qpc_freq);
    }
}

void edb_mutex_init(edb_mutex_t *m) {
    InitializeCriticalSectionAndSpinCount(m, 4000);
}
void edb_mutex_destroy(edb_mutex_t *m) {
    DeleteCriticalSection(m);
}
void edb_mutex_lock(edb_mutex_t *m) {
    EnterCriticalSection(m);
}
void edb_mutex_unlock(edb_mutex_t *m) {
    LeaveCriticalSection(m);
}

void edb_rwlock_init(edb_rwlock_t *rw) {
    InitializeSRWLock(rw);
}
void edb_rwlock_destroy(edb_rwlock_t *rw) {
    (void)rw;
}
void edb_rwlock_rdlock(edb_rwlock_t *rw) {
    AcquireSRWLockShared(rw);
}
void edb_rwlock_rdunlock(edb_rwlock_t *rw) {
    ReleaseSRWLockShared(rw);
}
void edb_rwlock_wrlock(edb_rwlock_t *rw) {
    AcquireSRWLockExclusive(rw);
}
void edb_rwlock_wrunlock(edb_rwlock_t *rw) {
    ReleaseSRWLockExclusive(rw);
}

void edb_cond_init(edb_cond_t *c) {
    InitializeConditionVariable(c);
}
void edb_cond_destroy(edb_cond_t *c) {
    (void)c;
}
void edb_cond_wait(edb_cond_t *c, edb_mutex_t *m) {
    SleepConditionVariableCS(c, m, INFINITE);
}
bool edb_cond_timedwait_ms(edb_cond_t *c, edb_mutex_t *m, uint32_t timeout_ms) {
    return SleepConditionVariableCS(c, m, timeout_ms) != 0;
}
void edb_cond_signal(edb_cond_t *c) {
    WakeConditionVariable(c);
}
void edb_cond_broadcast(edb_cond_t *c) {
    WakeAllConditionVariable(c);
}

typedef struct {
    edb_thread_fn fn;
    void *arg;
} win_thread_wrap_t;

static unsigned __stdcall win_thread_entry(void *p) {
    win_thread_wrap_t *w = (win_thread_wrap_t *)p;
    edb_thread_fn fn = w->fn;
    void *arg = w->arg;
    free(w);
    fn(arg);
    return 0;
}

bool edb_thread_create(edb_thread_t *t, edb_thread_fn fn, void *arg) {
    win_thread_wrap_t *w = (win_thread_wrap_t *)malloc(sizeof(*w));
    if (!w) return false;
    w->fn = fn;
    w->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, win_thread_entry, w, 0, NULL);
    if (!h) {
        free(w);
        return false;
    }
    *t = (HANDLE)h;
    return true;
}

void edb_thread_join(edb_thread_t t) {
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
}

uint64_t edb_now_ns(void) {
    ensure_qpc();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double sec = (double)now.QuadPart / (double)g_qpc_freq.QuadPart;
    return (uint64_t)(sec * 1000000000.0);
}

uint64_t edb_now_us(void) {
    return edb_now_ns() / 1000ULL;
}

uint64_t edb_now_ms(void) {
    return GetTickCount64();
}

uint64_t edb_wall_sec(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000000ULL;
}

void edb_sleep_ms(uint32_t ms) {
    Sleep(ms);
}

int edb_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 4;
}

uint64_t edb_total_ram_mb(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        return ms.ullTotalPhys / (1024ULL * 1024ULL);
    }
    return 4096;
}

uint64_t edb_pid(void) {
    return (uint64_t)GetCurrentProcessId();
}

bool edb_file_open(edb_file_t *f, const char *path, bool create, bool truncate) {
    memset(f, 0, sizeof(*f));
    DWORD disp = OPEN_EXISTING;
    if (create && truncate) disp = CREATE_ALWAYS;
    else if (create) disp = OPEN_ALWAYS;
    else if (truncate) disp = TRUNCATE_EXISTING;

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    f->handle = h;
    edb_mutex_init(&f->io_mu);
    f->is_open = true;
    return true;
}

void edb_file_close(edb_file_t *f) {
    if (f && f->is_open) {
        CloseHandle(f->handle);
        edb_mutex_destroy(&f->io_mu);
        f->handle = INVALID_HANDLE_VALUE;
        f->is_open = false;
    }
}

bool edb_file_pread(edb_file_t *f, void *buf, size_t len, uint64_t offset, size_t *out_read) {
    if (!f || !f->is_open) return false;
    edb_mutex_lock(&f->io_mu);
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f->handle, li, NULL, FILE_BEGIN)) {
        edb_mutex_unlock(&f->io_mu);
        return false;
    }
    size_t total = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (total < len) {
        DWORD chunk = (DWORD)((len - total) > 0x40000000ULL ? 0x40000000ULL : (len - total));
        DWORD got = 0;
        if (!ReadFile(f->handle, dst + total, chunk, &got, NULL)) {
            edb_mutex_unlock(&f->io_mu);
            return false;
        }
        total += got;
        if (got == 0) break;
    }
    edb_mutex_unlock(&f->io_mu);
    if (out_read) *out_read = total;
    return true;
}

bool edb_file_pwrite(edb_file_t *f, const void *buf, size_t len, uint64_t offset) {
    if (!f || !f->is_open) return false;
    edb_mutex_lock(&f->io_mu);
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f->handle, li, NULL, FILE_BEGIN)) {
        edb_mutex_unlock(&f->io_mu);
        return false;
    }
    size_t total = 0;
    const uint8_t *src = (const uint8_t *)buf;
    while (total < len) {
        DWORD chunk = (DWORD)((len - total) > 0x40000000ULL ? 0x40000000ULL : (len - total));
        DWORD put = 0;
        if (!WriteFile(f->handle, src + total, chunk, &put, NULL)) {
            edb_mutex_unlock(&f->io_mu);
            return false;
        }
        total += put;
        if (put == 0) {
            edb_mutex_unlock(&f->io_mu);
            return false;
        }
    }
    edb_mutex_unlock(&f->io_mu);
    return true;
}

bool edb_file_append(edb_file_t *f, const void *buf, size_t len, uint64_t *out_offset) {
    if (!f || !f->is_open) return false;
    edb_mutex_lock(&f->io_mu);
    LARGE_INTEGER zero = {0}, pos = {0};
    if (!SetFilePointerEx(f->handle, zero, &pos, FILE_END)) {
        edb_mutex_unlock(&f->io_mu);
        return false;
    }
    if (out_offset) *out_offset = (uint64_t)pos.QuadPart;
    size_t total = 0;
    const uint8_t *src = (const uint8_t *)buf;
    while (total < len) {
        DWORD chunk = (DWORD)((len - total) > 0x40000000ULL ? 0x40000000ULL : (len - total));
        DWORD put = 0;
        if (!WriteFile(f->handle, src + total, chunk, &put, NULL)) {
            edb_mutex_unlock(&f->io_mu);
            return false;
        }
        total += put;
    }
    edb_mutex_unlock(&f->io_mu);
    return true;
}

bool edb_file_sync(edb_file_t *f) {
    if (!f || !f->is_open) return false;
    return FlushFileBuffers(f->handle) != 0;
}

bool edb_file_size(edb_file_t *f, uint64_t *out_size) {
    if (!f || !f->is_open) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->handle, &sz)) return false;
    *out_size = (uint64_t)sz.QuadPart;
    return true;
}

bool edb_file_truncate(edb_file_t *f, uint64_t size) {
    if (!f || !f->is_open) return false;
    edb_mutex_lock(&f->io_mu);
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    bool ok = SetFilePointerEx(f->handle, li, NULL, FILE_BEGIN) && SetEndOfFile(f->handle);
    edb_mutex_unlock(&f->io_mu);
    return ok;
}

bool edb_mkdir_p(const char *path) {
    if (!path || !*path) return false;
    char tmp[EDB_MAX_PATH];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == '/') tmp[i] = '\\';
    }
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '\\') {
            if (i == 2 && tmp[1] == ':') continue;
            tmp[i] = '\0';
            CreateDirectoryA(tmp, NULL);
            tmp[i] = '\\';
        }
    }
    if (CreateDirectoryA(tmp, NULL)) return true;
    DWORD err = GetLastError();
    return err == ERROR_ALREADY_EXISTS;
}

bool edb_file_exists(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool edb_file_remove(const char *path) {
    return DeleteFileA(path) != 0;
}

static BOOL WINAPI win_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        if (g_shutdown_cb) g_shutdown_cb();
        return TRUE;
    }
    return FALSE;
}

void edb_install_signal_handlers(void (*on_shutdown)(void)) {
    g_shutdown_cb = on_shutdown;
    SetConsoleCtrlHandler(win_ctrl_handler, TRUE);
}

bool edb_net_init(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void edb_net_cleanup(void) {
    WSACleanup();
}

void edb_sock_close(edb_socket_t s) {
    if (s != EDB_INVALID_SOCK) closesocket(s);
}

bool edb_sock_set_nonblocking(edb_socket_t s, bool nb) {
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool edb_sock_set_nodelay(edb_socket_t s, bool nodelay) {
    BOOL v = nodelay ? TRUE : FALSE;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&v, sizeof(v)) == 0;
}

bool edb_sock_set_reuseaddr(edb_socket_t s, bool reuse) {
    BOOL v = reuse ? TRUE : FALSE;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&v, sizeof(v)) == 0;
}

bool edb_sock_set_reuseport(edb_socket_t s, bool reuse) {
    (void)s;
    (void)reuse;
    return true;
}

bool edb_sock_set_keepalive(edb_socket_t s, bool ka, int idle_sec, int interval_sec) {
    BOOL v = ka ? TRUE : FALSE;
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&v, sizeof(v)) != 0) return false;
    if (ka) {
        struct tcp_keepalive alive;
        alive.onoff = 1;
        alive.keepalivetime = (ULONG)(idle_sec > 0 ? idle_sec * 1000 : 60000);
        alive.keepaliveinterval = (ULONG)(interval_sec > 0 ? interval_sec * 1000 : 10000);
        DWORD bytes = 0;
        WSAIoctl(s, SIO_KEEPALIVE_VALS, &alive, sizeof(alive), NULL, 0, &bytes, NULL, NULL);
    }
    return true;
}

bool edb_sock_set_bufs(edb_socket_t s, int recv_kb, int send_kb) {
    if (recv_kb > 0) {
        int v = recv_kb * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&v, sizeof(v));
    }
    if (send_kb > 0) {
        int v = send_kb * 1024;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&v, sizeof(v));
    }
    return true;
}

int edb_sock_last_error(void) {
    return WSAGetLastError();
}

bool edb_sock_would_block(int err) {
    return err == WSAEWOULDBLOCK || err == WSA_IO_PENDING;
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
        if (sock == EDB_INVALID_SOCK) continue;
        if (timeout_ms > 0) {
            DWORD tv = (DWORD)timeout_ms;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
        }
        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            edb_sock_set_nodelay(sock, true);
            break;
        }
        closesocket(sock);
        sock = EDB_INVALID_SOCK;
    }
    freeaddrinfo(res);
    return sock;
}

int edb_sock_send_all(edb_socket_t s, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        int chunk = (int)((len - sent) > 0x7FFFFFFF ? 0x7FFFFFFF : (len - sent));
        int n = send(s, p + sent, chunk, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int edb_sock_recv_exact(edb_socket_t s, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len) {
        int chunk = (int)((len - got) > 0x7FFFFFFF ? 0x7FFFFFFF : (len - got));
        int n = recv(s, p + got, chunk, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

#endif /* _WIN32 */
