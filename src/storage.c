#include "storage.h"

/* ---- Serialization Helpers ---- */
static void buf_put_str(edb_buf_t *b, const char *s) {
    uint16_t len = s ? (uint16_t)strlen(s) : 0;
    edb_buf_append_u16(b, len);
    if (len > 0) edb_buf_append(b, s, len);
}

static bool buf_read_u8(const uint8_t *p, size_t len, size_t *pos, uint8_t *out) {
    if (*pos + 1 > len) return false;
    *out = p[(*pos)++];
    return true;
}

static bool buf_read_u16(const uint8_t *p, size_t len, size_t *pos, uint16_t *out) {
    if (*pos + 2 > len) return false;
    *out = (uint16_t)p[*pos] | ((uint16_t)p[*pos + 1] << 8);
    *pos += 2;
    return true;
}

static bool buf_read_u32(const uint8_t *p, size_t len, size_t *pos, uint32_t *out) {
    if (*pos + 4 > len) return false;
    *out = (uint32_t)p[*pos] | ((uint32_t)p[*pos + 1] << 8) |
           ((uint32_t)p[*pos + 2] << 16) | ((uint32_t)p[*pos + 3] << 24);
    *pos += 4;
    return true;
}

static bool buf_read_u64(const uint8_t *p, size_t len, size_t *pos, uint64_t *out) {
    if (*pos + 8 > len) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[*pos + i]) << (i * 8);
    *pos += 8;
    *out = v;
    return true;
}

static bool buf_read_str(const uint8_t *p, size_t len, size_t *pos, char *out, size_t out_cap) {
    uint16_t slen = 0;
    if (!buf_read_u16(p, len, pos, &slen)) return false;
    if (*pos + slen > len) return false;
    size_t copy = slen < out_cap ? slen : (out_cap - 1);
    if (copy > 0) memcpy(out, p + *pos, copy);
    out[copy] = '\0';
    *pos += slen;
    return true;
}

static void serialize_val(edb_buf_t *b, const edb_val_t *v) {
    edb_buf_append_u8(b, (uint8_t)v->type);
    switch (v->type) {
    case EDB_TYPE_NULL:
        break;
    case EDB_TYPE_INT:
    case EDB_TYPE_TIMESTAMP:
        edb_buf_append_i64(b, v->v.i64);
        break;
    case EDB_TYPE_REAL:
        edb_buf_append_f64(b, v->v.f64);
        break;
    case EDB_TYPE_BOOL:
        edb_buf_append_u8(b, v->v.b ? 1 : 0);
        break;
    case EDB_TYPE_TEXT:
        edb_buf_append_u32(b, v->v.str.len);
        if (v->v.str.len > 0) edb_buf_append(b, v->v.str.ptr, v->v.str.len);
        break;
    case EDB_TYPE_BLOB:
        edb_buf_append_u32(b, v->v.blob.len);
        if (v->v.blob.len > 0) edb_buf_append(b, v->v.blob.ptr, v->v.blob.len);
        break;
    }
}

static bool deserialize_val(const uint8_t *p, size_t len, size_t *pos, edb_val_t *out) {
    uint8_t t = 0;
    if (!buf_read_u8(p, len, pos, &t)) return false;
    memset(out, 0, sizeof(*out));
    out->type = (edb_type_t)t;
    switch (out->type) {
    case EDB_TYPE_NULL:
        return true;
    case EDB_TYPE_INT:
    case EDB_TYPE_TIMESTAMP: {
        uint64_t u = 0;
        if (!buf_read_u64(p, len, pos, &u)) return false;
        out->v.i64 = (int64_t)u;
        return true;
    }
    case EDB_TYPE_REAL: {
        uint64_t u = 0;
        if (!buf_read_u64(p, len, pos, &u)) return false;
        memcpy(&out->v.f64, &u, 8);
        return true;
    }
    case EDB_TYPE_BOOL: {
        uint8_t b = 0;
        if (!buf_read_u8(p, len, pos, &b)) return false;
        out->v.b = b != 0;
        return true;
    }
    case EDB_TYPE_TEXT: {
        uint32_t sl = 0;
        if (!buf_read_u32(p, len, pos, &sl)) return false;
        if (*pos + sl > len) return false;
        *out = edb_val_text_copy((const char *)(p + *pos), sl);
        *pos += sl;
        return out->v.str.ptr != NULL;
    }
    case EDB_TYPE_BLOB: {
        uint32_t bl = 0;
        if (!buf_read_u32(p, len, pos, &bl)) return false;
        if (*pos + bl > len) return false;
        out->v.blob.len = bl;
        out->v.blob.ptr = (uint8_t *)malloc(bl + 1);
        if (!out->v.blob.ptr) return false;
        if (bl > 0) memcpy(out->v.blob.ptr, p + *pos, bl);
        *pos += bl;
        return true;
    }
    default:
        return false;
    }
}

static void serialize_row(edb_buf_t *b, const edb_row_t *r) {
    edb_buf_append_u16(b, (uint16_t)r->col_count);
    for (uint32_t i = 0; i < r->col_count; i++) {
        serialize_val(b, &r->vals[i]);
    }
}

static bool deserialize_row(const uint8_t *p, size_t len, uint32_t table_cols, edb_row_t *out) {
    size_t pos = 0;
    uint16_t cnt = 0;
    if (!buf_read_u16(p, len, &pos, &cnt)) return false;
    uint32_t alloc_cols = table_cols > cnt ? table_cols : cnt;
    edb_row_init(out, alloc_cols);
    for (uint16_t i = 0; i < cnt; i++) {
        if (!deserialize_val(p, len, &pos, &out->vals[i])) {
            edb_row_free(out);
            return false;
        }
    }
    for (uint32_t i = cnt; i < alloc_cols; i++) {
        out->vals[i] = edb_val_null();
    }
    return true;
}

void edb_row_init(edb_row_t *r, uint32_t col_count) {
    r->col_count = col_count;
    r->vals = col_count > 0 ? (edb_val_t *)calloc(col_count, sizeof(edb_val_t)) : NULL;
}

void edb_row_free(edb_row_t *r) {
    if (!r) return;
    if (r->vals) {
        for (uint32_t i = 0; i < r->col_count; i++) {
            edb_val_free(&r->vals[i]);
        }
        free(r->vals);
        r->vals = NULL;
    }
    r->col_count = 0;
}

edb_row_t edb_row_clone(const edb_row_t *src) {
    edb_row_t dst;
    edb_row_init(&dst, src->col_count);
    for (uint32_t i = 0; i < src->col_count; i++) {
        dst.vals[i] = edb_val_clone(&src->vals[i]);
    }
    return dst;
}

static void free_table_struct(edb_table_t *t) {
    if (!t) return;
    for (uint32_t i = 0; i < t->col_count; i++) {
        if (t->cols[i].has_default) edb_val_free(&t->cols[i].default_val);
    }
    free(t->cols);
    for (uint32_t i = 0; i < t->index_count; i++) {
        edb_index_destroy(t->indexes[i]);
    }
    free(t->indexes);
    edb_rwlock_destroy(&t->lock);
    free(t);
}

/* ---- Slotted Page Helpers ---- */
static bool page_insert_bytes(uint8_t *page, uint32_t page_size, const uint8_t *data, uint16_t len, uint16_t *out_slot) {
    (void)page_size;
    edb_page_hdr_t *hdr = (edb_page_hdr_t *)page;
    edb_slot_t *slots = (edb_slot_t *)(page + sizeof(edb_page_hdr_t));

    /* Check for reusable tombstone slot */
    int reuse_slot = -1;
    for (uint16_t i = 0; i < hdr->slot_count; i++) {
        if (slots[i].length == 0) {
            reuse_slot = (int)i;
            break;
        }
    }

    uint16_t needed_slot_space = (reuse_slot >= 0) ? 0 : (uint16_t)sizeof(edb_slot_t);
    if (hdr->free_end < hdr->free_start || (uint32_t)(hdr->free_end - hdr->free_start) < (uint32_t)len + needed_slot_space) {
        return false;
    }

    uint16_t new_end = (uint16_t)(hdr->free_end - len);
    memcpy(page + new_end, data, len);
    hdr->free_end = new_end;

    uint16_t sidx;
    if (reuse_slot >= 0) {
        sidx = (uint16_t)reuse_slot;
    } else {
        sidx = hdr->slot_count++;
        hdr->free_start = (uint16_t)(hdr->free_start + sizeof(edb_slot_t));
    }
    slots[sidx].offset = new_end;
    slots[sidx].length = len;
    *out_slot = sidx;
    return true;
}

