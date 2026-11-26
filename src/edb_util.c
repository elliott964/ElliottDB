#include "edb_common.h"
#include "pal.h"
#include <ctype.h>
#include <time.h>

const char *edb_status_str(edb_status_t st) {
    switch (st) {
    case EDB_OK:             return "OK";
    case EDB_ERR_SYNTAX:     return "SYNTAX_ERROR";
    case EDB_ERR_NOT_FOUND:  return "NOT_FOUND";
    case EDB_ERR_EXISTS:     return "ALREADY_EXISTS";
    case EDB_ERR_CONSTRAINT: return "CONSTRAINT_VIOLATION";
    case EDB_ERR_IO:         return "IO_ERROR";
    case EDB_ERR_OOM:        return "OUT_OF_MEMORY";
    case EDB_ERR_AUTH:       return "AUTH_FAILED";
    case EDB_ERR_OVERLOAD:   return "OVERLOADED";
    case EDB_ERR_INVALID:    return "INVALID_ARGUMENT";
    case EDB_ERR_TXN:        return "TRANSACTION_ERROR";
    case EDB_ERR_READONLY:   return "READ_ONLY";
    case EDB_ERR_INTERNAL:   return "INTERNAL_ERROR";
    default:                 return "UNKNOWN_ERROR";
    }
}

