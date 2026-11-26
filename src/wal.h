#ifndef EDB_WAL_H
#define EDB_WAL_H

#include "config.h"
#include "metrics.h"
#include "pal.h"

#define EDB_WAL_MAGIC 0x45444257U /* "EDBW" */

typedef enum {
    EDB_WAL_INSERT = 1,
    EDB_WAL_DELETE = 2,
    EDB_WAL_UPDATE = 3,
    EDB_WAL_CREATE_TABLE = 4,
    EDB_WAL_DROP_TABLE = 5,
    EDB_WAL_ALTER_TABLE = 6,
    EDB_WAL_CREATE_INDEX = 7,
    EDB_WAL_DROP_INDEX = 8,
    EDB_WAL_TXN_COMMIT = 9,
    EDB_WAL_TXN_ROLLBACK = 10
} edb_wal_op_t;

typedef struct {
    uint32_t magic;
    uint32_t payload_len;
    uint64_t lsn;
    uint64_t txn_id;
    uint32_t table_id;
    uint8_t op;
    uint8_t flags;
    uint16_t reserved;
} edb_wal_rec_hdr_t;

typedef enum {
    EDB_WAL_SYNC_OFF = 0,
    EDB_WAL_SYNC_NORMAL = 1,
    EDB_WAL_SYNC_FULL = 2
} edb_wal_sync_mode_t;

typedef struct {
    edb_file_t file;
    bool enabled;
    bool in_memory;
    edb_wal_sync_mode_t sync_mode;
    bool checksum_enabled;
    bool lenient_recovery;
    uint32_t group_commit_ms;
    uint32_t group_commit_max_ops;
    uint64_t next_lsn;
    uint64_t flushed_lsn;
    uint64_t synced_lsn;
    uint64_t bytes_written;
    edb_mutex_t mu;
    edb_cond_t sync_cv;
    bool flushing;
    edb_buf_t pending_buf;
    uint32_t pending_ops;
    edb_metrics_t *metrics;
    char wal_path[EDB_MAX_PATH];
} edb_wal_t;

typedef bool (*edb_wal_replay_cb)(const edb_wal_rec_hdr_t *hdr, const uint8_t *payload, void *ctx);

bool edb_wal_open(edb_wal_t *w, const edb_config_t *cfg, edb_metrics_t *metrics, uint64_t start_lsn);
void edb_wal_close(edb_wal_t *w);

bool edb_wal_append(edb_wal_t *w, uint64_t txn_id, edb_wal_op_t op, uint32_t table_id,
                    const void *payload, uint32_t payload_len, uint64_t *out_lsn);
bool edb_wal_flush(edb_wal_t *w, uint64_t target_lsn, bool force_sync);
bool edb_wal_truncate(edb_wal_t *w, uint64_t new_start_lsn);
bool edb_wal_replay(edb_wal_t *w, uint64_t min_lsn, edb_wal_replay_cb cb, void *ctx);
uint64_t edb_wal_size_bytes(edb_wal_t *w);

#endif /* EDB_WAL_H */
