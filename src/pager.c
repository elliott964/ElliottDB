#include "pager.h"

static uint32_t header_crc(const edb_file_header_t *h) {
    return edb_crc32c(h, offsetof(edb_file_header_t, crc32));
}

static size_t shard_index(const edb_pager_t *p, uint32_t page_id) {
    uint64_t h = edb_hash64(&page_id, sizeof(page_id));
    return (size_t)(h & p->shard_mask);
}

static edb_bp_frame_t *shard_find_frame(edb_bp_shard_t *sh, uint32_t page_id) {
    for (size_t i = 0; i < sh->frame_count; i++) {
        if (sh->frames[i].page_id == page_id) {
            return &sh->frames[i];
        }
    }
    return NULL;
}

static bool flush_frame_to_disk(edb_pager_t *p, edb_bp_frame_t *f) {
    if (!f->dirty || p->in_memory) return true;
    uint64_t off = (uint64_t)f->page_id * (uint64_t)p->page_size;
    if (!edb_file_pwrite(&p->file, f->data, p->page_size, off)) return false;
    f->dirty = false;
    return true;
}

static edb_bp_frame_t *shard_alloc_or_evict(edb_pager_t *p, edb_bp_shard_t *sh) {
    if (p->in_memory || sh->frame_count < sh->max_frames) {
        if (sh->frame_count == sh->frame_cap) {
            size_t ncap = sh->frame_cap == 0 ? 16 : sh->frame_cap * 2;
            edb_bp_frame_t *nf = (edb_bp_frame_t *)realloc(sh->frames, ncap * sizeof(edb_bp_frame_t));
            if (!nf) return NULL;
            sh->frames = nf;
            sh->frame_cap = ncap;
        }
        edb_bp_frame_t *f = &sh->frames[sh->frame_count];
        memset(f, 0, sizeof(*f));
        f->data = (uint8_t *)malloc(p->page_size);
        if (!f->data) return NULL;
        sh->frame_count++;
        return f;
    }

    /* Clock sweep eviction for unpinned frames */
    size_t n = sh->frame_count;
    for (size_t pass = 0; pass < n * 2; pass++) {
        size_t idx = sh->clock_hand % n;
        sh->clock_hand = (idx + 1) % n;
        edb_bp_frame_t *f = &sh->frames[idx];
        if (f->pin_count == 0) {
            if (f->ref_bit) {
                f->ref_bit = false;
            } else {
                if (!flush_frame_to_disk(p, f)) return NULL;
                f->page_id = EDB_INVALID_PAGE_ID;
                f->dirty = false;
                return f;
            }
        }
    }

    /* All frames pinned transiently: temporarily expand frame array */
    if (sh->frame_count == sh->frame_cap) {
        size_t ncap = sh->frame_cap * 2;
        edb_bp_frame_t *nf = (edb_bp_frame_t *)realloc(sh->frames, ncap * sizeof(edb_bp_frame_t));
        if (!nf) return NULL;
        sh->frames = nf;
        sh->frame_cap = ncap;
    }
    edb_bp_frame_t *f = &sh->frames[sh->frame_count];
    memset(f, 0, sizeof(*f));
    f->data = (uint8_t *)malloc(p->page_size);
    if (!f->data) return NULL;
    sh->frame_count++;
    return f;
}

