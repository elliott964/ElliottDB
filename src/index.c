#include "index.h"
#include <math.h>

const char *edb_type_name(edb_type_t t) {
    switch (t) {
    case EDB_TYPE_NULL:      return "NULL";
    case EDB_TYPE_INT:       return "INT";
    case EDB_TYPE_REAL:      return "REAL";
    case EDB_TYPE_TEXT:      return "TEXT";
    case EDB_TYPE_BOOL:      return "BOOL";
    case EDB_TYPE_BLOB:      return "BLOB";
    case EDB_TYPE_TIMESTAMP: return "TIMESTAMP";
    default:                 return "UNKNOWN";
    }
}

bool edb_type_from_str(const char *s, edb_type_t *out) {
    if (!s) return false;
    if (edb_strcasecmp(s, "INT") == 0 || edb_strcasecmp(s, "INTEGER") == 0 ||
        edb_strcasecmp(s, "BIGINT") == 0 || edb_strcasecmp(s, "SMALLINT") == 0 ||
        edb_strcasecmp(s, "TINYINT") == 0) {
        *out = EDB_TYPE_INT;
        return true;
    }
    if (edb_strcasecmp(s, "REAL") == 0 || edb_strcasecmp(s, "FLOAT") == 0 ||
        edb_strcasecmp(s, "DOUBLE") == 0 || edb_strcasecmp(s, "NUMERIC") == 0 ||
        edb_strcasecmp(s, "DECIMAL") == 0) {
        *out = EDB_TYPE_REAL;
        return true;
    }
    if (edb_strcasecmp(s, "TEXT") == 0 || edb_strcasecmp(s, "VARCHAR") == 0 ||
        edb_strcasecmp(s, "CHAR") == 0 || edb_strcasecmp(s, "STRING") == 0) {
        *out = EDB_TYPE_TEXT;
        return true;
    }
    if (edb_strcasecmp(s, "BOOL") == 0 || edb_strcasecmp(s, "BOOLEAN") == 0) {
        *out = EDB_TYPE_BOOL;
        return true;
    }
    if (edb_strcasecmp(s, "BLOB") == 0 || edb_strcasecmp(s, "BYTES") == 0) {
        *out = EDB_TYPE_BLOB;
        return true;
    }
    if (edb_strcasecmp(s, "TIMESTAMP") == 0 || edb_strcasecmp(s, "DATETIME") == 0 ||
        edb_strcasecmp(s, "DATE") == 0) {
        *out = EDB_TYPE_TIMESTAMP;
        return true;
    }
    return false;
}

edb_val_t edb_val_null(void) {
    edb_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = EDB_TYPE_NULL;
    return v;
}

edb_val_t edb_val_int(int64_t val) {
    edb_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = EDB_TYPE_INT;
    v.v.i64 = val;
    return v;
}

edb_val_t edb_val_real(double val) {
    edb_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = EDB_TYPE_REAL;
    v.v.f64 = val;
    return v;
}

edb_val_t edb_val_bool(bool val) {
    edb_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = EDB_TYPE_BOOL;
    v.v.b = val;
    return v;
}

edb_val_t edb_val_text_copy(const char *s, uint32_t len) {
    edb_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = EDB_TYPE_TEXT;
    v.v.str.len = len;
    v.v.str.ptr = (char *)malloc(len + 1);
    if (v.v.str.ptr) {
        if (len > 0 && s) memcpy(v.v.str.ptr, s, len);
        v.v.str.ptr[len] = '\0';
    }
    return v;
}

edb_val_t edb_val_clone(const edb_val_t *src) {
    edb_val_t v = *src;
    if (src->type == EDB_TYPE_TEXT && src->v.str.ptr) {
        v.v.str.ptr = (char *)malloc(src->v.str.len + 1);
        if (v.v.str.ptr) {
            memcpy(v.v.str.ptr, src->v.str.ptr, src->v.str.len);
            v.v.str.ptr[src->v.str.len] = '\0';
        }
    } else if (src->type == EDB_TYPE_BLOB && src->v.blob.ptr) {
        v.v.blob.ptr = (uint8_t *)malloc(src->v.blob.len + 1);
        if (v.v.blob.ptr) {
            memcpy(v.v.blob.ptr, src->v.blob.ptr, src->v.blob.len);
        }
    }
    return v;
}

