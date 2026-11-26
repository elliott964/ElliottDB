#ifndef EDB_PAGER_H
#define EDB_PAGER_H

#include "config.h"
#include "metrics.h"
#include "pal.h"

#define EDB_INVALID_PAGE_ID 0U

typedef enum {
    EDB_PAGE_FREE = 0,
    EDB_PAGE_HEADER = 1,
    EDB_PAGE_CATALOG = 2,
    EDB_PAGE_DATA = 3
} edb_page_type_t;

typedef struct {
    char magic[16];          /* "ELLIOTTDB1\0..." */
    uint32_t format_version; /* 1 */
    uint32_t page_size;
    uint32_t page_count;
    uint32_t catalog_page_id;
    uint32_t next_table_id;
    uint32_t reserved;
    uint64_t checkpoint_lsn;
    uint64_t next_txn_id;
    uint32_t crc32;
    uint8_t pad[16];
} edb_file_header_t;

typedef struct {
    uint32_t page_id;
    uint32_t table_id;
    uint16_t page_type;
    uint16_t slot_count;
    uint16_t free_start;
    uint16_t free_end;
    uint32_t next_page;
    uint32_t prev_page;
    uint64_t lsn;
} edb_page_hdr_t;

typedef struct {
    uint16_t offset;
    uint16_t length; /* 0 = deleted tombstone */
} edb_slot_t;

typedef uint64_t edb_rid_t;
#define EDB_MAKE_RID(pid, slot) (((uint64_t)(pid) << 16) | (uint64_t)(slot))
#define EDB_RID_PAGE(rid) ((uint32_t)((rid) >> 16))
#define EDB_RID_SLOT(rid) ((uint16_t)((rid) & 0xFFFFU))
#define EDB_INVALID_RID 0ULL

typedef struct {
    uint32_t page_id;
    uint32_t pin_count;
    bool dirty;
    bool ref_bit;
    uint64_t last_used;
    uint8_t *data;
} edb_bp_frame_t;

typedef struct {
    edb_mutex_t mu;
    edb_bp_frame_t *frames;
    size_t frame_count;
    size_t frame_cap;
    size_t max_frames;
    size_t clock_hand;
    uint64_t use_seq;
} edb_bp_shard_t;

typedef struct {
    edb_file_t file;
    bool in_memory;
    uint32_t page_size;
    uint32_t shard_count;
    uint32_t shard_mask;
    edb_bp_shard_t *shards;
    edb_mutex_t alloc_mu;
    uint32_t page_count;
    edb_metrics_t *metrics;
    char db_path[EDB_MAX_PATH];
} edb_pager_t;

bool edb_pager_open(edb_pager_t *p, const edb_config_t *cfg, edb_metrics_t *metrics, bool *out_new_db);
void edb_pager_close(edb_pager_t *p);

bool edb_pager_read_header(edb_pager_t *p, edb_file_header_t *hdr);
bool edb_pager_write_header(edb_pager_t *p, const edb_file_header_t *hdr);

uint8_t *edb_pager_get_page(edb_pager_t *p, uint32_t page_id);
uint8_t *edb_pager_alloc_page(edb_pager_t *p, uint32_t table_id, uint16_t page_type, uint32_t *out_page_id);
void edb_pager_unpin_page(edb_pager_t *p, uint32_t page_id, bool dirty);
bool edb_pager_flush_all(edb_pager_t *p, bool sync_disk);
uint32_t edb_pager_page_count(edb_pager_t *p);

#endif /* EDB_PAGER_H */
