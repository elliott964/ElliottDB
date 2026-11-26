#ifndef EDB_INDEX_H
#define EDB_INDEX_H

#include "edb_common.h"
#include "pager.h"

typedef enum {
    EDB_TYPE_NULL = 0,
    EDB_TYPE_INT = 1,
    EDB_TYPE_REAL = 2,
    EDB_TYPE_TEXT = 3,
    EDB_TYPE_BOOL = 4,
    EDB_TYPE_BLOB = 5,
    EDB_TYPE_TIMESTAMP = 6
} edb_type_t;

typedef struct {
    edb_type_t type;
    union {
        int64_t i64;
        double f64;
        bool b;
        struct {
            char *ptr;
            uint32_t len;
        } str;
        struct {
            uint8_t *ptr;
            uint32_t len;
        } blob;
    } v;
} edb_val_t;

const char *edb_type_name(edb_type_t t);
bool edb_type_from_str(const char *s, edb_type_t *out);
edb_val_t edb_val_null(void);
edb_val_t edb_val_int(int64_t v);
edb_val_t edb_val_real(double v);
edb_val_t edb_val_bool(bool v);
edb_val_t edb_val_text_copy(const char *s, uint32_t len);
edb_val_t edb_val_clone(const edb_val_t *src);
void edb_val_free(edb_val_t *v);
int edb_val_cmp(const edb_val_t *a, const edb_val_t *b);
uint64_t edb_val_hash(const edb_val_t *v);

typedef enum {
    EDB_IDX_BTREE = 0,
    EDB_IDX_HASH = 1
} edb_index_type_t;

typedef struct {
    edb_rid_t *items;
    size_t count;
    size_t cap;
} edb_rid_list_t;

void edb_rid_list_init(edb_rid_list_t *l);
bool edb_rid_list_push(edb_rid_list_t *l, edb_rid_t rid);
void edb_rid_list_free(edb_rid_list_t *l);

typedef struct edb_btree_node {
    bool is_leaf;
    size_t count;
    edb_val_t *keys;
    edb_rid_t *rids;             /* leaf only: count entries */
    struct edb_btree_node **children; /* internal only: count + 1 entries */
    struct edb_btree_node *next; /* leaf linked list */
    struct edb_btree_node *prev;
} edb_btree_node_t;

typedef struct {
    bool occupied;
    bool tombstone;
    uint64_t hash;
    edb_val_t key;
    edb_rid_list_t rids;
} edb_hash_slot_t;

typedef struct edb_index {
    char name[EDB_MAX_NAME];
    uint32_t col_idx;
    edb_index_type_t type;
    bool unique;
    size_t btree_order;
    size_t hash_load_factor;

    /* B+Tree state */
    edb_btree_node_t *root;

    /* Hash index state */
    edb_hash_slot_t *slots;
    size_t slot_cap;
    size_t key_count;
} edb_index_t;

edb_index_t *edb_index_create(const char *name, uint32_t col_idx, edb_index_type_t type,
                              bool unique, size_t btree_order, size_t hash_load_factor);
void edb_index_destroy(edb_index_t *idx);
void edb_index_clear(edb_index_t *idx);
bool edb_index_insert(edb_index_t *idx, const edb_val_t *key, edb_rid_t rid, bool *out_dup_err);
bool edb_index_delete(edb_index_t *idx, const edb_val_t *key, edb_rid_t rid);
bool edb_index_find_eq(const edb_index_t *idx, const edb_val_t *key, edb_rid_list_t *out);
bool edb_index_find_range(const edb_index_t *idx,
                          const edb_val_t *lo, bool lo_inc,
                          const edb_val_t *hi, bool hi_inc,
                          edb_rid_list_t *out);

#endif /* EDB_INDEX_H */