static bool page_update_in_place(uint8_t *page, uint16_t slot_idx, const uint8_t *data, uint16_t len) {
    edb_page_hdr_t *hdr = (edb_page_hdr_t *)page;
    edb_slot_t *slots = (edb_slot_t *)(page + sizeof(edb_page_hdr_t));
    if (slot_idx >= hdr->slot_count || slots[slot_idx].length == 0) return false;
    if (len <= slots[slot_idx].length) {
        memcpy(page + slots[slot_idx].offset, data, len);
        slots[slot_idx].length = len;
        return true;
    }
    if (hdr->free_end >= hdr->free_start && (uint32_t)(hdr->free_end - hdr->free_start) >= len) {
        uint16_t new_end = (uint16_t)(hdr->free_end - len);
        memcpy(page + new_end, data, len);
        hdr->free_end = new_end;
        slots[slot_idx].offset = new_end;
        slots[slot_idx].length = len;
        return true;
    }
    return false;
}

/* ---- Catalog Persistence ---- */
static void serialize_catalog(edb_db_t *db, edb_buf_t *out) {
    edb_buf_append_u32(out, (uint32_t)db->table_count);
    for (size_t i = 0; i < db->table_count; i++) {
        edb_table_t *t = db->tables[i];
        edb_buf_append_u32(out, t->id);
        buf_put_str(out, t->name);
        edb_buf_append_u32(out, t->col_count);
        edb_buf_append_u32(out, (uint32_t)t->pk_col);
        edb_buf_append_u32(out, t->first_page);
        edb_buf_append_u32(out, t->last_page);
        edb_buf_append_u64(out, t->row_count);
        edb_buf_append_i64(out, t->next_auto_inc);
        for (uint32_t c = 0; c < t->col_count; c++) {
            buf_put_str(out, t->cols[c].name);
            edb_buf_append_u8(out, (uint8_t)t->cols[c].type);
            uint8_t flags = (t->cols[c].not_null ? 1 : 0) |
                            (t->cols[c].primary_key ? 2 : 0) |
                            (t->cols[c].auto_increment ? 4 : 0) |
                            (t->cols[c].unique ? 8 : 0) |
                            (t->cols[c].has_default ? 16 : 0);
            edb_buf_append_u8(out, flags);
            if (t->cols[c].has_default) serialize_val(out, &t->cols[c].default_val);
        }
        edb_buf_append_u32(out, t->index_count);
        for (uint32_t idx = 0; idx < t->index_count; idx++) {
            buf_put_str(out, t->indexes[idx]->name);
            edb_buf_append_u32(out, t->indexes[idx]->col_idx);
            edb_buf_append_u8(out, (uint8_t)t->indexes[idx]->type);
            edb_buf_append_u8(out, t->indexes[idx]->unique ? 1 : 0);
        }
    }
}

static bool write_catalog_pages(edb_db_t *db) {
    if (db->cfg->in_memory) return true;
    edb_buf_t cat;
    edb_buf_init(&cat);
    serialize_catalog(db, &cat);

    size_t usable = db->pager.page_size - sizeof(edb_page_hdr_t) - 8;
    size_t pos = 0;
    uint32_t first_pid = db->catalog_page_id;
    uint32_t prev_pid = EDB_INVALID_PAGE_ID;
    uint32_t cur_pid = first_pid;

    do {
        uint8_t *page = NULL;
        if (cur_pid == EDB_INVALID_PAGE_ID) {
            page = edb_pager_alloc_page(&db->pager, 0, EDB_PAGE_CATALOG, &cur_pid);
            if (first_pid == EDB_INVALID_PAGE_ID) first_pid = cur_pid;
        } else {
            page = edb_pager_get_page(&db->pager, cur_pid);
        }
        if (!page) { edb_buf_free(&cat); return false; }

        edb_page_hdr_t *hdr = (edb_page_hdr_t *)page;
        uint32_t next_pid = hdr->next_page;
        size_t chunk = cat.len - pos;
        if (chunk > usable) chunk = usable;

        if (prev_pid != EDB_INVALID_PAGE_ID) {
            uint8_t *pp = edb_pager_get_page(&db->pager, prev_pid);
            if (pp) {
                ((edb_page_hdr_t *)pp)->next_page = cur_pid;
                edb_pager_unpin_page(&db->pager, prev_pid, true);
            }
        }

        uint32_t total_len_u32 = (uint32_t)cat.len;
        uint32_t chunk_u32 = (uint32_t)chunk;
        memcpy(page + sizeof(edb_page_hdr_t), &total_len_u32, 4);
        memcpy(page + sizeof(edb_page_hdr_t) + 4, &chunk_u32, 4);
        if (chunk > 0) {
            memcpy(page + sizeof(edb_page_hdr_t) + 8, cat.data + pos, chunk);
        }
        pos += chunk;

        if (pos >= cat.len) {
            hdr->next_page = EDB_INVALID_PAGE_ID;
            edb_pager_unpin_page(&db->pager, cur_pid, true);
            break;
        }
        prev_pid = cur_pid;
        edb_pager_unpin_page(&db->pager, cur_pid, true);
        cur_pid = next_pid;
    } while (pos < cat.len);

    db->catalog_page_id = first_pid;
    edb_buf_free(&cat);
    return true;
}

static bool rebuild_table_indexes_cb(edb_rid_t rid, const edb_row_t *row, void *ctx) {
    edb_table_t *t = (edb_table_t *)ctx;
    for (uint32_t i = 0; i < t->index_count; i++) {
        uint32_t cidx = t->indexes[i]->col_idx;
        if (cidx < row->col_count) {
            bool dup = false;
            edb_index_insert(t->indexes[i], &row->vals[cidx], rid, &dup);
        }
    }
    return true;
}

