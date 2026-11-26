#include "wal.h"

static edb_wal_sync_mode_t parse_sync_mode(const char *s) {
    if (edb_strcasecmp(s, "off") == 0) return EDB_WAL_SYNC_OFF;
    if (edb_strcasecmp(s, "full") == 0) return EDB_WAL_SYNC_FULL;
    return EDB_WAL_SYNC_NORMAL;
}

bool edb_wal_open(edb_wal_t *w, const edb_config_t *cfg, edb_metrics_t *metrics, uint64_t start_lsn) {
    memset(w, 0, sizeof(*w));
    w->enabled = cfg->wal_enabled && !cfg->in_memory;
    w->in_memory = cfg->in_memory;
    w->sync_mode = parse_sync_mode(cfg->wal_sync_mode);
    w->checksum_enabled = cfg->wal_checksum_enabled;
    w->lenient_recovery = (edb_strcasecmp(cfg->wal_recovery_mode, "lenient") == 0);
    w->group_commit_ms = (uint32_t)cfg->wal_group_commit_ms;
    w->group_commit_max_ops = (uint32_t)cfg->wal_group_commit_max_ops;
    w->next_lsn = start_lsn > 0 ? start_lsn : 1;
    w->flushed_lsn = start_lsn > 0 ? start_lsn - 1 : 0;
    w->synced_lsn = w->flushed_lsn;
    w->metrics = metrics;
    edb_mutex_init(&w->mu);
    edb_cond_init(&w->sync_cv);
    edb_buf_init(&w->pending_buf);

    if (!w->enabled) return true;

    edb_mkdir_p(cfg->wal_dir);
#ifdef _WIN32
    snprintf(w->wal_path, sizeof(w->wal_path), "%s\\elliottdb.wal", cfg->wal_dir);
#else
    snprintf(w->wal_path, sizeof(w->wal_path), "%s/elliottdb.wal", cfg->wal_dir);
#endif

    if (!edb_file_open(&w->file, w->wal_path, true, false)) {
        return false;
    }
    uint64_t sz = 0;
    if (edb_file_size(&w->file, &sz)) {
        w->bytes_written = sz;
    }
    return true;
}

void edb_wal_close(edb_wal_t *w) {
    if (w->enabled) {
        edb_wal_flush(w, w->next_lsn > 0 ? w->next_lsn - 1 : 0, true);
        edb_file_close(&w->file);
    }
    edb_buf_free(&w->pending_buf);
    edb_cond_destroy(&w->sync_cv);
    edb_mutex_destroy(&w->mu);
}

static bool wal_write_pending_locked(edb_wal_t *w, bool do_sync) {
    if (w->pending_buf.len > 0) {
        uint64_t off = 0;
        if (!edb_file_append(&w->file, w->pending_buf.data, w->pending_buf.len, &off)) {
            return false;
        }
        w->bytes_written += w->pending_buf.len;
        if (w->metrics) {
            edb_atomic_add_u64(&w->metrics->wal_writes, 1);
            edb_atomic_add_u64(&w->metrics->wal_bytes, w->pending_buf.len);
        }
        edb_buf_clear(&w->pending_buf);
        w->pending_ops = 0;
        w->flushed_lsn = w->next_lsn - 1;
    }
    if (do_sync && w->synced_lsn < w->flushed_lsn) {
        if (!edb_file_sync(&w->file)) return false;
        w->synced_lsn = w->flushed_lsn;
        if (w->metrics) edb_atomic_add_u64(&w->metrics->wal_fsyncs, 1);
    }
    return true;
}

bool edb_wal_append(edb_wal_t *w, uint64_t txn_id, edb_wal_op_t op, uint32_t table_id,
                    const void *payload, uint32_t payload_len, uint64_t *out_lsn) {
    edb_mutex_lock(&w->mu);
    uint64_t lsn = w->next_lsn++;
    if (out_lsn) *out_lsn = lsn;
    if (!w->enabled) {
        edb_mutex_unlock(&w->mu);
        return true;
    }

    edb_wal_rec_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = EDB_WAL_MAGIC;
    hdr.payload_len = payload_len;
    hdr.lsn = lsn;
    hdr.txn_id = txn_id;
    hdr.table_id = table_id;
    hdr.op = (uint8_t)op;
    hdr.flags = 0;

    size_t rec_start = w->pending_buf.len;
    edb_buf_append(&w->pending_buf, &hdr, sizeof(hdr));
    if (payload_len > 0 && payload) {
        edb_buf_append(&w->pending_buf, payload, payload_len);
    }
    uint32_t crc = edb_crc32c(w->pending_buf.data + rec_start, sizeof(hdr) + payload_len);
    edb_buf_append_u32(&w->pending_buf, crc);
    w->pending_ops++;

    bool ok = true;
    if (w->sync_mode == EDB_WAL_SYNC_NORMAL || w->pending_ops >= w->group_commit_max_ops) {
        ok = wal_write_pending_locked(w, false);
    }
    edb_mutex_unlock(&w->mu);
    return ok;
}

