#ifndef EDB_STORAGE_H
#define EDB_STORAGE_H

#include "config.h"
#include "index.h"
#include "metrics.h"
#include "pager.h"
#include "wal.h"

#define EDB_MAX_COLS 128

typedef struct {
    char name[EDB_MAX_NAME];
    edb_type_t type;
    bool not_null;
    bool primary_key;
    bool auto_increment;
    bool unique;
    bool has_default;
    edb_val_t default_val;
} edb_col_def_t;

typedef struct {
    uint32_t col_count;
    edb_val_t *vals;
} edb_row_t;

typedef struct edb_table {
    uint32_t id;
    char name[EDB_MAX_NAME];
    uint32_t col_count;
    edb_col_def_t *cols;
    int32_t pk_col;
    uint32_t first_page;
    uint32_t last_page;
    uint64_t row_count;
    int64_t next_auto_inc;
    edb_atomic_u64 version; /* Incremented on every write for O(1) cache invalidation */

    edb_index_t **indexes;
    uint32_t index_count;

    edb_rwlock_t lock;
    bool dropped;
} edb_table_t;

typedef enum {
    EDB_UNDO_INSERT = 1,
    EDB_UNDO_DELETE = 2,
    EDB_UNDO_UPDATE = 3
} edb_undo_type_t;

typedef struct {
    edb_undo_type_t type;
    uint32_t table_id;
    edb_rid_t rid;
    edb_row_t old_row;
} edb_undo_entry_t;

typedef struct {
    uint64_t txn_id;
    edb_undo_entry_t *entries;
    size_t count;
    size_t cap;
} edb_txn_t;

typedef struct {
    edb_config_t *cfg;
    edb_metrics_t *metrics;
    edb_pager_t pager;
    edb_wal_t wal;

    edb_rwlock_t catalog_lock;
    edb_table_t **tables;
    size_t table_count;
    size_t table_cap;
    uint32_t next_table_id;
    uint32_t catalog_page_id;
    uint64_t checkpoint_lsn;
    edb_atomic_u64 next_txn_id;
    edb_atomic_u64 schema_version;
} edb_db_t;

/* Row helpers */
void edb_row_init(edb_row_t *r, uint32_t col_count);
void edb_row_free(edb_row_t *r);
edb_row_t edb_row_clone(const edb_row_t *src);

/* Database lifecycle */
edb_status_t edb_db_open(edb_db_t *db, edb_config_t *cfg, edb_metrics_t *metrics, char *err, size_t err_cap);
void edb_db_close(edb_db_t *db);
edb_status_t edb_db_checkpoint(edb_db_t *db);
edb_status_t edb_db_vacuum(edb_db_t *db);

/* Transactions */
edb_txn_t *edb_txn_begin(edb_db_t *db);
edb_status_t edb_txn_commit(edb_db_t *db, edb_txn_t *txn);
edb_status_t edb_txn_rollback(edb_db_t *db, edb_txn_t *txn);
void edb_txn_free(edb_txn_t *txn);

/* DDL */
edb_status_t edb_create_table(edb_db_t *db, const char *name, const edb_col_def_t *cols, uint32_t col_count,
                              bool if_not_exists, char *err, size_t err_cap);
edb_status_t edb_drop_table(edb_db_t *db, const char *name, bool if_exists, char *err, size_t err_cap);
edb_status_t edb_alter_add_column(edb_db_t *db, const char *table_name, const edb_col_def_t *col,
                                  char *err, size_t err_cap);
edb_status_t edb_create_index(edb_db_t *db, const char *idx_name, const char *table_name,
                              const char *col_name, edb_index_type_t idx_type, bool unique,
                              bool if_not_exists, char *err, size_t err_cap);
edb_status_t edb_drop_index(edb_db_t *db, const char *idx_name, bool if_exists, char *err, size_t err_cap);

/* Lookup helpers (caller acquires table->lock) */
edb_table_t *edb_find_table(edb_db_t *db, const char *name);
edb_table_t *edb_find_table_by_id(edb_db_t *db, uint32_t id);
int32_t edb_table_col_index(const edb_table_t *t, const char *col_name);
edb_index_t *edb_table_find_index_for_col(const edb_table_t *t, uint32_t col_idx);

/* DML (caller holds table->lock write lock for insert/update/delete, read/write for fetch) */
edb_status_t edb_table_insert(edb_db_t *db, edb_table_t *t, edb_txn_t *txn,
                              edb_row_t *row, edb_rid_t *out_rid, int64_t *out_insert_id,
                              char *err, size_t err_cap);
edb_status_t edb_table_delete(edb_db_t *db, edb_table_t *t, edb_txn_t *txn,
                              edb_rid_t rid, char *err, size_t err_cap);
edb_status_t edb_table_update(edb_db_t *db, edb_table_t *t, edb_txn_t *txn,
                              edb_rid_t rid, const edb_row_t *new_row,
                              char *err, size_t err_cap);
bool edb_table_fetch_row(edb_db_t *db, const edb_table_t *t, edb_rid_t rid, edb_row_t *out_row);

typedef bool (*edb_scan_cb)(edb_rid_t rid, const edb_row_t *row, void *ctx);
bool edb_table_scan(edb_db_t *db, const edb_table_t *t, edb_scan_cb cb, void *ctx);

#endif /* EDB_STORAGE_H */