static bool load_catalog_pages(edb_db_t *db) {
    if (db->catalog_page_id == EDB_INVALID_PAGE_ID) return true;
    edb_buf_t cat;
    edb_buf_init(&cat);
    uint32_t pid = db->catalog_page_id;
    uint32_t expected_total = 0;

    while (pid != EDB_INVALID_PAGE_ID) {
        uint8_t *page = edb_pager_get_page(&db->pager, pid);
        if (!page) { edb_buf_free(&cat); return false; }
        edb_page_hdr_t *hdr = (edb_page_hdr_t *)page;
        uint32_t total_len = 0, chunk_len = 0;
        memcpy(&total_len, page + sizeof(edb_page_hdr_t), 4);
        memcpy(&chunk_len, page + sizeof(edb_page_hdr_t) + 4, 4);
        if (expected_total == 0) expected_total = total_len;
        if (chunk_len > db->pager.page_size - sizeof(edb_page_hdr_t) - 8) {
            edb_pager_unpin_page(&db->pager, pid, false);
            edb_buf_free(&cat);
            return false;
        }
        edb_buf_append(&cat, page + sizeof(edb_page_hdr_t) + 8, chunk_len);
        uint32_t next = hdr->next_page;
        edb_pager_unpin_page(&db->pager, pid, false);
        pid = next;
    }

    if (cat.len == 0) { edb_buf_free(&cat); return true; }

    size_t pos = 0;
    uint32_t tcount = 0;
    if (!buf_read_u32(cat.data, cat.len, &pos, &tcount)) { edb_buf_free(&cat); return false; }

    for (uint32_t ti = 0; ti < tcount; ti++) {
        edb_table_t *t = (edb_table_t *)calloc(1, sizeof(edb_table_t));
        edb_rwlock_init(&t->lock);
        uint32_t pk_u32 = 0;
        buf_read_u32(cat.data, cat.len, &pos, &t->id);
        buf_read_str(cat.data, cat.len, &pos, t->name, sizeof(t->name));
        buf_read_u32(cat.data, cat.len, &pos, &t->col_count);
        buf_read_u32(cat.data, cat.len, &pos, &pk_u32);
        t->pk_col = (int32_t)pk_u32;
        buf_read_u32(cat.data, cat.len, &pos, &t->first_page);
        buf_read_u32(cat.data, cat.len, &pos, &t->last_page);
        buf_read_u64(cat.data, cat.len, &pos, &t->row_count);
        uint64_t ainc = 0;
        buf_read_u64(cat.data, cat.len, &pos, &ainc);
        t->next_auto_inc = (int64_t)ainc;

        t->cols = (edb_col_def_t *)calloc(t->col_count, sizeof(edb_col_def_t));
        for (uint32_t c = 0; c < t->col_count; c++) {
            uint8_t typ = 0, flags = 0;
            buf_read_str(cat.data, cat.len, &pos, t->cols[c].name, sizeof(t->cols[c].name));
            buf_read_u8(cat.data, cat.len, &pos, &typ);
            buf_read_u8(cat.data, cat.len, &pos, &flags);
            t->cols[c].type = (edb_type_t)typ;
            t->cols[c].not_null = (flags & 1) != 0;
            t->cols[c].primary_key = (flags & 2) != 0;
            t->cols[c].auto_increment = (flags & 4) != 0;
            t->cols[c].unique = (flags & 8) != 0;
            t->cols[c].has_default = (flags & 16) != 0;
            if (t->cols[c].has_default) deserialize_val(cat.data, cat.len, &pos, &t->cols[c].default_val);
        }

        uint32_t icount = 0;
        buf_read_u32(cat.data, cat.len, &pos, &icount);
        t->index_count = icount;
        if (icount > 0) {
            t->indexes = (edb_index_t **)calloc(icount, sizeof(edb_index_t *));
            for (uint32_t ii = 0; ii < icount; ii++) {
                char iname[EDB_MAX_NAME];
                uint32_t col_idx = 0;
                uint8_t itype = 0, uniq = 0;
                buf_read_str(cat.data, cat.len, &pos, iname, sizeof(iname));
                buf_read_u32(cat.data, cat.len, &pos, &col_idx);
                buf_read_u8(cat.data, cat.len, &pos, &itype);
                buf_read_u8(cat.data, cat.len, &pos, &uniq);
                t->indexes[ii] = edb_index_create(iname, col_idx, (edb_index_type_t)itype, uniq != 0,
                                                  (size_t)db->cfg->btree_order, (size_t)db->cfg->hash_load_factor);
            }
        }

        if (db->table_count == db->table_cap) {
            size_t nc = db->table_cap == 0 ? 16 : db->table_cap * 2;
            db->tables = (edb_table_t **)realloc(db->tables, nc * sizeof(edb_table_t *));
            db->table_cap = nc;
        }
        db->tables[db->table_count++] = t;
        if (t->id >= db->next_table_id) db->next_table_id = t->id + 1;

        if (t->index_count > 0) {
            edb_table_scan(db, t, rebuild_table_indexes_cb, t);
        }
    }

    edb_buf_free(&cat);
    return true;
}

/* ---- WAL Replay ---- */
typedef struct {
    edb_wal_rec_hdr_t hdr;
    uint8_t *payload;
} replay_rec_t;

typedef struct {
    edb_db_t *db;
    replay_rec_t *recs;
    size_t count;
    size_t cap;
    uint64_t *committed_txns;
    size_t committed_count;
    size_t committed_cap;
} replay_ctx_t;

static bool collect_wal_cb(const edb_wal_rec_hdr_t *hdr, const uint8_t *payload, void *vctx) {
    replay_ctx_t *ctx = (replay_ctx_t *)vctx;
    if (hdr->op == EDB_WAL_TXN_COMMIT) {
        if (ctx->committed_count == ctx->committed_cap) {
            size_t nc = ctx->committed_cap == 0 ? 64 : ctx->committed_cap * 2;
            ctx->committed_txns = (uint64_t *)realloc(ctx->committed_txns, nc * sizeof(uint64_t));
            ctx->committed_cap = nc;
        }
        ctx->committed_txns[ctx->committed_count++] = hdr->txn_id;
        return true;
    }
    if (hdr->op == EDB_WAL_TXN_ROLLBACK) return true;

    if (ctx->count == ctx->cap) {
        size_t nc = ctx->cap == 0 ? 128 : ctx->cap * 2;
        ctx->recs = (replay_rec_t *)realloc(ctx->recs, nc * sizeof(replay_rec_t));
        ctx->cap = nc;
    }
    replay_rec_t *r = &ctx->recs[ctx->count++];
    r->hdr = *hdr;
    if (hdr->payload_len > 0 && payload) {
        r->payload = (uint8_t *)malloc(hdr->payload_len);
        memcpy(r->payload, payload, hdr->payload_len);
    } else {
        r->payload = NULL;
    }
    return true;
}

static bool is_txn_committed(const replay_ctx_t *ctx, uint64_t txn_id) {
    if (txn_id == 0) return true;
    for (size_t i = 0; i < ctx->committed_count; i++) {
        if (ctx->committed_txns[i] == txn_id) return true;
    }
    return false;
}