void edb_val_free(edb_val_t *v) {
    if (!v) return;
    if (v->type == EDB_TYPE_TEXT) {
        free(v->v.str.ptr);
        v->v.str.ptr = NULL;
    } else if (v->type == EDB_TYPE_BLOB) {
        free(v->v.blob.ptr);
        v->v.blob.ptr = NULL;
    }
    v->type = EDB_TYPE_NULL;
}

int edb_val_cmp(const edb_val_t *a, const edb_val_t *b) {
    if (a->type == EDB_TYPE_NULL && b->type == EDB_TYPE_NULL) return 0;
    if (a->type == EDB_TYPE_NULL) return -1;
    if (b->type == EDB_TYPE_NULL) return 1;

    if ((a->type == EDB_TYPE_INT || a->type == EDB_TYPE_TIMESTAMP) &&
        (b->type == EDB_TYPE_INT || b->type == EDB_TYPE_TIMESTAMP)) {
        if (a->v.i64 < b->v.i64) return -1;
        if (a->v.i64 > b->v.i64) return 1;
        return 0;
    }
    if ((a->type == EDB_TYPE_INT || a->type == EDB_TYPE_REAL) &&
        (b->type == EDB_TYPE_INT || b->type == EDB_TYPE_REAL)) {
        double da = (a->type == EDB_TYPE_INT) ? (double)a->v.i64 : a->v.f64;
        double db = (b->type == EDB_TYPE_INT) ? (double)b->v.i64 : b->v.f64;
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }
    if (a->type == EDB_TYPE_BOOL && b->type == EDB_TYPE_BOOL) {
        return (int)a->v.b - (int)b->v.b;
    }
    if (a->type == EDB_TYPE_TEXT && b->type == EDB_TYPE_TEXT) {
        uint32_t minlen = a->v.str.len < b->v.str.len ? a->v.str.len : b->v.str.len;
        int c = minlen > 0 ? memcmp(a->v.str.ptr, b->v.str.ptr, minlen) : 0;
        if (c != 0) return c;
        if (a->v.str.len < b->v.str.len) return -1;
        if (a->v.str.len > b->v.str.len) return 1;
        return 0;
    }
    if (a->type == EDB_TYPE_BLOB && b->type == EDB_TYPE_BLOB) {
        uint32_t minlen = a->v.blob.len < b->v.blob.len ? a->v.blob.len : b->v.blob.len;
        int c = minlen > 0 ? memcmp(a->v.blob.ptr, b->v.blob.ptr, minlen) : 0;
        if (c != 0) return c;
        if (a->v.blob.len < b->v.blob.len) return -1;
        if (a->v.blob.len > b->v.blob.len) return 1;
        return 0;
    }
    return (int)a->type - (int)b->type;
}

uint64_t edb_val_hash(const edb_val_t *v) {
    switch (v->type) {
    case EDB_TYPE_NULL:
        return 0x9E3779B97F4A7C15ULL;
    case EDB_TYPE_INT:
    case EDB_TYPE_TIMESTAMP: {
        int64_t x = v->v.i64;
        return edb_hash64(&x, sizeof(x));
    }
    case EDB_TYPE_REAL: {
        double d = v->v.f64;
        /* Canonicalize integer-valued doubles so INT 5 == REAL 5.0 hashes identically */
        if (floor(d) == d && d >= -9e15 && d <= 9e15) {
            int64_t x = (int64_t)d;
            return edb_hash64(&x, sizeof(x));
        }
        return edb_hash64(&d, sizeof(d));
    }
    case EDB_TYPE_BOOL: {
        uint8_t b = v->v.b ? 1 : 0;
        return edb_hash64(&b, 1);
    }
    case EDB_TYPE_TEXT:
        return edb_hash64(v->v.str.ptr ? v->v.str.ptr : "", v->v.str.len);
    case EDB_TYPE_BLOB:
        return edb_hash64(v->v.blob.ptr ? v->v.blob.ptr : (const uint8_t *)"", v->v.blob.len);
    default:
        return 0;
    }
}