bool edb_wal_flush(edb_wal_t *w, uint64_t target_lsn, bool force_sync) {
    if (!w->enabled) return true;
    edb_mutex_lock(&w->mu);
    if (target_lsn == 0) target_lsn = w->next_lsn > 0 ? w->next_lsn - 1 : 0;

    if (w->sync_mode == EDB_WAL_SYNC_OFF && !force_sync) {
        edb_mutex_unlock(&w->mu);
        return true;
    }

    bool need_sync = force_sync || (w->sync_mode == EDB_WAL_SYNC_FULL);
    if (!need_sync) {
        bool ok = wal_write_pending_locked(w, false);
        edb_mutex_unlock(&w->mu);
        return ok;
    }

    /* Group commit: if another thread is flushing/syncing, wait for our LSN */
    while (w->flushing && w->synced_lsn < target_lsn) {
        edb_cond_wait(&w->sync_cv, &w->mu);
    }
    if (w->synced_lsn >= target_lsn && w->pending_buf.len == 0) {
        edb_mutex_unlock(&w->mu);
        return true;
    }

    w->flushing = true;
    if (!force_sync && w->group_commit_ms > 0 && w->pending_ops < w->group_commit_max_ops) {
        edb_mutex_unlock(&w->mu);
        edb_sleep_ms(w->group_commit_ms);
        edb_mutex_lock(&w->mu);
    }
    bool ok = wal_write_pending_locked(w, true);
    w->flushing = false;
    edb_cond_broadcast(&w->sync_cv);
    edb_mutex_unlock(&w->mu);
    return ok;
}

bool edb_wal_truncate(edb_wal_t *w, uint64_t new_start_lsn) {
    if (!w->enabled) return true;
    edb_mutex_lock(&w->mu);
    edb_buf_clear(&w->pending_buf);
    w->pending_ops = 0;
    bool ok = edb_file_truncate(&w->file, 0) && edb_file_sync(&w->file);
    w->bytes_written = 0;
    if (new_start_lsn > w->next_lsn) w->next_lsn = new_start_lsn;
    w->flushed_lsn = w->next_lsn - 1;
    w->synced_lsn = w->flushed_lsn;
    edb_mutex_unlock(&w->mu);
    return ok;
}

bool edb_wal_replay(edb_wal_t *w, uint64_t min_lsn, edb_wal_replay_cb cb, void *ctx) {
    if (!w->enabled) return true;
    uint64_t fsz = 0;
    if (!edb_file_size(&w->file, &fsz) || fsz == 0) return true;

    uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
    if (!buf) return false;
    size_t got = 0;
    if (!edb_file_pread(&w->file, buf, (size_t)fsz, 0, &got)) {
        free(buf);
        return false;
    }

    size_t pos = 0;
    uint64_t max_seen_lsn = min_lsn;
    bool ok = true;
    while (pos + sizeof(edb_wal_rec_hdr_t) + 4 <= got) {
        edb_wal_rec_hdr_t hdr;
        memcpy(&hdr, buf + pos, sizeof(hdr));
        if (hdr.magic != EDB_WAL_MAGIC) {
            if (w->lenient_recovery) break;
            ok = false;
            break;
        }
        size_t rec_len = sizeof(hdr) + (size_t)hdr.payload_len + 4;
        if (pos + rec_len > got) {
            /* Partial tail record from crash */
            break;
        }
        if (w->checksum_enabled) {
            uint32_t expected_crc = 0;
            memcpy(&expected_crc, buf + pos + sizeof(hdr) + hdr.payload_len, 4);
            uint32_t actual_crc = edb_crc32c(buf + pos, sizeof(hdr) + hdr.payload_len);
            if (expected_crc != actual_crc) {
                if (w->lenient_recovery) break;
                ok = false;
                break;
            }
        }
        if (hdr.lsn > max_seen_lsn) max_seen_lsn = hdr.lsn;
        if (hdr.lsn > min_lsn) {
            const uint8_t *payload = hdr.payload_len > 0 ? (buf + pos + sizeof(hdr)) : NULL;
            if (!cb(&hdr, payload, ctx)) {
                ok = false;
                break;
            }
        }
        pos += rec_len;
    }
    if (max_seen_lsn >= w->next_lsn) {
        w->next_lsn = max_seen_lsn + 1;
    }
    free(buf);
    return ok;
}

uint64_t edb_wal_size_bytes(edb_wal_t *w) {
    if (!w->enabled) return 0;
    edb_mutex_lock(&w->mu);
    uint64_t sz = w->bytes_written + w->pending_buf.len;
    edb_mutex_unlock(&w->mu);
    return sz;
}