static void apply_replay_record(edb_db_t *db, const replay_rec_t *rec) {
    const uint8_t *p = rec->payload;
    size_t len = rec->hdr.payload_len;
    size_t pos = 0;
    char err[128];

    switch ((edb_wal_op_t)rec->hdr.op) {
    case EDB_WAL_CREATE_TABLE: {
        char tname[EDB_MAX_NAME];
        uint32_t col_cnt = 0;
        if (!buf_read_str(p, len, &pos, tname, sizeof(tname))) return;
        if (!buf_read_u32(p, len, &pos, &col_cnt) || col_cnt > EDB_MAX_COLS) return;
        edb_col_def_t *cols = (edb_col_def_t *)calloc(col_cnt, sizeof(edb_col_def_t));
        for (uint32_t i = 0; i < col_cnt; i++) {
            uint8_t typ = 0, flags = 0;
            buf_read_str(p, len, &pos, cols[i].name, sizeof(cols[i].name));
            buf_read_u8(p, len, &pos, &typ);
            buf_read_u8(p, len, &pos, &flags);
            cols[i].type = (edb_type_t)typ;
            cols[i].not_null = (flags & 1) != 0;
            cols[i].primary_key = (flags & 2) != 0;
            cols[i].auto_increment = (flags & 4) != 0;
            cols[i].unique = (flags & 8) != 0;
            cols[i].has_default = (flags & 16) != 0;
            if (cols[i].has_default) deserialize_val(p, len, &pos, &cols[i].default_val);
        }
        /*Temporarily disable WAL during replay*/
        bool was = db->wal.enabled;
        db->wal.enabled = false;
        edb_create_table(db, tname, cols, col_cnt, true, err, sizeof(err));
        db->wal.enabled = was;
        for (uint32_t i = 0; i < col_cnt; i++) {
            if (cols[i].has_default) edb_val_free(&cols[i].default_val);
        }
        free(cols);
        break;
    }
    case EDB_WAL_DROP_TABLE: {
        char tname[EDB_MAX_NAME];
        if (!buf_read_str(p, len, &pos, tname, sizeof(tname))) return;
        bool was = db->wal.enabled;
        db->wal.enabled = false;
        edb_drop_table(db, tname, true, err, sizeof(err));
        db->wal.enabled = was;
        break;
    }
    case EDB_WAL_CREATE_INDEX: {
        char iname[EDB_MAX_NAME], tname[EDB_MAX_NAME], cname[EDB_MAX_NAME];
        uint8_t itype = 0, uniq = 0;
        buf_read_str(p, len, &pos, iname, sizeof(iname));
        buf_read_str(p, len, &pos, tname, sizeof(tname));
        buf_read_str(p, len, &pos, cname, sizeof(cname));
        buf_read_u8(p, len, &pos, &itype);
        buf_read_u8(p, len, &pos, &uniq);
        bool was = db->wal.enabled;
        db->wal.enabled = false;
        edb_create_index(db, iname, tname, cname, (edb_index_type_t)itype, uniq != 0, true, err, sizeof(err));
        db->wal.enabled = was;
        break;
    }
    case EDB_WAL_DROP_INDEX: {
        char iname[EDB_MAX_NAME];
        if (!buf_read_str(p, len, &pos, iname, sizeof(iname))) return;
        bool was = db->wal.enabled;
        db->wal.enabled = false;
        edb_drop_index(db, iname, true, err, sizeof(err));
        db->wal.enabled = was;
        break;
    }
    case EDB_WAL_INSERT: {
        edb_table_t *t = edb_find_table_by_id(db, rec->hdr.table_id);
        if (!t) return;
        uint64_t rid = 0;
        uint32_t rlen = 0;
        if (!buf_read_u64(p, len, &pos, &rid)) return;
        if (!buf_read_u32(p, len, &pos, &rlen) || pos + rlen > len) return;
        edb_row_t row;
        if (!deserialize_row(p + pos, rlen, t->col_count, &row)) return;
        /* Check if row already exists on page */
        edb_row_t existing;
        bool already = edb_table_fetch_row(db, t, rid, &existing);
        if (already) {
            edb_row_free(&existing);
            edb_row_free(&row);
            return;
        }
        bool was = db->wal.enabled;
        db->wal.enabled = false;
        edb_rid_t new_rid = 0;
        edb_table_insert(db, t, NULL, &row, &new_rid, NULL, err, sizeof(err));
        db->wal.enabled = was;
        edb_row_free(&row);
        break;
    }
    case EDB_WAL_DELETE: {
        edb_table_t *t = edb_find_table_by_id(db, rec->hdr.table_id);
        if (!t) return;
        uint64_t rid = 0;
        if (!buf_read_u64(p, len, &pos, &rid)) return;
        bool was = db->wal.enabled;
        db->wal.enabled = false;
        edb_table_delete(db, t, NULL, rid, err, sizeof(err));
        db->wal.enabled = was;
        break;
    }
    case EDB_WAL_UPDATE: {
        edb_table_t *t = edb_find_table_by_id(db, rec->hdr.table_id);
        if (!t) return;
        uint64_t rid = 0;
        uint32_t rlen = 0;
        if (!buf_read_u64(p, len, &pos, &rid)) return;
        if (!buf_read_u32(p, len, &pos, &rlen) || pos + rlen > len) return;
        edb_row_t row;
        if (!deserialize_row(p + pos, rlen, t->col_count, &row)) return;
        bool was = db->wal.enabled;
        db->wal.enabled = false;
        edb_table_update(db, t, NULL, rid, &row, err, sizeof(err));
        db->wal.enabled = was;
        edb_row_free(&row);
        break;
    }
    default:
        break;
    }
}

/* ---- Database Lifecycle ---- */
edb_status_t edb_db_open(edb_db_t *db, edb_config_t *cfg, edb_metrics_t *metrics, char *err, size_t err_cap) {
    memset(db, 0, sizeof(*db));
    db->cfg = cfg;
    db->metrics = metrics;
    db->next_table_id = 1;
    edb_rwlock_init(&db->catalog_lock);
    edb_atomic_store_u64(&db->next_txn_id, 1);
    edb_atomic_store_u64(&db->schema_version, 1);

    bool is_new = false;
    if (!edb_pager_open(&db->pager, cfg, metrics, &is_new)) {
        if (err) snprintf(err, err_cap, "Failed to open database pager in '%s'", cfg->data_dir);
        return EDB_ERR_IO;
    }

    edb_file_header_t hdr;
    if (!edb_pager_read_header(&db->pager, &hdr)) {
        if (err) snprintf(err, err_cap, "Corrupt database header");
        edb_pager_close(&db->pager);
        return EDB_ERR_IO;
    }
    db->catalog_page_id = hdr.catalog_page_id;
    db->checkpoint_lsn = hdr.checkpoint_lsn;
    if (hdr.next_table_id > db->next_table_id) db->next_table_id = hdr.next_table_id;
    if (hdr.next_txn_id > 1) edb_atomic_store_u64(&db->next_txn_id, hdr.next_txn_id);

    if (!is_new) {
        if (!load_catalog_pages(db)) {
            if (err) snprintf(err, err_cap, "Failed to load database catalog");
            edb_pager_close(&db->pager);
            return EDB_ERR_IO;
        }
    }

    if (!edb_wal_open(&db->wal, cfg, metrics, db->checkpoint_lsn + 1)) {
        if (err) snprintf(err, err_cap, "Failed to open WAL");
        edb_pager_close(&db->pager);
        return EDB_ERR_IO;
    }

    if (db->wal.enabled) {
        replay_ctx_t rctx;
        memset(&rctx, 0, sizeof(rctx));
        rctx.db = db;
        if (edb_wal_replay(&db->wal, db->checkpoint_lsn, collect_wal_cb, &rctx)) {
            for (size_t i = 0; i < rctx.count; i++) {
                if (is_txn_committed(&rctx, rctx.recs[i].hdr.txn_id)) {
                    apply_replay_record(db, &rctx.recs[i]);
                }
                free(rctx.recs[i].payload);
            }
            if (rctx.count > 0) {
                EDB_LOG_I("Recovered %zu WAL records", rctx.count);
            }
        }
        free(rctx.recs);
        free(rctx.committed_txns);
    }

    return EDB_OK;
}

edb_status_t edb_db_checkpoint(edb_db_t *db) {
    if (db->cfg->in_memory) return EDB_OK;
    edb_rwlock_wrlock(&db->catalog_lock);
    if (!write_catalog_pages(db)) {
        edb_rwlock_wrunlock(&db->catalog_lock);
        return EDB_ERR_IO;
    }
    if (!edb_pager_flush_all(&db->pager, true)) {
        edb_rwlock_wrunlock(&db->catalog_lock);
        return EDB_ERR_IO;
    }
    uint64_t ckpt_lsn = db->wal.next_lsn > 0 ? db->wal.next_lsn - 1 : 0;
    edb_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, ELLIOTTDB_MAGIC, strlen(ELLIOTTDB_MAGIC));
    hdr.format_version = 1;
    hdr.page_size = db->pager.page_size;
    hdr.page_count = edb_pager_page_count(&db->pager);
    hdr.catalog_page_id = db->catalog_page_id;
    hdr.next_table_id = db->next_table_id;
    hdr.checkpoint_lsn = ckpt_lsn;
    hdr.next_txn_id = edb_atomic_load_u64(&db->next_txn_id);
    bool ok = edb_pager_write_header(&db->pager, &hdr) && edb_file_sync(&db->pager.file);
    if (ok) {
        db->checkpoint_lsn = ckpt_lsn;
        edb_wal_truncate(&db->wal, ckpt_lsn + 1);
        if (db->metrics) edb_atomic_add_u64(&db->metrics->checkpoints, 1);
    }
    edb_rwlock_wrunlock(&db->catalog_lock);
    return ok ? EDB_OK : EDB_ERR_IO;
}

edb_status_t edb_db_vacuum(edb_db_t *db) {
    return edb_db_checkpoint(db);
}

