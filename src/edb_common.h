#ifndef EDB_COMMON_H
#define EDB_COMMON_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELLIOTTDB_VERSION "1.0.0"
#define ELLIOTTDB_MAGIC "ELLIOTTDB1"
#define EDB_MAX_PATH 1024
#define EDB_MAX_NAME 128

#if defined(_MSC_VER)
#define EDB_INLINE __forceinline
#define EDB_ALIGN(n) __declspec(align(n))
#define EDB_NORETURN __declspec(noreturn)
#else
#define EDB_INLINE static inline __attribute__((always_inline))
#define EDB_ALIGN(n) __attribute__((aligned(n)))
#define EDB_NORETURN __attribute__((noreturn))
#endif

typedef enum {
    EDB_OK = 0,
    EDB_ERR_SYNTAX = 1,
    EDB_ERR_NOT_FOUND = 2,
    EDB_ERR_EXISTS = 3,
    EDB_ERR_CONSTRAINT = 4,
    EDB_ERR_IO = 5,
    EDB_ERR_OOM = 6,
    EDB_ERR_AUTH = 7,
    EDB_ERR_OVERLOAD = 8,
    EDB_ERR_INVALID = 9,
    EDB_ERR_TXN = 10,
    EDB_ERR_READONLY = 11,
    EDB_ERR_INTERNAL = 12
} edb_status_t;

const char *edb_status_str(edb_status_t st);

/* ---- Dynamic Byte Buffer ---- */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} edb_buf_t;

void edb_buf_init(edb_buf_t *b);
bool edb_buf_reserve(edb_buf_t *b, size_t extra);
bool edb_buf_append(edb_buf_t *b, const void *ptr, size_t len);
bool edb_buf_append_str(edb_buf_t *b, const char *s);
bool edb_buf_append_char(edb_buf_t *b, char c);
bool edb_buf_append_u8(edb_buf_t *b, uint8_t v);
bool edb_buf_append_u16(edb_buf_t *b, uint16_t v);
bool edb_buf_append_u32(edb_buf_t *b, uint32_t v);
bool edb_buf_append_u64(edb_buf_t *b, uint64_t v);
bool edb_buf_append_i64(edb_buf_t *b, int64_t v);
bool edb_buf_append_f64(edb_buf_t *b, double v);
bool edb_buf_printf(edb_buf_t *b, const char *fmt, ...);
void edb_buf_consume(edb_buf_t *b, size_t n);
void edb_buf_clear(edb_buf_t *b);
void edb_buf_free(edb_buf_t *b);

/* ---- Scratch Bump Arena ---- */
#define EDB_ARENA_BLOCK_SIZE (64 * 1024)

typedef struct edb_arena_block {
    struct edb_arena_block *next;
    size_t used;
    size_t cap;
    uint8_t data[1];
} edb_arena_block_t;

typedef struct {
    edb_arena_block_t *head;
    edb_arena_block_t *current;
    size_t total_allocated;
} edb_arena_t;

void edb_arena_init(edb_arena_t *a);
void *edb_arena_alloc(edb_arena_t *a, size_t size);
void *edb_arena_calloc(edb_arena_t *a, size_t count, size_t size);
char *edb_arena_strdup(edb_arena_t *a, const char *s);
char *edb_arena_strndup(edb_arena_t *a, const char *s, size_t len);
void edb_arena_reset(edb_arena_t *a);
void edb_arena_free(edb_arena_t *a);

/* ---- Hashing & CRC32C ---- */
uint32_t edb_crc32c(const void *data, size_t len);
uint64_t edb_hash64(const void *data, size_t len);
uint64_t edb_hash_str_ci(const char *s, size_t len);

/* ---- String Helpers ---- */
int edb_strcasecmp(const char *a, const char *b);
int edb_strncasecmp(const char *a, const char *b, size_t n);
char *edb_strdup(const char *s);
char *edb_strndup(const char *s, size_t n);
void edb_str_trim(char *s);
bool edb_like_match(const char *str, const char *pat, bool case_insensitive);
void edb_json_append_escaped(edb_buf_t *b, const char *s, size_t len);

/* ---- Structured Logger ---- */
typedef enum {
    EDB_LOG_TRACE = 0,
    EDB_LOG_DEBUG = 1,
    EDB_LOG_INFO = 2,
    EDB_LOG_WARN = 3,
    EDB_LOG_ERROR = 4,
    EDB_LOG_FATAL = 5
} edb_log_level_t;

void edb_log_init(const char *level_str, const char *file_path, const char *format_str);
void edb_log_shutdown(void);
void edb_log_set_level(const char *level_str);
edb_log_level_t edb_log_get_level(void);
void edb_log_msg(edb_log_level_t lvl, const char *fmt, ...);

#define EDB_LOG_T(...) do { if (edb_log_get_level() <= EDB_LOG_TRACE) edb_log_msg(EDB_LOG_TRACE, __VA_ARGS__); } while (0)
#define EDB_LOG_D(...) do { if (edb_log_get_level() <= EDB_LOG_DEBUG) edb_log_msg(EDB_LOG_DEBUG, __VA_ARGS__); } while (0)
#define EDB_LOG_I(...) do { if (edb_log_get_level() <= EDB_LOG_INFO)  edb_log_msg(EDB_LOG_INFO,  __VA_ARGS__); } while (0)
#define EDB_LOG_W(...) do { if (edb_log_get_level() <= EDB_LOG_WARN)  edb_log_msg(EDB_LOG_WARN,  __VA_ARGS__); } while (0)
#define EDB_LOG_E(...) do { if (edb_log_get_level() <= EDB_LOG_ERROR) edb_log_msg(EDB_LOG_ERROR, __VA_ARGS__); } while (0)

#endif /* EDB_COMMON_H */