void edb_rid_list_init(edb_rid_list_t *l) {
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

bool edb_rid_list_push(edb_rid_list_t *l, edb_rid_t rid) {
    if (l->count == l->cap) {
        size_t ncap = l->cap == 0 ? 8 : l->cap * 2;
        edb_rid_t *ni = (edb_rid_t *)realloc(l->items, ncap * sizeof(edb_rid_t));
        if (!ni) return false;
        l->items = ni;
        l->cap = ncap;
    }
    l->items[l->count++] = rid;
    return true;
}

void edb_rid_list_free(edb_rid_list_t *l) {
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

/* ---- B+Tree Implementation ---- */
static edb_btree_node_t *bt_alloc_node(bool is_leaf, size_t order) {
    edb_btree_node_t *n = (edb_btree_node_t *)calloc(1, sizeof(edb_btree_node_t));
    if (!n) return NULL;
    n->is_leaf = is_leaf;
    n->keys = (edb_val_t *)calloc(order + 1, sizeof(edb_val_t));
    n->rids = (edb_rid_t *)calloc(order + 1, sizeof(edb_rid_t));
    if (!is_leaf) {
        n->children = (edb_btree_node_t **)calloc(order + 2, sizeof(edb_btree_node_t *));
    }
    if (!n->keys || !n->rids || (!is_leaf && !n->children)) {
        free(n->keys);
        free(n->rids);
        free(n->children);
        free(n);
        return NULL;
    }
    return n;
}

static void bt_free_node(edb_btree_node_t *n) {
    if (!n) return;
    if (!n->is_leaf && n->children) {
        for (size_t i = 0; i <= n->count; i++) {
            bt_free_node(n->children[i]);
        }
        free(n->children);
    }
    for (size_t i = 0; i < n->count; i++) {
        edb_val_free(&n->keys[i]);
    }
    free(n->keys);
    free(n->rids);
    free(n);
}

static int bt_cmp_entry(const edb_val_t *k1, edb_rid_t r1, const edb_val_t *k2, edb_rid_t r2, bool unique) {
    int c = edb_val_cmp(k1, k2);
    if (c != 0 || unique) return c;
    if (r1 < r2) return -1;
    if (r1 > r2) return 1;
    return 0;
}

static edb_btree_node_t *bt_find_leaf(const edb_btree_node_t *root, const edb_val_t *key, edb_rid_t rid, bool unique) {
    const edb_btree_node_t *cur = root;
    while (cur && !cur->is_leaf) {
        size_t lo = 0, hi = cur->count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int c = bt_cmp_entry(key, rid, &cur->keys[mid], cur->rids[mid], unique);
            if (c < 0) hi = mid;
            else lo = mid + 1;
        }
        cur = cur->children[lo];
    }
    return (edb_btree_node_t *)cur;
}

typedef struct {
    bool split;
    edb_val_t up_key;
    edb_rid_t up_rid;
    edb_btree_node_t *right_child;
} bt_split_t;

static bool bt_insert_rec(edb_index_t *idx, edb_btree_node_t *node,
                          const edb_val_t *key, edb_rid_t rid,
                          bool *out_dup_err, bt_split_t *out_split) {
    size_t max_keys = idx->btree_order - 1;
    if (node->is_leaf) {
        size_t lo = 0, hi = node->count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int c = bt_cmp_entry(&node->keys[mid], node->rids[mid], key, rid, idx->unique);
            if (c == 0) {
                if (idx->unique && out_dup_err) *out_dup_err = true;
                return false;
            }
            if (c < 0) lo = mid + 1;
            else hi = mid;
        }
        for (size_t i = node->count; i > lo; i--) {
            node->keys[i] = node->keys[i - 1];
            node->rids[i] = node->rids[i - 1];
        }
        node->keys[lo] = edb_val_clone(key);
        node->rids[lo] = rid;
        node->count++;

        if (node->count <= max_keys) {
            out_split->split = false;
            return true;
        }

        size_t mid = node->count / 2;
        edb_btree_node_t *right = bt_alloc_node(true, idx->btree_order);
        if (!right) return false;
        right->count = node->count - mid;
        for (size_t i = 0; i < right->count; i++) {
            right->keys[i] = node->keys[mid + i];
            right->rids[i] = node->rids[mid + i];
        }
        node->count = mid;
        right->next = node->next;
        right->prev = node;
        if (node->next) node->next->prev = right;
        node->next = right;

        out_split->split = true;
        out_split->up_key = edb_val_clone(&right->keys[0]);
        out_split->up_rid = right->rids[0];
        out_split->right_child = right;
        return true;
    }

    size_t lo = 0, hi = node->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = bt_cmp_entry(key, rid, &node->keys[mid], node->rids[mid], idx->unique);
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }

    bt_split_t child_split;
    memset(&child_split, 0, sizeof(child_split));
    if (!bt_insert_rec(idx, node->children[lo], key, rid, out_dup_err, &child_split)) {
        return false;
    }
    if (!child_split.split) {
        out_split->split = false;
        return true;
    }

    for (size_t i = node->count; i > lo; i--) {
        node->keys[i] = node->keys[i - 1];
        node->rids[i] = node->rids[i - 1];
        node->children[i + 1] = node->children[i];
    }
    node->keys[lo] = child_split.up_key;
    node->rids[lo] = child_split.up_rid;
    node->children[lo + 1] = child_split.right_child;
    node->count++;

    if (node->count <= max_keys) {
        out_split->split = false;
        return true;
    }

    size_t mid = node->count / 2;
    edb_btree_node_t *right = bt_alloc_node(false, idx->btree_order);
    if (!right) return false;
    out_split->split = true;
    out_split->up_key = node->keys[mid];
    out_split->up_rid = node->rids[mid];
    out_split->right_child = right;

    right->count = node->count - mid - 1;
    for (size_t i = 0; i < right->count; i++) {
        right->keys[i] = node->keys[mid + 1 + i];
        right->rids[i] = node->rids[mid + 1 + i];
        right->children[i] = node->children[mid + 1 + i];
    }
    right->children[right->count] = node->children[node->count];
    node->count = mid;
    return true;
}