void edb_db_close(edb_db_t *db) {
    edb_db_checkpoint(db);
    edb_wal_close(&db->wal);
    edb_pager_close(&db->pager);
    for (size_t i = 0; i < db->table_count; i++) {
        free_table_struct(db->tables[i]);
    }
    free(db->tables);
    db->tables = NULL;
    db->table_count = 0;
    edb_rwlock_destroy(&db->catalog_lock);
}

/* ---- Transactions ---- */
edb_txn_t *edb_txn_begin(edb_db_t *db) {
    edb_txn_t *txn = (edb_txn_t *)calloc(1, sizeof(edb_txn_t));
    if (!txn) return NULL;
    txn->txn_id = edb_atomic_add_u64(&db->next_txn_id, 1);
    return txn;
}

static void txn_push_undo(edb_txn_t *txn, edb_undo_type_t type, uint32_t table_id, edb_rid_t rid, const edb_row_t *old_row) {
    if (!txn) return;
    if (txn->count == txn->cap) {
        size_t nc = txn->cap == 0 ? 16 : txn->cap * 2;
        txn->entries = (edb_undo_entry_t *)realloc(txn->entries, nc * sizeof(edb_undo_entry_t));
        txn->cap = nc;
    }
    edb_undo_entry_t *e = &txn->entries[txn->count++];
    e->type = type;
    e->table_id = table_id;
    e->rid = rid;
    if (old_row) e->old_row = edb_row_clone(old_row);
    else memset(&e->old_row, 0, sizeof(e->old_row));
}

edb_status_t edb_txn_commit(edb_db_t *db, edb_txn_t *txn) {
    if (!txn) return EDB_OK;
    uint64_t lsn = 0;
    edb_wal_append(&db->wal, txn->txn_id, EDB_WAL_TXN_COMMIT, 0, NULL, 0, &lsn);
    edb_wal_flush(&db->wal, lsn, false);
    edb_txn_free(txn);
    return EDB_OK;
}

edb_status_t edb_txn_rollback(edb_db_t *db, edb_txn_t *txn) {
    if (!txn) return EDB_OK;
    char err[128];
    for (size_t i = txn->count; i > 0; i--) {
        edb_undo_entry_t *e = &txn->entries[i - 1];
        edb_rwlock_rdlock(&db->catalog_lock);
        edb_table_t *t = edb_find_table_by_id(db, e->table_id);
        edb_rwlock_rdunlock(&db->catalog_lock);
        if (!t) continue;
        edb_rwlock_wrlock(&t->lock);
        bool was = db->wal.enabled;
        db->wal.enabled = false;
        if (e->type == EDB_UNDO_INSERT) {
            edb_table_delete(db, t, NULL, e->rid, err, sizeof(err));
        } else if (e->type == EDB_UNDO_DELETE) {
            edb_rid_t dummy = 0;
            edb_table_insert(db, t, NULL, &e->old_row, &dummy, NULL, err, sizeof(err));
        } else if (e->type == EDB_UNDO_UPDATE) {
            edb_table_update(db, t, NULL, e->rid, &e->old_row, err, sizeof(err));
        }
        db->wal.enabled = was;
        edb_rwlock_wrunlock(&t->lock);
    }
    edb_wal_append(&db->wal, txn->txn_id, EDB_WAL_TXN_ROLLBACK, 0, NULL, 0, NULL);
    edb_txn_free(txn);
    return EDB_OK;
}

void edb_txn_free(edb_txn_t *txn) {
    if (!txn) return;
    for (size_t i = 0; i < txn->count; i++) {
        edb_row_free(&txn->entries[i].old_row);
    }
    free(txn->entries);
    free(txn);
}

/* ---- Table Lookup ---- */
edb_table_t *edb_find_table(edb_db_t *db, const char *name) {
    for (size_t i = 0; i < db->table_count; i++) {
        if (!db->tables[i]->dropped && edb_strcasecmp(db->tables[i]->name, name) == 0) {
            return db->tables[i];
        }
    }
    return NULL;
}

edb_table_t *edb_find_table_by_id(edb_db_t *db, uint32_t id) {
    for (size_t i = 0; i < db->table_count; i++) {
        if (!db->tables[i]->dropped && db->tables[i]->id == id) {
            return db->tables[i];
        }
    }
    return NULL;
}

int32_t edb_table_col_index(const edb_table_t *t, const char *col_name) {
    for (uint32_t i = 0; i < t->col_count; i++) {
        if (edb_strcasecmp(t->cols[i].name, col_name) == 0) return (int32_t)i;
    }
    return -1;
}

edb_index_t *edb_table_find_index_for_col(const edb_table_t *t, uint32_t col_idx) {
    for (uint32_t i = 0; i < t->index_count; i++) {
        if (t->indexes[i]->col_idx == col_idx) return t->indexes[i];
    }
    return NULL;
}

/* ---- DDL ---- */
edb_status_t edb_create_table(edb_db_t *db, const char *name, const edb_col_def_t *cols, uint32_t col_count,
                              bool if_not_exists, char *err, size_t err_cap) {
    if (col_count == 0 || col_count > EDB_MAX_COLS) {
        if (err) snprintf(err, err_cap, "Invalid column count");
        return EDB_ERR_INVALID;
    }
    edb_rwlock_wrlock(&db->catalog_lock);
    if (edb_find_table(db, name)) {
        edb_rwlock_wrunlock(&db->catalog_lock);
        if (if_not_exists) return EDB_OK;
        if (err) snprintf(err, err_cap, "Table '%s' already exists", name);
        return EDB_ERR_EXISTS;
    }

    edb_table_t *t = (edb_table_t *)calloc(1, sizeof(edb_table_t));
    t->id = db->next_table_id++;
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->col_count = col_count;
    t->cols = (edb_col_def_t *)calloc(col_count, sizeof(edb_col_def_t));
    t->pk_col = -1;
    t->next_auto_inc = 1;
    edb_atomic_store_u64(&t->version, 1);
    edb_rwlock_init(&t->lock);

    for (uint32_t i = 0; i < col_count; i++) {
        t->cols[i] = cols[i];
        if (cols[i].has_default) t->cols[i].default_val = edb_val_clone(&cols[i].default_val);
        if (cols[i].primary_key) {
            t->pk_col = (int32_t)i;
            t->cols[i].not_null = true;
            t->cols[i].unique = true;
        }
    }

    if (db->table_count == db->table_cap) {
        size_t nc = db->table_cap == 0 ? 16 : db->table_cap * 2;
        db->tables = (edb_table_t **)realloc(db->tables, nc * sizeof(edb_table_t *));
        db->table_cap = nc;
    }
    db->tables[db->table_count++] = t;
    edb_atomic_add_u64(&db->schema_version, 1);

    /* Auto-create index on PK or UNIQUE columns */
    for (uint32_t i = 0; i < col_count; i++) {
        if ((t->cols[i].primary_key && db->cfg->auto_index_pk) || t->cols[i].unique) {
            char iname[EDB_MAX_NAME];
            snprintf(iname, sizeof(iname), "idx_%s_%s", t->name, t->cols[i].name);
            edb_index_type_t itype = (edb_strcasecmp(db->cfg->default_index_type, "hash") == 0 && !t->cols[i].primary_key)
                                     ? EDB_IDX_HASH : EDB_IDX_BTREE;
            edb_index_t *idx = edb_index_create(iname, i, itype, true,
                                                (size_t)db->cfg->btree_order,
                                                (size_t)db->cfg->hash_load_factor);
            if (idx) {
                t->indexes = (edb_index_t **)realloc(t->indexes, (t->index_count + 1) * sizeof(edb_index_t *));
                t->indexes[t->index_count++] = idx;
            }
        }
    }

    if (db->wal.enabled) {
        edb_buf_t wb;
        edb_buf_init(&wb);
        buf_put_str(&wb, t->name);
        edb_buf_append_u32(&wb, col_count);
        for (uint32_t i = 0; i < col_count; i++) {
            buf_put_str(&wb, t->cols[i].name);
            edb_buf_append_u8(&wb, (uint8_t)t->cols[i].type);
            uint8_t flags = (t->cols[i].not_null ? 1 : 0) |
                            (t->cols[i].primary_key ? 2 : 0) |
                            (t->cols[i].auto_increment ? 4 : 0) |
                            (t->cols[i].unique ? 8 : 0) |
                            (t->cols[i].has_default ? 16 : 0);
            edb_buf_append_u8(&wb, flags);
            if (t->cols[i].has_default) serialize_val(&wb, &t->cols[i].default_val);
        }
        uint64_t lsn = 0;
        edb_wal_append(&db->wal, 0, EDB_WAL_CREATE_TABLE, t->id, wb.data, (uint32_t)wb.len, &lsn);
        edb_wal_flush(&db->wal, lsn, false);
        edb_buf_free(&wb);
    }

    write_catalog_pages(db);
    edb_rwlock_wrunlock(&db->catalog_lock);
    return EDB_OK;
}

