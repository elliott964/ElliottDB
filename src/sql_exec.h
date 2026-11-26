#ifndef EDB_SQL_EXEC_H
#define EDB_SQL_EXEC_H

#include "sql_ast.h"

#define EDB_CACHE_SHARDS 64

typedef struct {
    edb_status_t status;
    char err_msg[256];
    uint32_t col_count;
    char **col_names;
    edb_type_t *col_types;
    uint32_t row_count;
    uint32_t row_cap;
    edb_row_t *rows;
    uint64_t rows_affected;
    uint64_t last_insert_id;
    uint64_t elapsed_us;
    bool from_cache;
    bool is_select_like;
} edb_result_t;

typedef struct {
    char name[EDB_MAX_NAME];
    char *sql;
    size_t sql_len;
} edb_prep_stmt_t;

typedef struct {
    edb_txn_t *txn;
    edb_prep_stmt_t *preps;
    size_t prep_count;
    size_t prep_cap;
} edb_session_t;

typedef struct edb_plan_entry {
    uint64_t hash;
    char *sql;
    size_t sql_len;
    edb_arena_t arena;
    edb_stmt_t *stmt;
    uint64_t last_used;
    struct edb_plan_entry *next;
} edb_plan_entry_t;

typedef struct {
    edb_mutex_t mu;
    edb_plan_entry_t *head;
    size_t count;
    size_t max_entries;
    uint64_t seq;
} edb_plan_shard_t;

typedef struct edb_res_entry {
    uint64_t hash;
    char *sql;
    size_t sql_len;
    uint64_t schema_version;
    uint32_t table_ids[8];
    uint64_t table_versions[8];
    uint32_t table_count;
    uint64_t created_ms;
    size_t approx_bytes;
    edb_result_t result;
    struct edb_res_entry *next;
} edb_res_entry_t;

typedef struct {
    edb_mutex_t mu;
    edb_res_entry_t *head;
    size_t total_bytes;
    size_t max_bytes;
} edb_res_shard_t;

typedef struct {
    edb_db_t *db;
    edb_plan_shard_t plan_shards[EDB_CACHE_SHARDS];
    edb_res_shard_t res_shards[EDB_CACHE_SHARDS];
} edb_engine_t;

void edb_engine_init(edb_engine_t *eng, edb_db_t *db);
void edb_engine_destroy(edb_engine_t *eng);

void edb_session_init(edb_session_t *s);
void edb_session_destroy(edb_engine_t *eng, edb_session_t *s);

void edb_result_init(edb_result_t *r);
void edb_result_free(edb_result_t *r);

void edb_execute_sql(edb_engine_t *eng, edb_session_t *sess,
                     const char *sql, size_t sql_len, edb_result_t *out);

void edb_execute_sql_params(edb_engine_t *eng, edb_session_t *sess,
                            const char *sql, size_t sql_len,
                            const edb_val_t *params, size_t param_count,
                            edb_result_t *out);

void edb_result_to_json(const edb_result_t *r, edb_buf_t *out);
void edb_result_to_text(const edb_result_t *r, edb_buf_t *out);

#endif /* EDB_SQL_EXEC_H */