/* ---- Hash Index Implementation ---- */
static bool hash_rehash(edb_index_t *idx, size_t new_cap) {
    edb_hash_slot_t *ns = (edb_hash_slot_t *)calloc(new_cap, sizeof(edb_hash_slot_t));
    if (!ns) return false;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < idx->slot_cap; i++) {
        edb_hash_slot_t *old = &idx->slots[i];
        if (!old->occupied || old->tombstone) continue;
        size_t pos = (size_t)(old->hash & mask);
        while (ns[pos].occupied) {
            pos = (pos + 1) & mask;
        }
        ns[pos] = *old;
    }
    free(idx->slots);
    idx->slots = ns;
    idx->slot_cap = new_cap;
    return true;
}

/* ---- Public Index API ---- */
edb_index_t *edb_index_create(const char *name, uint32_t col_idx, edb_index_type_t type,
                              bool unique, size_t btree_order, size_t hash_load_factor) {
    edb_index_t *idx = (edb_index_t *)calloc(1, sizeof(edb_index_t));
    if (!idx) return NULL;
    snprintf(idx->name, sizeof(idx->name), "%s", name ? name : "");
    idx->col_idx = col_idx;
    idx->type = type;
    idx->unique = unique;
    idx->btree_order = btree_order >= 8 ? btree_order : 64;
    idx->hash_load_factor = (hash_load_factor >= 25 && hash_load_factor <= 95) ? hash_load_factor : 75;

    if (type == EDB_IDX_BTREE) {
        idx->root = bt_alloc_node(true, idx->btree_order);
        if (!idx->root) { free(idx); return NULL; }
    } else {
        idx->slot_cap = 256;
        idx->slots = (edb_hash_slot_t *)calloc(idx->slot_cap, sizeof(edb_hash_slot_t));
        if (!idx->slots) { free(idx); return NULL; }
    }
    return idx;
}