bool edb_pager_open(edb_pager_t *p, const edb_config_t *cfg, edb_metrics_t *metrics, bool *out_new_db) {
    memset(p, 0, sizeof(*p));
    p->in_memory = cfg->in_memory;
    p->page_size = (uint32_t)cfg->page_size;
    p->shard_count = (uint32_t)cfg->buffer_pool_shards;
    if (p->shard_count == 0) p->shard_count = 64;
    p->shard_mask = p->shard_count - 1;
    p->metrics = metrics;
    edb_mutex_init(&p->alloc_mu);

    uint64_t total_bp_bytes = (uint64_t)cfg->buffer_pool_size_mb * 1024ULL * 1024ULL;
    size_t total_frames = (size_t)(total_bp_bytes / p->page_size);
    if (total_frames < p->shard_count * 4) total_frames = p->shard_count * 4;
    size_t per_shard = total_frames / p->shard_count;
    if (per_shard < 4) per_shard = 4;

    p->shards = (edb_bp_shard_t *)calloc(p->shard_count, sizeof(edb_bp_shard_t));
    if (!p->shards) return false;
    for (uint32_t i = 0; i < p->shard_count; i++) {
        edb_mutex_init(&p->shards[i].mu);
        p->shards[i].max_frames = per_shard;
    }

    if (p->in_memory) {
        p->page_count = 1; /* Page 0 reserved for header */
        if (out_new_db) *out_new_db = true;
        return true;
    }

    if (cfg->auto_create_dir) {
        edb_mkdir_p(cfg->data_dir);
    }

#ifdef _WIN32
    snprintf(p->db_path, sizeof(p->db_path), "%s\\%s", cfg->data_dir, cfg->db_filename);
#else
    snprintf(p->db_path, sizeof(p->db_path), "%s/%s", cfg->data_dir, cfg->db_filename);
#endif

    bool existed = edb_file_exists(p->db_path);
    if (!edb_file_open(&p->file, p->db_path, true, false)) {
        return false;
    }

    uint64_t fsz = 0;
    if (!edb_file_size(&p->file, &fsz)) {
        edb_file_close(&p->file);
        return false;
    }

    if (!existed || fsz < p->page_size) {
        if (out_new_db) *out_new_db = true;
        p->page_count = 1;
        edb_file_header_t hdr;
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.magic, ELLIOTTDB_MAGIC, strlen(ELLIOTTDB_MAGIC));
        hdr.format_version = 1;
        hdr.page_size = p->page_size;
        hdr.page_count = 1;
        hdr.catalog_page_id = 0;
        hdr.next_table_id = 1;
        hdr.next_txn_id = 1;
        hdr.checkpoint_lsn = 0;
        if (!edb_pager_write_header(p, &hdr)) {
            edb_file_close(&p->file);
            return false;
        }
        if (cfg->preallocate_mb > 0) {
            uint64_t pre_bytes = (uint64_t)cfg->preallocate_mb * 1024ULL * 1024ULL;
            if (pre_bytes > p->page_size) {
                uint8_t zero = 0;
                edb_file_pwrite(&p->file, &zero, 1, pre_bytes - 1);
            }
        }
    } else {
        if (out_new_db) *out_new_db = false;
        edb_file_header_t hdr;
        if (!edb_pager_read_header(p, &hdr)) {
            edb_file_close(&p->file);
            return false;
        }
        p->page_size = hdr.page_size;
        p->page_count = hdr.page_count;
    }
    return true;
}

void edb_pager_close(edb_pager_t *p) {
    if (!p->shards) return;
    edb_pager_flush_all(p, true);
    for (uint32_t i = 0; i < p->shard_count; i++) {
        edb_bp_shard_t *sh = &p->shards[i];
        for (size_t j = 0; j < sh->frame_count; j++) {
            free(sh->frames[j].data);
        }
        free(sh->frames);
        edb_mutex_destroy(&sh->mu);
    }
    free(p->shards);
    p->shards = NULL;
    if (!p->in_memory) {
        edb_file_close(&p->file);
    }
    edb_mutex_destroy(&p->alloc_mu);
}

bool edb_pager_read_header(edb_pager_t *p, edb_file_header_t *hdr) {
    if (p->in_memory) {
        memset(hdr, 0, sizeof(*hdr));
        memcpy(hdr->magic, ELLIOTTDB_MAGIC, strlen(ELLIOTTDB_MAGIC));
        hdr->format_version = 1;
        hdr->page_size = p->page_size;
        hdr->page_count = p->page_count;
        hdr->next_table_id = 1;
        hdr->next_txn_id = 1;
        return true;
    }
    uint8_t buf[4096];
    memset(buf, 0, sizeof(buf));
    size_t got = 0;
    if (!edb_file_pread(&p->file, buf, sizeof(edb_file_header_t), 0, &got) || got < sizeof(edb_file_header_t)) {
        return false;
    }
    memcpy(hdr, buf, sizeof(*hdr));
    if (memcmp(hdr->magic, ELLIOTTDB_MAGIC, strlen(ELLIOTTDB_MAGIC)) != 0) {
        return false;
    }
    if (hdr->crc32 != header_crc(hdr)) {
        return false;
    }
    return true;
}

bool edb_pager_write_header(edb_pager_t *p, const edb_file_header_t *hdr_in) {
    if (p->in_memory) return true;
    edb_file_header_t hdr = *hdr_in;
    hdr.crc32 = header_crc(&hdr);
    uint8_t *page0 = (uint8_t *)calloc(1, p->page_size);
    if (!page0) return false;
    memcpy(page0, &hdr, sizeof(hdr));
    bool ok = edb_file_pwrite(&p->file, page0, p->page_size, 0);
    free(page0);
    return ok;
}