edb_status_t edb_drop_table(edb_db_t *db, const char *name, bool if_exists, char *err, size_t err_cap) {
    edb_rwlock_wrlock(&db->catalog_lock);
    size_t found = (size_t)-1;
    for (size_t i = 0; i < db->table_count; i++) {
        if (edb_strcasecmp(db->tables[i]->name, name) == 0) {
            found = i;
            break;
        }
    }
    if (found == (size_t)-1) {
        edb_rwlock_wrunlock(&db->catalog_lock);
        if (if_exists) return EDB_OK;
        if (err) snprintf(err, err_cap, "Table '%s' does not exist", name);
        return EDB_ERR_NOT_FOUND;
    }

    edb_table_t *t = db->tables[found];
    if (db->wal.enabled) {
        edb_buf_t wb;
        edb_buf_init(&wb);
        buf_put_str(&wb, t->name);
        uint64_t lsn = 0;
        edb_wal_append(&db->wal, 0, EDB_WAL_DROP_TABLE, t->id, wb.data, (uint32_t)wb.len, &lsn);
        edb_wal_flush(&db->wal, lsn, false);
        edb_buf_free(&wb);
    }

    free_table_struct(t);
    db->tables[found] = db->tables[db->table_count - 1];
    db->table_count--;
    edb_atomic_add_u64(&db->schema_version, 1);
    write_catalog_pages(db);
    edb_rwlock_wrunlock(&db->catalog_lock);
    return EDB_OK;
}

edb_status_t edb_alter_add_column(edb_db_t *db, const char *table_name, const edb_col_def_t *col,
                                  char *err, size_t err_cap) {
    edb_rwlock_wrlock(&db->catalog_lock);
    edb_table_t *t = edb_find_table(db, table_name);
    if (!t) {
        edb_rwlock_wrunlock(&db->catalog_lock);
        if (err) snprintf(err, err_cap, "Table '%s' not found", table_name);
        return EDB_ERR_NOT_FOUND;
    }
    edb_rwlock_wrlock(&t->lock);
    if (edb_table_col_index(t, col->name) >= 0) {
        edb_rwlock_wrunlock(&t->lock);
        edb_rwlock_wrunlock(&db->catalog_lock);
        if (err) snprintf(err, err_cap, "Column '%s' already exists", col->name);
        return EDB_ERR_EXISTS;
    }
    t->cols = (edb_col_def_t *)realloc(t->cols, (t->col_count + 1) * sizeof(edb_col_def_t));
    t->cols[t->col_count] = *col;
    if (col->has_default) t->cols[t->col_count].default_val = edb_val_clone(&col->default_val);
    t->col_count++;
    edb_atomic_add_u64(&t->version, 1);
    edb_atomic_add_u64(&db->schema_version, 1);
    edb_rwlock_wrunlock(&t->lock);
    write_catalog_pages(db);
    edb_rwlock_wrunlock(&db->catalog_lock);
    return EDB_OK;
}

edb_status_t edb_create_index(edb_db_t *db, const char *idx_name, const char *table_name,
                              const char *col_name, edb_index_type_t idx_type, bool unique,
                              bool if_not_exists, char *err, size_t err_cap) {
    edb_rwlock_wrlock(&db->catalog_lock);
    edb_table_t *t = edb_find_table(db, table_name);
    if (!t) {
        edb_rwlock_wrunlock(&db->catalog_lock);
        if (err) snprintf(err, err_cap, "Table '%s' not found", table_name);
        return EDB_ERR_NOT_FOUND;
    }
    edb_rwlock_wrlock(&t->lock);
    for (uint32_t i = 0; i < t->index_count; i++) {
        if (edb_strcasecmp(t->indexes[i]->name, idx_name) == 0) {
            edb_rwlock_wrunlock(&t->lock);
            edb_rwlock_wrunlock(&db->catalog_lock);
            if (if_not_exists) return EDB_OK;
            if (err) snprintf(err, err_cap, "Index '%s' already exists", idx_name);
            return EDB_ERR_EXISTS;
        }
    }
    int32_t cidx = edb_table_col_index(t, col_name);
    if (cidx < 0) {
        edb_rwlock_wrunlock(&t->lock);
        edb_rwlock_wrunlock(&db->catalog_lock);
        if (err) snprintf(err, err_cap, "Column '%s' not found in table '%s'", col_name, table_name);
        return EDB_ERR_NOT_FOUND;
    }

    edb_index_t *idx = edb_index_create(idx_name, (uint32_t)cidx, idx_type, unique,
                                        (size_t)db->cfg->btree_order,
                                        (size_t)db->cfg->hash_load_factor);
    if (!idx) {
        edb_rwlock_wrunlock(&t->lock);
        edb_rwlock_wrunlock(&db->catalog_lock);
        return EDB_ERR_OOM;
    }

    /* Populate index from existing rows */
    typedef struct { edb_index_t *idx; uint32_t cidx; bool dup; } pop_ctx_t;
    /* Inline scan over pages */
    uint32_t pid = t->first_page;
    bool dup_err = false;
    while (pid != EDB_INVALID_PAGE_ID && !dup_err) {
        uint8_t *page = edb_pager_get_page(&db->pager, pid);
        if (!page) break;
        edb_page_hdr_t *hdr = (edb_page_hdr_t *)page;
        edb_slot_t *slots = (edb_slot_t *)(page + sizeof(edb_page_hdr_t));
        for (uint16_t s = 0; s < hdr->slot_count; s++) {
            if (slots[s].length == 0) continue;
            edb_row_t row;
            if (deserialize_row(page + slots[s].offset, slots[s].length, t->col_count, &row)) {
                edb_rid_t rid = EDB_MAKE_RID(pid, s);
                if (!edb_index_insert(idx, &row.vals[cidx], rid, &dup_err) && dup_err) {
                    edb_row_free(&row);
                    break;
                }
                edb_row_free(&row);
            }
        }
        uint32_t next = hdr->next_page;
        edb_pager_unpin_page(&db->pager, pid, false);
        pid = next;
    }

    if (dup_err) {
        edb_index_destroy(idx);
        edb_rwlock_wrunlock(&t->lock);
        edb_rwlock_wrunlock(&db->catalog_lock);
        if (err) snprintf(err, err_cap, "Duplicate key encountered building UNIQUE index '%s'", idx_name);
        return EDB_ERR_CONSTRAINT;
    }

    t->indexes = (edb_index_t **)realloc(t->indexes, (t->index_count + 1) * sizeof(edb_index_t *));
    t->indexes[t->index_count++] = idx;
    edb_atomic_add_u64(&t->version, 1);

    if (db->wal.enabled) {
        edb_buf_t wb;
        edb_buf_init(&wb);
        buf_put_str(&wb, idx_name);
        buf_put_str(&wb, table_name);
        buf_put_str(&wb, col_name);
        edb_buf_append_u8(&wb, (uint8_t)idx_type);
        edb_buf_append_u8(&wb, unique ? 1 : 0);
        uint64_t lsn = 0;
        edb_wal_append(&db->wal, 0, EDB_WAL_CREATE_INDEX, t->id, wb.data, (uint32_t)wb.len, &lsn);
        edb_wal_flush(&db->wal, lsn, false);
        edb_buf_free(&wb);
    }

    edb_rwlock_wrunlock(&t->lock);
    write_catalog_pages(db);
    edb_rwlock_wrunlock(&db->catalog_lock);
    return EDB_OK;
}