/* ---- Dynamic Buffer ---- */
void edb_buf_init(edb_buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

bool edb_buf_reserve(edb_buf_t *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t new_cap = b->cap == 0 ? 256 : b->cap;
    while (new_cap < b->len + extra) {
        if (new_cap > ((size_t)1 << 30)) return false;
        new_cap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(b->data, new_cap);
    if (!p) return false;
    b->data = p;
    b->cap = new_cap;
    return true;
}

bool edb_buf_append(edb_buf_t *b, const void *ptr, size_t len) {
    if (len == 0) return true;
    if (!edb_buf_reserve(b, len + 1)) return false;
    memcpy(b->data + b->len, ptr, len);
    b->len += len;
    b->data[b->len] = '\0';
    return true;
}

bool edb_buf_append_str(edb_buf_t *b, const char *s) {
    if (!s) return true;
    return edb_buf_append(b, s, strlen(s));
}

bool edb_buf_append_char(edb_buf_t *b, char c) {
    return edb_buf_append(b, &c, 1);
}

bool edb_buf_append_u8(edb_buf_t *b, uint8_t v) {
    return edb_buf_append(b, &v, 1);
}

bool edb_buf_append_u16(edb_buf_t *b, uint16_t v) {
    uint8_t buf[2];
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    return edb_buf_append(b, buf, 2);
}

bool edb_buf_append_u32(edb_buf_t *b, uint32_t v) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
    return edb_buf_append(b, buf, 4);
}

bool edb_buf_append_u64(edb_buf_t *b, uint64_t v) {
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
    return edb_buf_append(b, buf, 8);
}

bool edb_buf_append_i64(edb_buf_t *b, int64_t v) {
    return edb_buf_append_u64(b, (uint64_t)v);
}

bool edb_buf_append_f64(edb_buf_t *b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    return edb_buf_append_u64(b, u);
}

bool edb_buf_printf(edb_buf_t *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return false;
    }
    if (!edb_buf_reserve(b, (size_t)need + 1)) {
        va_end(ap2);
        return false;
    }
    vsnprintf((char *)b->data + b->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
    return true;
}

void edb_buf_consume(edb_buf_t *b, size_t n) {
    if (n == 0) return;
    if (n >= b->len) {
        b->len = 0;
        if (b->data) b->data[0] = '\0';
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = '\0';
}

void edb_buf_clear(edb_buf_t *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void edb_buf_free(edb_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* ---- Arena Allocator ---- */
void edb_arena_init(edb_arena_t *a) {
    a->head = NULL;
    a->current = NULL;
    a->total_allocated = 0;
}

static edb_arena_block_t *arena_new_block(size_t min_cap) {
    size_t cap = min_cap > EDB_ARENA_BLOCK_SIZE ? min_cap : EDB_ARENA_BLOCK_SIZE;
    edb_arena_block_t *blk = (edb_arena_block_t *)malloc(sizeof(edb_arena_block_t) + cap);
    if (!blk) return NULL;
    blk->next = NULL;
    blk->used = 0;
    blk->cap = cap;
    return blk;
}

void *edb_arena_alloc(edb_arena_t *a, size_t size) {
    size_t aligned = (size + 7) & ~(size_t)7;
    if (aligned == 0) aligned = 8;

    if (a->current && a->current->used + aligned <= a->current->cap) {
        void *p = a->current->data + a->current->used;
        a->current->used += aligned;
        a->total_allocated += aligned;
        return p;
    }

    /* Check if next block after current already exists (after reset) */
    if (a->current && a->current->next && a->current->next->cap >= aligned) {
        a->current = a->current->next;
        a->current->used = aligned;
        a->total_allocated += aligned;
        return a->current->data;
    }

    edb_arena_block_t *blk = arena_new_block(aligned);
    if (!blk) return NULL;
    if (!a->head) {
        a->head = blk;
    } else if (a->current) {
        blk->next = a->current->next;
        a->current->next = blk;
    }
    a->current = blk;
    blk->used = aligned;
    a->total_allocated += aligned;
    return blk->data;
}

void *edb_arena_calloc(edb_arena_t *a, size_t count, size_t size) {
    size_t total = count * size;
    void *p = edb_arena_alloc(a, total);
    if (p) memset(p, 0, total);
    return p;
}

char *edb_arena_strdup(edb_arena_t *a, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)edb_arena_alloc(a, n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

char *edb_arena_strndup(edb_arena_t *a, const char *s, size_t len) {
    if (!s) return NULL;
    char *p = (char *)edb_arena_alloc(a, len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

void edb_arena_reset(edb_arena_t *a) {
    edb_arena_block_t *b = a->head;
    while (b) {
        b->used = 0;
        b = b->next;
    }
    a->current = a->head;
    a->total_allocated = 0;
}

void edb_arena_free(edb_arena_t *a) {
    edb_arena_block_t *b = a->head;
    while (b) {
        edb_arena_block_t *nxt = b->next;
        free(b);
        b = nxt;
    }
    a->head = NULL;
    a->current = NULL;
    a->total_allocated = 0;
}

/* ---- CRC32C (Castagnoli) & 64-bit Hash ---- */
static uint32_t g_crc32c_table[256];
static volatile int g_crc_ready = 0;

static void init_crc32c_table(void) {
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78U) : (crc >> 1);
        }
        g_crc32c_table[i] = crc;
    }
    g_crc_ready = 1;
}

uint32_t edb_crc32c(const void *data, size_t len) {
    init_crc32c_table();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ g_crc32c_table[(crc ^ p[i]) & 0xFFU];
    }
    return crc ^ 0xFFFFFFFFU;
}

uint64_t edb_hash64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    /* SplitMix64 finalizer for great avalanche */
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h;
}

uint64_t edb_hash_str_ci(const char *s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)tolower((unsigned char)s[i]);
        h ^= c;
        h *= 1099511628211ULL;
    }
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h;
}

/* ---- String Helpers ---- */
int edb_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int edb_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

char *edb_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

char *edb_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    char *p = (char *)malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

void edb_str_trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) start++;
    size_t end = len;
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    if (start > 0) memmove(s, s + start, end - start);
    s[end - start] = '\0';
}

bool edb_like_match(const char *str, const char *pat, bool case_insensitive) {
    const char *s = str, *p = pat;
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*p == '%') {
            star = p++;
            ss = s;
        } else if (*p == '_' ||
                   (case_insensitive ? (tolower((unsigned char)*p) == tolower((unsigned char)*s))
                                     : (*p == *s))) {
            p++;
            s++;
        } else if (star) {
            p = star + 1;
            s = ++ss;
        } else {
            return false;
        }
    }
    while (*p == '%') p++;
    return *p == '\0';
}