uint8_t *edb_pager_get_page(edb_pager_t *p, uint32_t page_id) {
    if (page_id == EDB_INVALID_PAGE_ID) return NULL;
    size_t sidx = shard_index(p, page_id);
    edb_bp_shard_t *sh = &p->shards[sidx];
    edb_mutex_lock(&sh->mu);
    edb_bp_frame_t *f = shard_find_frame(sh, page_id);
    if (f) {
        f->pin_count++;
        f->ref_bit = true;
        f->last_used = ++sh->use_seq;
        if (p->metrics) edb_atomic_add_u64(&p->metrics->buffer_pool_hits, 1);
        edb_mutex_unlock(&sh->mu);
        return f->data;
    }

    if (p->in_memory) {
        edb_mutex_unlock(&sh->mu);
        return NULL;
    }

    f = shard_alloc_or_evict(p, sh);
    if (!f) {
        edb_mutex_unlock(&sh->mu);
        return NULL;
    }

    size_t got = 0;
    uint64_t off = (uint64_t)page_id * (uint64_t)p->page_size;
    memset(f->data, 0, p->page_size);
    if (!edb_file_pread(&p->file, f->data, p->page_size, off, &got)) {
        f->page_id = EDB_INVALID_PAGE_ID;
        edb_mutex_unlock(&sh->mu);
        return NULL;
    }
    f->page_id = page_id;
    f->pin_count = 1;
    f->dirty = false;
    f->ref_bit = true;
    f->last_used = ++sh->use_seq;
    if (p->metrics) edb_atomic_add_u64(&p->metrics->buffer_pool_misses, 1);
    edb_mutex_unlock(&sh->mu);
    return f->data;
}

uint8_t *edb_pager_alloc_page(edb_pager_t *p, uint32_t table_id, uint16_t page_type, uint32_t *out_page_id) {
    edb_mutex_lock(&p->alloc_mu);
    uint32_t pid = p->page_count++;
    edb_mutex_unlock(&p->alloc_mu);

    size_t sidx = shard_index(p, pid);
    edb_bp_shard_t *sh = &p->shards[sidx];
    edb_mutex_lock(&sh->mu);
    edb_bp_frame_t *f = shard_alloc_or_evict(p, sh);
    if (!f) {
        edb_mutex_unlock(&sh->mu);
        return NULL;
    }
    memset(f->data, 0, p->page_size);
    edb_page_hdr_t *hdr = (edb_page_hdr_t *)f->data;
    hdr->page_id = pid;
    hdr->table_id = table_id;
    hdr->page_type = page_type;
    hdr->slot_count = 0;
    hdr->free_start = (uint16_t)sizeof(edb_page_hdr_t);
    hdr->free_end = (uint16_t)p->page_size;
    hdr->next_page = EDB_INVALID_PAGE_ID;
    hdr->prev_page = EDB_INVALID_PAGE_ID;
    hdr->lsn = 0;

    f->page_id = pid;
    f->pin_count = 1;
    f->dirty = true;
    f->ref_bit = true;
    f->last_used = ++sh->use_seq;

    if (!p->in_memory) {
        uint64_t off = (uint64_t)pid * (uint64_t)p->page_size;
        edb_file_pwrite(&p->file, f->data, p->page_size, off);
        f->dirty = false;
    }

    edb_mutex_unlock(&sh->mu);
    if (out_page_id) *out_page_id = pid;
    return f->data;
}

void edb_pager_unpin_page(edb_pager_t *p, uint32_t page_id, bool dirty) {
    if (page_id == EDB_INVALID_PAGE_ID) return;
    size_t sidx = shard_index(p, page_id);
    edb_bp_shard_t *sh = &p->shards[sidx];
    edb_mutex_lock(&sh->mu);
    edb_bp_frame_t *f = shard_find_frame(sh, page_id);
    if (f) {
        if (dirty) f->dirty = true;
        if (f->pin_count > 0) f->pin_count--;
    }
    edb_mutex_unlock(&sh->mu);
}

bool edb_pager_flush_all(edb_pager_t *p, bool sync_disk) {
    if (p->in_memory) return true;
    bool ok = true;
    for (uint32_t i = 0; i < p->shard_count; i++) {
        edb_bp_shard_t *sh = &p->shards[i];
        edb_mutex_lock(&sh->mu);
        for (size_t j = 0; j < sh->frame_count; j++) {
            if (sh->frames[j].dirty && sh->frames[j].page_id != EDB_INVALID_PAGE_ID) {
                if (!flush_frame_to_disk(p, &sh->frames[j])) ok = false;
            }
        }
        edb_mutex_unlock(&sh->mu);
    }
    if (sync_disk && ok) {
        ok = edb_file_sync(&p->file);
    }
    return ok;
}

uint32_t edb_pager_page_count(edb_pager_t *p) {
    edb_mutex_lock(&p->alloc_mu);
    uint32_t c = p->page_count;
    edb_mutex_unlock(&p->alloc_mu);
    return c;
}