edb_status_t edb_drop_index(edb_db_t *db, const char *idx_name, bool if_exists, char *err, size_t err_cap) {
    edb_rwlock_wrlock(&db->catalog_lock);
    for (size_t ti = 0; ti < db->table_count; ti++) {
        edb_table_t *t = db->tables[ti];
        edb_rwlock_wrlock(&t->lock);
        for (uint32_t ii = 0; ii < t->index_count; ii++) {
            if (edb_strcasecmp(t->indexes[ii]->name, idx_name) == 0) {
                edb_index_destroy(t->indexes[ii]);
                t->indexes[ii] = t->indexes[t->index_count - 1];
                t->index_count--;
                edb_atomic_add_u64(&t->version, 1);
                if (db->wal.enabled) {
                    edb_buf_t wb;
                    edb_buf_init(&wb);
                    buf_put_str(&wb, idx_name);
                    uint64_t lsn = 0;
                    edb_wal_append(&db->wal, 0, EDB_WAL_DROP_INDEX, t->id, wb.data, (uint32_t)wb.len, &lsn);
                    edb_wal_flush(&db->wal, lsn, false);
                    edb_buf_free(&wb);
                }
                edb_rwlock_wrunlock(&t->lock);
                write_catalog_pages(db);
                edb_rwlock_wrunlock(&db->catalog_lock);
                return EDB_OK;
            }
        }
        edb_rwlock_wrunlock(&t->lock);
    }
    edb_rwlock_wrunlock(&db->catalog_lock);
    if (if_exists) return EDB_OK;
    if (err) snprintf(err, err_cap, "Index '%s' not found", idx_name);
    return EDB_ERR_NOT_FOUND;
}

/* ---- DML ---- */
static edb_status_t validate_row_constraints(edb_table_t *t, edb_row_t *row, int64_t *out_inc, char *err, size_t err_cap) {
    for (uint32_t i = 0; i < t->col_count; i++) {
        if (row->vals[i].type == EDB_TYPE_NULL) {
            if (t->cols[i].auto_increment) {
                int64_t v = t->next_auto_inc++;
                row->vals[i] = edb_val_int(v);
                if (out_inc) *out_inc = v;
            } else if (t->cols[i].has_default) {
                row->vals[i] = edb_val_clone(&t->cols[i].default_val);
            } else if (t->cols[i].not_null) {
                if (err) snprintf(err, err_cap, "Column '%s' cannot be NULL", t->cols[i].name);
                return EDB_ERR_CONSTRAINT;
            }
        } else if (t->cols[i].auto_increment && row->vals[i].type == EDB_TYPE_INT) {
            if (row->vals[i].v.i64 >= t->next_auto_inc) {
                t->next_auto_inc = row->vals[i].v.i64 + 1;
            }
            if (out_inc) *out_inc = row->vals[i].v.i64;
        }
    }
    return EDB_OK;
}

edb_status_t edb_table_insert(edb_db_t *db, edb_table_t *t, edb_txn_t *txn,
                              edb_row_t *row, edb_rid_t *out_rid, int64_t *out_insert_id,
                              char *err, size_t err_cap) {
    int64_t inc_id = 0;
    edb_status_t st = validate_row_constraints(t, row, &inc_id, err, err_cap);
    if (st != EDB_OK) return st;

    /* Check unique indexes BEFORE mutating pages */
    for (uint32_t i = 0; i < t->index_count; i++) {
        edb_index_t *idx = t->indexes[i];
        if (!idx->unique) continue;
        const edb_val_t *k = &row->vals[idx->col_idx];
        if (k->type == EDB_TYPE_NULL) continue;
        edb_rid_list_t hits;
        edb_rid_list_init(&hits);
        edb_index_find_eq(idx, k, &hits);
        bool exists = hits.count > 0;
        edb_rid_list_free(&hits);
        if (exists) {
            if (err) snprintf(err, err_cap, "UNIQUE constraint failed on column '%s'", t->cols[idx->col_idx].name);
            return EDB_ERR_CONSTRAINT;
        }
    }

    edb_buf_t rb;
    edb_buf_init(&rb);
    serialize_row(&rb, row);
    if (rb.len > db->pager.page_size - sizeof(edb_page_hdr_t) - sizeof(edb_slot_t) - 16) {
        edb_buf_free(&rb);
        if (err) snprintf(err, err_cap, "Row size (%zu bytes) exceeds page limit", rb.len);
        return EDB_ERR_INVALID;
    }

    uint32_t target_pid = t->last_page;
    uint16_t slot = 0;
    bool placed = false;

    if (target_pid != EDB_INVALID_PAGE_ID) {
        uint8_t *page = edb_pager_get_page(&db->pager, target_pid);
        if (page) {
            placed = page_insert_bytes(page, db->pager.page_size, rb.data, (uint16_t)rb.len, &slot);
            edb_pager_unpin_page(&db->pager, target_pid, placed);
        }
    }

    if (!placed) {
        uint32_t new_pid = EDB_INVALID_PAGE_ID;
        uint8_t *page = edb_pager_alloc_page(&db->pager, t->id, EDB_PAGE_DATA, &new_pid);
        if (!page) {
            edb_buf_free(&rb);
            return EDB_ERR_OOM;
        }
        edb_page_hdr_t *nhdr = (edb_page_hdr_t *)page;
        nhdr->prev_page = t->last_page;
        placed = page_insert_bytes(page, db->pager.page_size, rb.data, (uint16_t)rb.len, &slot);
        edb_pager_unpin_page(&db->pager, new_pid, true);

        if (t->last_page != EDB_INVALID_PAGE_ID) {
            uint8_t *prev = edb_pager_get_page(&db->pager, t->last_page);
            if (prev) {
                ((edb_page_hdr_t *)prev)->next_page = new_pid;
                edb_pager_unpin_page(&db->pager, t->last_page, true);
            }
        }
        if (t->first_page == EDB_INVALID_PAGE_ID) t->first_page = new_pid;
        t->last_page = new_pid;
        target_pid = new_pid;
    }

    edb_rid_t rid = EDB_MAKE_RID(target_pid, slot);
    for (uint32_t i = 0; i < t->index_count; i++) {
        bool dup = false;
        edb_index_insert(t->indexes[i], &row->vals[t->indexes[i]->col_idx], rid, &dup);
    }

    t->row_count++;
    edb_atomic_add_u64(&t->version, 1);

    if (txn) {
        txn_push_undo(txn, EDB_UNDO_INSERT, t->id, rid, NULL);
    }

    if (db->wal.enabled) {
        edb_buf_t wb;
        edb_buf_init(&wb);
        edb_buf_append_u64(&wb, rid);
        edb_buf_append_u32(&wb, (uint32_t)rb.len);
        edb_buf_append(&wb, rb.data, rb.len);
        uint64_t lsn = 0;
        edb_wal_append(&db->wal, txn ? txn->txn_id : 0, EDB_WAL_INSERT, t->id, wb.data, (uint32_t)wb.len, &lsn);
        if (!txn) edb_wal_flush(&db->wal, lsn, false);
        edb_buf_free(&wb);
    }

    edb_buf_free(&rb);
    if (out_rid) *out_rid = rid;
    if (out_insert_id) *out_insert_id = inc_id;
    return EDB_OK;
}