void edb_index_clear(edb_index_t *idx) {
    if (!idx) return;
    if (idx->type == EDB_IDX_BTREE) {
        bt_free_node(idx->root);
        idx->root = bt_alloc_node(true, idx->btree_order);
    } else {
        for (size_t i = 0; i < idx->slot_cap; i++) {
            if (idx->slots[i].occupied && !idx->slots[i].tombstone) {
                edb_val_free(&idx->slots[i].key);
                edb_rid_list_free(&idx->slots[i].rids);
            }
        }
        memset(idx->slots, 0, idx->slot_cap * sizeof(edb_hash_slot_t));
        idx->key_count = 0;
    }
}

void edb_index_destroy(edb_index_t *idx) {
    if (!idx) return;
    if (idx->type == EDB_IDX_BTREE) {
        bt_free_node(idx->root);
    } else {
        for (size_t i = 0; i < idx->slot_cap; i++) {
            if (idx->slots[i].occupied && !idx->slots[i].tombstone) {
                edb_val_free(&idx->slots[i].key);
                edb_rid_list_free(&idx->slots[i].rids);
            }
        }
        free(idx->slots);
    }
    free(idx);
}

bool edb_index_insert(edb_index_t *idx, const edb_val_t *key, edb_rid_t rid, bool *out_dup_err) {
    if (out_dup_err) *out_dup_err = false;
    if (!idx || !key || key->type == EDB_TYPE_NULL) return true;

    if (idx->type == EDB_IDX_BTREE) {
        bt_split_t sp;
        memset(&sp, 0, sizeof(sp));
        if (!bt_insert_rec(idx, idx->root, key, rid, out_dup_err, &sp)) return false;
        if (sp.split) {
            edb_btree_node_t *nr = bt_alloc_node(false, idx->btree_order);
            if (!nr) return false;
            nr->keys[0] = sp.up_key;
            nr->rids[0] = sp.up_rid;
            nr->children[0] = idx->root;
            nr->children[1] = sp.right_child;
            nr->count = 1;
            idx->root = nr;
        }
        return true;
    }

    if ((idx->key_count + 1) * 100 >= idx->slot_cap * idx->hash_load_factor) {
        if (!hash_rehash(idx, idx->slot_cap * 2)) return false;
    }
    uint64_t h = edb_val_hash(key);
    size_t mask = idx->slot_cap - 1;
    size_t pos = (size_t)(h & mask);
    size_t first_tomb = (size_t)-1;
    while (idx->slots[pos].occupied) {
        if (idx->slots[pos].tombstone) {
            if (first_tomb == (size_t)-1) first_tomb = pos;
        } else if (idx->slots[pos].hash == h && edb_val_cmp(&idx->slots[pos].key, key) == 0) {
            if (idx->unique) {
                if (out_dup_err) *out_dup_err = true;
                return false;
            }
            return edb_rid_list_push(&idx->slots[pos].rids, rid);
        }
        pos = (pos + 1) & mask;
    }
    size_t dst = (first_tomb != (size_t)-1) ? first_tomb : pos;
    idx->slots[dst].occupied = true;
    idx->slots[dst].tombstone = false;
    idx->slots[dst].hash = h;
    idx->slots[dst].key = edb_val_clone(key);
    edb_rid_list_init(&idx->slots[dst].rids);
    edb_rid_list_push(&idx->slots[dst].rids, rid);
    idx->key_count++;
    return true;
}