void edb_json_append_escaped(edb_buf_t *b, const char *s, size_t len) {
    edb_buf_append_char(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  edb_buf_append_str(b, "\\\""); break;
        case '\\': edb_buf_append_str(b, "\\\\"); break;
        case '\b': edb_buf_append_str(b, "\\b"); break;
        case '\f': edb_buf_append_str(b, "\\f"); break;
        case '\n': edb_buf_append_str(b, "\\n"); break;
        case '\r': edb_buf_append_str(b, "\\r"); break;
        case '\t': edb_buf_append_str(b, "\\t"); break;
        default:
            if (c < 0x20) {
                edb_buf_printf(b, "\\u%04x", (unsigned)c);
            } else {
                edb_buf_append_char(b, (char)c);
            }
            break;
        }
    }
    edb_buf_append_char(b, '"');
}

/* ---- Structured Logger ---- */
static edb_mutex_t g_log_mu;
static volatile int g_log_mu_init = 0;
static edb_log_level_t g_log_level = EDB_LOG_INFO;
static bool g_log_json = false;
static FILE *g_log_fp = NULL;

static void ensure_log_mu(void) {
    if (!g_log_mu_init) {
        edb_mutex_init(&g_log_mu);
        g_log_mu_init = 1;
    }
}

static edb_log_level_t parse_log_level(const char *s) {
    if (!s) return EDB_LOG_INFO;
    if (edb_strcasecmp(s, "trace") == 0) return EDB_LOG_TRACE;
    if (edb_strcasecmp(s, "debug") == 0) return EDB_LOG_DEBUG;
    if (edb_strcasecmp(s, "info") == 0)  return EDB_LOG_INFO;
    if (edb_strcasecmp(s, "warn") == 0 || edb_strcasecmp(s, "warning") == 0) return EDB_LOG_WARN;
    if (edb_strcasecmp(s, "error") == 0) return EDB_LOG_ERROR;
    if (edb_strcasecmp(s, "fatal") == 0) return EDB_LOG_FATAL;
    return EDB_LOG_INFO;
}

static const char *level_name(edb_log_level_t lvl) {
    switch (lvl) {
    case EDB_LOG_TRACE: return "TRACE";
    case EDB_LOG_DEBUG: return "DEBUG";
    case EDB_LOG_INFO:  return "INFO";
    case EDB_LOG_WARN:  return "WARN";
    case EDB_LOG_ERROR: return "ERROR";
    case EDB_LOG_FATAL: return "FATAL";
    default:            return "INFO";
    }
}

void edb_log_init(const char *level_str, const char *file_path, const char *format_str) {
    ensure_log_mu();
    edb_mutex_lock(&g_log_mu);
    g_log_level = parse_log_level(level_str);
    g_log_json = (format_str && edb_strcasecmp(format_str, "json") == 0);
    if (g_log_fp && g_log_fp != stderr && g_log_fp != stdout) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    if (file_path && *file_path) {
        g_log_fp = fopen(file_path, "a");
    }
    edb_mutex_unlock(&g_log_mu);
}

void edb_log_shutdown(void) {
    if (!g_log_mu_init) return;
    edb_mutex_lock(&g_log_mu);
    if (g_log_fp && g_log_fp != stderr && g_log_fp != stdout) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    edb_mutex_unlock(&g_log_mu);
}

void edb_log_set_level(const char *level_str) {
    g_log_level = parse_log_level(level_str);
}

edb_log_level_t edb_log_get_level(void) {
    return g_log_level;
}

void edb_log_msg(edb_log_level_t lvl, const char *fmt, ...) {
    if (lvl < g_log_level) return;
    ensure_log_mu();

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_now);

    edb_mutex_lock(&g_log_mu);
    FILE *out = g_log_fp ? g_log_fp : stderr;
    if (g_log_json) {
        edb_buf_t b;
        edb_buf_init(&b);
        edb_buf_printf(&b, "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":", ts, level_name(lvl));
        edb_json_append_escaped(&b, msg, strlen(msg));
        edb_buf_append_str(&b, "}\n");
        fwrite(b.data, 1, b.len, out);
        edb_buf_free(&b);
    } else {
        fprintf(out, "%s [%s] %s\n", ts, level_name(lvl), msg);
    }
    fflush(out);
    edb_mutex_unlock(&g_log_mu);
}