edb_status_t edb_table_delete(edb_db_t *db, edb_table_t *t, edb_txn_t *txn,
                              edb_rid_t rid, char *err, size_t err_cap) {
    uint32_t pid = EDB_RID_PAGE(rid);
    uint16_t slot = EDB_RID_SLOT(rid);
    uint8_t *page = edb_pager_get_page(&db->pager, pid);
    if (!page) {
        if (err) snprintf(err, err_cap, "Invalid page in RID");
        return EDB_ERR_NOT_FOUND;
    }
    edb_page_hdr_t *hdr = (edb_page_hdr_t *)page;
    edb_slot_t *slots = (edb_slot_t *)(page + sizeof(edb_page_hdr_t));
    if (slot >= hdr->slot_count || slots[slot].length == 0) {
        edb_pager_unpin_page(&db->pager, pid, false);
        return EDB_ERR_NOT_FOUND;
    }

    edb_row_t old_row;
    if (deserialize_row(page + slots[slot].offset, slots[slot].length, t->col_count, &old_row)) {
        for (uint32_t i = 0; i < t->index_count; i++) {
            edb_index_delete(t->indexes[i], &old_row.vals[t->indexes[i]->col_idx], rid);
        }
        if (txn) txn_push_undo(txn, EDB_UNDO_DELETE, t->id, rid, &old_row);
        edb_row_free(&old_row);
    }

    slots[slot].length = 0;
    edb_pager_unpin_page(&db->pager, pid, true);
    if (t->row_count > 0) t->row_count--;
    edb_atomic_add_u64(&t->version, 1);

    if (db->wal.enabled) {
        edb_buf_t wb;
        edb_buf_init(&wb);
        edb_buf_append_u64(&wb, rid);
        uint64_t lsn = 0;
        edb_wal_append(&db->wal, txn ? txn->txn_id : 0, EDB_WAL_DELETE, t->id, wb.data, (uint32_t)wb.len, &lsn);
        if (!txn) edb_wal_flush(&db->wal, lsn, false);
        edb_buf_free(&wb);
    }
    return EDB_OK;
}

edb_status_t edb_table_update(edb_db_t *db, edb_table_t *t, edb_txn_t *txn,
                              edb_rid_t rid, const edb_row_t *new_row,
                              char *err, size_t err_cap) {
    edb_row_t old_row;
    if (!edb_table_fetch_row(db, t, rid, &old_row)) {
        return EDB_ERR_NOT_FOUND;
    }

    /* Check unique index constraints for modified indexed columns */
    for (uint32_t i = 0; i < t->index_count; i++) {
        edb_index_t *idx = t->indexes[i];
        if (!idx->unique) continue;
        uint32_t ci = idx->col_idx;
        if (edb_val_cmp(&old_row.vals[ci], &new_row->vals[ci]) != 0 && new_row->vals[ci].type != EDB_TYPE_NULL) {
            edb_rid_list_t hits;
            edb_rid_list_init(&hits);
            edb_index_find_eq(idx, &new_row->vals[ci], &hits);
            bool clash = false;
            for (size_t k = 0; k < hits.count; k++) {
                if (hits.items[k] != rid) { clash = true; break; }
            }
            edb_rid_list_free(&hits);
            if (clash) {
                edb_row_free(&old_row);
                if (err) snprintf(err, err_cap, "UNIQUE constraint failed on column '%s'", t->cols[ci].name);
                return EDB_ERR_CONSTRAINT;
            }
        }
    }

    edb_buf_t rb;
    edb_buf_init(&rb);
    serialize_row(&rb, new_row);

    uint32_t pid = EDB_RID_PAGE(rid);
    uint16_t slot = EDB_RID_SLOT(rid);
    uint8_t *page = edb_pager_get_page(&db->pager, pid);
    if (!page) {
        edb_buf_free(&rb);
        edb_row_free(&old_row);
        return EDB_ERR_NOT_FOUND;
    }

    bool updated_in_place = page_update_in_place(page, slot, rb.data, (uint16_t)rb.len);
    if (updated_in_place) {
        edb_pager_unpin_page(&db->pager, pid, true);
        for (uint32_t i = 0; i < t->index_count; i++) {
            uint32_t ci = t->indexes[i]->col_idx;
            if (edb_val_cmp(&old_row.vals[ci], &new_row->vals[ci]) != 0) {
                edb_index_delete(t->indexes[i], &old_row.vals[ci], rid);
                bool dup = false;
                edb_index_insert(t->indexes[i], &new_row->vals[ci], rid, &dup);
            }
        }
        if (txn) txn_push_undo(txn, EDB_UNDO_UPDATE, t->id, rid, &old_row);
        edb_atomic_add_u64(&t->version, 1);

        if (db->wal.enabled) {
            edb_buf_t wb;
            edb_buf_init(&wb);
            edb_buf_append_u64(&wb, rid);
            edb_buf_append_u32(&wb, (uint32_t)rb.len);
            edb_buf_append(&wb, rb.data, rb.len);
            uint64_t lsn = 0;
            edb_wal_append(&db->wal, txn ? txn->txn_id : 0, EDB_WAL_UPDATE, t->id, wb.data, (uint32_t)wb.len, &lsn);
            if (!txn) edb_wal_flush(&db->wal, lsn, false);
            edb_buf_free(&wb);
        }
        edb_buf_free(&rb);
        edb_row_free(&old_row);
        return EDB_OK;
    }

    edb_pager_unpin_page(&db->pager, pid, false);
    edb_buf_free(&rb);
    edb_row_free(&old_row);

    /* Fallback: delete old + insert new */
    edb_row_t clone_new = edb_row_clone(new_row);
    edb_status_t st = edb_table_delete(db, t, txn, rid, err, err_cap);
    if (st == EDB_OK) {
        edb_rid_t nrid = 0;
        st = edb_table_insert(db, t, txn, &clone_new, &nrid, NULL, err, err_cap);
    }
    edb_row_free(&clone_new);
    return st;
}

bool edb_table_fetch_row(edb_db_t *db, const edb_table_t *t, edb_rid_t rid, edb_row_t *out_row) {
    uint32_t pid = EDB_RID_PAGE(rid);
    uint16_t slot = EDB_RID_SLOT(rid);
    uint8_t *page = edb_pager_get_page(&db->pager, pid);
    if (!page) return false;
    edb_page_hdr_t *hdr = (edb_page_hdr_t *)page;
    edb_slot_t *slots = (edb_slot_t *)(page + sizeof(edb_page_hdr_t));
    if (hdr->table_id != t->id || slot >= hdr->slot_count || slots[slot].length == 0) {
        edb_pager_unpin_page(&db->pager, pid, false);
        return false;
    }
    bool ok = deserialize_row(page + slots[slot].offset, slots[slot].length, t->col_count, out_row);
    edb_pager_unpin_page(&db->pager, pid, false);
    return ok;
}

bool edb_table_scan(edb_db_t *db, const edb_table_t *t, edb_scan_cb cb, void *ctx) {
    uint32_t pid = t->first_page;
    while (pid != EDB_INVALID_PAGE_ID) {
        uint8_t *page = edb_pager_get_page(&db->pager, pid);
        if (!page) return false;
        edb_page_hdr_t *hdr = (edb_page_hdr_t *)page;
        edb_slot_t *slots = (edb_slot_t *)(page + sizeof(edb_page_hdr_t));
        bool stop = false;
        for (uint16_t s = 0; s < hdr->slot_count; s++) {
            if (slots[s].length == 0) continue;
            edb_row_t row;
            if (deserialize_row(page + slots[s].offset, slots[s].length, t->col_count, &row)) {
                bool cont = cb(EDB_MAKE_RID(pid, s), &row, ctx);
                edb_row_free(&row);
                if (!cont) { stop = true; break; }
            }
        }
        uint32_t next = hdr->next_page;
        edb_pager_unpin_page(&db->pager, pid, false);
        if (stop) break;
        pid = next;
    }
    return true;
}