bool edb_index_delete(edb_index_t *idx, const edb_val_t *key, edb_rid_t rid) {
    if (!idx || !key || key->type == EDB_TYPE_NULL) return true;

    if (idx->type == EDB_IDX_BTREE) {
        edb_btree_node_t *leaf = bt_find_leaf(idx->root, key, idx->unique ? 0 : rid, idx->unique);
        if (!leaf) return true;
        for (size_t i = 0; i < leaf->count; i++) {
            int c = edb_val_cmp(&leaf->keys[i], key);
            if (c == 0 && (idx->unique || leaf->rids[i] == rid)) {
                edb_val_free(&leaf->keys[i]);
                for (size_t j = i; j + 1 < leaf->count; j++) {
                    leaf->keys[j] = leaf->keys[j + 1];
                    leaf->rids[j] = leaf->rids[j + 1];
                }
                leaf->count--;
                return true;
            }
        }
        return true;
    }

    uint64_t h = edb_val_hash(key);
    size_t mask = idx->slot_cap - 1;
    size_t pos = (size_t)(h & mask);
    while (idx->slots[pos].occupied) {
        if (!idx->slots[pos].tombstone && idx->slots[pos].hash == h &&
            edb_val_cmp(&idx->slots[pos].key, key) == 0) {
            edb_rid_list_t *rl = &idx->slots[pos].rids;
            for (size_t i = 0; i < rl->count; i++) {
                if (rl->items[i] == rid) {
                    rl->items[i] = rl->items[rl->count - 1];
                    rl->count--;
                    break;
                }
            }
            if (rl->count == 0) {
                edb_val_free(&idx->slots[pos].key);
                edb_rid_list_free(rl);
                idx->slots[pos].tombstone = true;
                if (idx->key_count > 0) idx->key_count--;
            }
            return true;
        }
        pos = (pos + 1) & mask;
    }
    return true;
}

bool edb_index_find_eq(const edb_index_t *idx, const edb_val_t *key, edb_rid_list_t *out) {
    if (!idx || !key || key->type == EDB_TYPE_NULL) return true;
    if (idx->type == EDB_IDX_HASH) {
        uint64_t h = edb_val_hash(key);
        size_t mask = idx->slot_cap - 1;
        size_t pos = (size_t)(h & mask);
        while (idx->slots[pos].occupied) {
            if (!idx->slots[pos].tombstone && idx->slots[pos].hash == h &&
                edb_val_cmp(&idx->slots[pos].key, key) == 0) {
                for (size_t i = 0; i < idx->slots[pos].rids.count; i++) {
                    edb_rid_list_push(out, idx->slots[pos].rids.items[i]);
                }
                return true;
            }
            pos = (pos + 1) & mask;
        }
        return true;
    }

    return edb_index_find_range(idx, key, true, key, true, out);
}

bool edb_index_find_range(const edb_index_t *idx,
                          const edb_val_t *lo, bool lo_inc,
                          const edb_val_t *hi, bool hi_inc,
                          edb_rid_list_t *out) {
    if (!idx) return false;
    if (idx->type != EDB_IDX_BTREE) {
        /* Fallback scan of hash table slots if range requested on hash index */
        for (size_t i = 0; i < idx->slot_cap; i++) {
            if (!idx->slots[i].occupied || idx->slots[i].tombstone) continue;
            const edb_val_t *k = &idx->slots[i].key;
            if (lo) {
                int c = edb_val_cmp(k, lo);
                if (c < 0 || (!lo_inc && c == 0)) continue;
            }
            if (hi) {
                int c = edb_val_cmp(k, hi);
                if (c > 0 || (!hi_inc && c == 0)) continue;
            }
            for (size_t j = 0; j < idx->slots[i].rids.count; j++) {
                edb_rid_list_push(out, idx->slots[i].rids.items[j]);
            }
        }
        return true;
    }

    edb_btree_node_t *leaf = NULL;
    if (lo) {
        leaf = bt_find_leaf(idx->root, lo, 0, true);
    } else {
        edb_btree_node_t *cur = idx->root;
        while (cur && !cur->is_leaf) cur = cur->children[0];
        leaf = cur;
    }

    while (leaf) {
        for (size_t i = 0; i < leaf->count; i++) {
            const edb_val_t *k = &leaf->keys[i];
            if (lo) {
                int c = edb_val_cmp(k, lo);
                if (c < 0 || (!lo_inc && c == 0)) continue;
            }
            if (hi) {
                int c = edb_val_cmp(k, hi);
                if (c > 0 || (!hi_inc && c == 0)) return true;
            }
            edb_rid_list_push(out, leaf->rids[i]);
        }
        leaf = leaf->next;
    }
    return true;
}
