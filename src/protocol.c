#include "protocol.h"
#include <ctype.h>

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (i * 8);
    return v;
}

void edb_bin_encode_frame(edb_buf_t *out, edb_bin_op_t op, uint16_t flags, const void *payload, uint32_t len) {
    uint8_t hdr[EDB_BIN_HDR_SIZE];
    hdr[0] = EDB_BIN_REQ_MAGIC;
    hdr[1] = (uint8_t)op;
    hdr[2] = (uint8_t)(flags & 0xFF);
    hdr[3] = (uint8_t)((flags >> 8) & 0xFF);
    hdr[4] = (uint8_t)(len & 0xFF);
    hdr[5] = (uint8_t)((len >> 8) & 0xFF);
    hdr[6] = (uint8_t)((len >> 16) & 0xFF);
    hdr[7] = (uint8_t)((len >> 24) & 0xFF);
    edb_buf_append(out, hdr, EDB_BIN_HDR_SIZE);
    if (len > 0 && payload) edb_buf_append(out, payload, len);
}

static void encode_bin_resp_hdr(edb_buf_t *out, uint8_t status, uint16_t flags, const uint8_t *body, uint32_t len) {
    uint8_t hdr[EDB_BIN_HDR_SIZE];
    hdr[0] = EDB_BIN_RESP_MAGIC;
    hdr[1] = status;
    hdr[2] = (uint8_t)(flags & 0xFF);
    hdr[3] = (uint8_t)((flags >> 8) & 0xFF);
    hdr[4] = (uint8_t)(len & 0xFF);
    hdr[5] = (uint8_t)((len >> 8) & 0xFF);
    hdr[6] = (uint8_t)((len >> 16) & 0xFF);
    hdr[7] = (uint8_t)((len >> 24) & 0xFF);
    edb_buf_append(out, hdr, EDB_BIN_HDR_SIZE);
    if (len > 0 && body) edb_buf_append(out, body, len);
}

static void encode_result_binary(const edb_result_t *r, edb_buf_t *out) {
    edb_buf_t body;
    edb_buf_init(&body);
    edb_buf_append_u64(&body, r->rows_affected);
    edb_buf_append_u64(&body, r->last_insert_id);
    edb_buf_append_u64(&body, r->elapsed_us);
    if (r->status != EDB_OK) {
        uint16_t elen = (uint16_t)strlen(r->err_msg);
        edb_buf_append_u16(&body, elen);
        edb_buf_append(&body, r->err_msg, elen);
    } else if (r->is_select_like) {
        edb_buf_append_u32(&body, r->col_count);
        edb_buf_append_u32(&body, r->row_count);
        for (uint32_t c = 0; c < r->col_count; c++) {
            uint16_t nl = (uint16_t)strlen(r->col_names[c]);
            edb_buf_append_u16(&body, nl);
            edb_buf_append(&body, r->col_names[c], nl);
            edb_buf_append_u8(&body, (uint8_t)r->col_types[c]);
        }
        for (uint32_t i = 0; i < r->row_count; i++) {
            for (uint32_t c = 0; c < r->col_count; c++) {
                const edb_val_t *v = &r->rows[i].vals[c];
                edb_buf_append_u8(&body, (uint8_t)v->type);
                switch (v->type) {
                case EDB_TYPE_NULL: break;
                case EDB_TYPE_INT:
                case EDB_TYPE_TIMESTAMP:
                    edb_buf_append_i64(&body, v->v.i64);
                    break;
                case EDB_TYPE_REAL:
                    edb_buf_append_f64(&body, v->v.f64);
                    break;
                case EDB_TYPE_BOOL:
                    edb_buf_append_u8(&body, v->v.b ? 1 : 0);
                    break;
                case EDB_TYPE_TEXT:
                    edb_buf_append_u32(&body, v->v.str.len);
                    if (v->v.str.len > 0) edb_buf_append(&body, v->v.str.ptr, v->v.str.len);
                    break;
                case EDB_TYPE_BLOB:
                    edb_buf_append_u32(&body, v->v.blob.len);
                    if (v->v.blob.len > 0) edb_buf_append(&body, v->v.blob.ptr, v->v.blob.len);
                    break;
                }
            }
        }
    }
    uint16_t flags = (r->is_select_like ? 1 : 0) | (r->from_cache ? 2 : 0);
    encode_bin_resp_hdr(out, (uint8_t)r->status, flags, body.data, (uint32_t)body.len);
    edb_buf_free(&body);
}

bool edb_bin_decode_response(const uint8_t *payload, size_t len, uint8_t status, uint16_t flags, edb_result_t *out) {
    edb_result_init(out);
    out->status = (edb_status_t)status;
    out->is_select_like = (flags & 1) != 0;
    out->from_cache = (flags & 2) != 0;
    if (len < 24) return false;
    size_t pos = 0;
    out->rows_affected = read_u64_le(payload + pos); pos += 8;
    out->last_insert_id = read_u64_le(payload + pos); pos += 8;
    out->elapsed_us = read_u64_le(payload + pos); pos += 8;
    if (out->status != EDB_OK) {
        if (pos + 2 > len) return false;
        uint16_t elen = read_u16_le(payload + pos); pos += 2;
        if (pos + elen > len) return false;
        size_t copy = elen < sizeof(out->err_msg) ? elen : (sizeof(out->err_msg) - 1);
        memcpy(out->err_msg, payload + pos, copy);
        out->err_msg[copy] = '\0';
        return true;
    }
    if (!out->is_select_like) return true;
    if (pos + 8 > len) return false;
    out->col_count = read_u32_le(payload + pos); pos += 4;
    out->row_count = read_u32_le(payload + pos); pos += 4;
    out->row_cap = out->row_count;
    out->col_names = (char **)calloc(out->col_count, sizeof(char *));
    out->col_types = (edb_type_t *)calloc(out->col_count, sizeof(edb_type_t));
    for (uint32_t c = 0; c < out->col_count; c++) {
        if (pos + 2 > len) return false;
        uint16_t nl = read_u16_le(payload + pos); pos += 2;
        if (pos + nl + 1 > len) return false;
        out->col_names[c] = edb_strndup((const char *)(payload + pos), nl);
        pos += nl;
        out->col_types[c] = (edb_type_t)payload[pos++];
    }
    out->rows = out->row_count > 0 ? (edb_row_t *)calloc(out->row_count, sizeof(edb_row_t)) : NULL;
    for (uint32_t r = 0; r < out->row_count; r++) {
        edb_row_init(&out->rows[r], out->col_count);
        for (uint32_t c = 0; c < out->col_count; c++) {
            if (pos >= len) return false;
            edb_val_t *v = &out->rows[r].vals[c];
            v->type = (edb_type_t)payload[pos++];
            switch (v->type) {
            case EDB_TYPE_NULL: break;
            case EDB_TYPE_INT:
            case EDB_TYPE_TIMESTAMP:
                if (pos + 8 > len) return false;
                v->v.i64 = (int64_t)read_u64_le(payload + pos); pos += 8;
                break;
            case EDB_TYPE_REAL: {
                if (pos + 8 > len) return false;
                uint64_t u = read_u64_le(payload + pos); pos += 8;
                memcpy(&v->v.f64, &u, 8);
                break;
            }
            case EDB_TYPE_BOOL:
                if (pos + 1 > len) return false;
                v->v.b = payload[pos++] != 0;
                break;
            case EDB_TYPE_TEXT: {
                if (pos + 4 > len) return false;
                uint32_t sl = read_u32_le(payload + pos); pos += 4;
                if (pos + sl > len) return false;
                *v = edb_val_text_copy((const char *)(payload + pos), sl);
                pos += sl;
                break;
            }
            case EDB_TYPE_BLOB: {
                if (pos + 4 > len) return false;
                uint32_t bl = read_u32_le(payload + pos); pos += 4;
                if (pos + bl > len) return false;
                v->v.blob.len = bl;
                v->v.blob.ptr = (uint8_t *)malloc(bl + 1);
                if (bl > 0) memcpy(v->v.blob.ptr, payload + pos, bl);
                pos += bl;
                break;
            }
            }
        }
    }
    return true;
}

static bool check_auth_credentials(const edb_config_t *cfg, const char *user, const char *pass) {
    if (!cfg->auth_enabled) return true;
    return strcmp(user, cfg->admin_user) == 0 && strcmp(pass, cfg->admin_password) == 0;
}

static bool decode_base64(const char *in, size_t in_len, char *out, size_t out_cap) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    uint32_t val = 0;
    int valb = -8;
    size_t pos = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') break;
        int d = T[c];
        if (d < 0) continue;
        val = (val << 6) + (uint32_t)d;
        valb += 6;
        if (valb >= 0) {
            if (pos + 1 < out_cap) out[pos++] = (char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    out[pos] = '\0';
    return true;
}

/* ---- HTTP Protocol Handler ---- */
static void send_http_response(edb_buf_t *wbuf, int code, const char *status_text,
                               const char *body, size_t body_len, bool keep_alive) {
    edb_buf_printf(wbuf,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "Server: ElliottDB/%s\r\n\r\n",
        code, status_text, body_len,
        keep_alive ? "keep-alive" : "close",
        ELLIOTTDB_VERSION);
    if (body_len > 0 && body) edb_buf_append(wbuf, body, body_len);
}

static bool extract_json_string_field(const char *json, size_t len, const char *field, edb_buf_t *out) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    size_t plen = strlen(pat);
    for (size_t i = 0; i + plen < len; i++) {
        if (memcmp(json + i, pat, plen) == 0) {
            size_t j = i + plen;
            while (j < len && isspace((unsigned char)json[j])) j++;
            if (j >= len || json[j] != ':') continue;
            j++;
            while (j < len && isspace((unsigned char)json[j])) j++;
            if (j >= len || json[j] != '"') return false;
            j++;
            while (j < len) {
                char c = json[j++];
                if (c == '"') return true;
                if (c == '\\' && j < len) {
                    char e = json[j++];
                    if (e == 'n') edb_buf_append_char(out, '\n');
                    else if (e == 'r') edb_buf_append_char(out, '\r');
                    else if (e == 't') edb_buf_append_char(out, '\t');
                    else edb_buf_append_char(out, e);
                } else {
                    edb_buf_append_char(out, c);
                }
            }
            return false;
        }
    }
    return false;
}

static int process_http(edb_engine_t *eng, edb_conn_t *conn, bool *out_close) {
    const char *buf = (const char *)conn->rbuf.data;
    size_t len = conn->rbuf.len;
    const char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            hdr_end = buf + i + 4;
            break;
        }
    }
    if (!hdr_end) return 0; /* wait for more bytes */

    size_t hdr_len = (size_t)(hdr_end - buf);
    size_t content_len = 0;
    bool keep_alive = true;
    bool auth_ok = !eng->db->cfg->auth_enabled || conn->authenticated;

    /* Parse request line */
    const char *line_end = (const char *)memchr(buf, '\n', hdr_len);
    size_t rl_len = line_end ? (size_t)(line_end - buf) : hdr_len;
    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);
    (void)rl_len;

    /* Parse headers */
    const char *cur = line_end ? line_end + 1 : hdr_end;
    while (cur < hdr_end) {
        const char *nl = (const char *)memchr(cur, '\n', (size_t)(hdr_end - cur));
        size_t l = nl ? (size_t)(nl - cur) : (size_t)(hdr_end - cur);
        if (l > 15 && edb_strncasecmp(cur, "Content-Length:", 15) == 0) {
            content_len = (size_t)strtoull(cur + 15, NULL, 10);
        } else if (l > 11 && edb_strncasecmp(cur, "Connection:", 11) == 0) {
            if (strstr(cur, "close") || strstr(cur, "Close")) keep_alive = false;
        } else if (l > 21 && edb_strncasecmp(cur, "Authorization: Basic ", 21) == 0) {
            char b64[512] = {0};
            size_t blen = l - 21;
            while (blen > 0 && (cur[21 + blen - 1] == '\r' || cur[21 + blen - 1] == '\n' || cur[21 + blen - 1] == ' ')) blen--;
            decode_base64(cur + 21, blen, b64, sizeof(b64));
            char *colon = strchr(b64, ':');
            if (colon) {
                *colon = '\0';
                auth_ok = check_auth_credentials(eng->db->cfg, b64, colon + 1);
            }
        }
        cur = nl ? nl + 1 : hdr_end;
    }

    size_t max_bytes = (size_t)eng->db->cfg->max_query_size_mb * 1024ULL * 1024ULL;
    if (content_len > max_bytes) {
        const char *msg = "{\"ok\":false,\"error\":\"Payload too large\"}";
        send_http_response(&conn->wbuf, 413, "Payload Too Large", msg, strlen(msg), false);
        *out_close = true;
        return (int)hdr_len;
    }

    if (len < hdr_len + content_len) return 0; /* wait for body */

    const char *body = hdr_end;
    size_t consumed = hdr_len + content_len;
    if (eng->db->metrics) edb_atomic_add_u64(&eng->db->metrics->total_requests, 1);

    if (!auth_ok && strcmp(path, "/health") != 0) {
        const char *msg = "{\"ok\":false,\"error\":\"Unauthorized\"}";
        send_http_response(&conn->wbuf, 401, "Unauthorized", msg, strlen(msg), keep_alive);
        if (!keep_alive) *out_close = true;
        return (int)consumed;
    }

    edb_buf_t json_out;
    edb_buf_init(&json_out);
    int http_code = 200;
    const char *http_status = "OK";

    if (strcmp(path, "/health") == 0) {
        uint64_t up = eng->db->metrics ? (edb_now_ns() - eng->db->metrics->start_time_ns) / 1000000000ULL : 0;
        edb_buf_printf(&json_out, "{\"status\":\"ok\",\"version\":\"%s\",\"node\":\"%s\",\"uptime_sec\":%llu}",
                       ELLIOTTDB_VERSION, eng->db->cfg->node_name, (unsigned long long)up);
    } else if (strcmp(path, "/metrics") == 0) {
        if (eng->db->metrics) edb_metrics_dump_json(eng->db->metrics, eng->db->cfg->node_name, &json_out);
        else edb_buf_append_str(&json_out, "{}");
    } else if (strcmp(path, "/config") == 0) {
        edb_config_dump_json(eng->db->cfg, &json_out);
    } else if (strcmp(path, "/query") == 0) {
        edb_buf_t sql_buf;
        edb_buf_init(&sql_buf);
        const char *sql_ptr = body;
        size_t sql_len = content_len;
        if (content_len > 0 && body[0] == '{') {
            if (extract_json_string_field(body, content_len, "sql", &sql_buf)) {
                sql_ptr = (const char *)sql_buf.data;
                sql_len = sql_buf.len;
            }
        }
        edb_result_t r;
        edb_execute_sql(eng, &conn->sess, sql_ptr, sql_len, &r);
        if (r.status != EDB_OK) { http_code = 400; http_status = "Bad Request"; }
        edb_result_to_json(&r, &json_out);
        edb_result_free(&r);
        edb_buf_free(&sql_buf);
    } else if (strcmp(path, "/batch") == 0) {
        edb_buf_append_char(&json_out, '[');
        size_t pos = 0;
        int qidx = 0;
        while (pos < content_len) {
            while (pos < content_len && (isspace((unsigned char)body[pos]) || body[pos] == ';')) pos++;
            if (pos >= content_len) break;
            size_t start = pos;
            bool in_str = false;
            while (pos < content_len) {
                if (body[pos] == '\'') in_str = !in_str;
                else if (!in_str && body[pos] == ';') break;
                pos++;
            }
            if (qidx++ > 0) edb_buf_append_char(&json_out, ',');
            edb_result_t r;
            edb_execute_sql(eng, &conn->sess, body + start, pos - start, &r);
            edb_result_to_json(&r, &json_out);
            edb_result_free(&r);
        }
        edb_buf_append_char(&json_out, ']');
    } else {
        http_code = 404;
        http_status = "Not Found";
        edb_buf_append_str(&json_out, "{\"ok\":false,\"error\":\"Not found\"}");
    }

    send_http_response(&conn->wbuf, http_code, http_status,
                       json_out.data ? (const char *)json_out.data : "", json_out.len, keep_alive);
    edb_buf_free(&json_out);
    if (!keep_alive) *out_close = true;
    return (int)consumed;
}

/* ---- Binary Protocol Handler ---- */
static int process_binary(edb_engine_t *eng, edb_conn_t *conn, bool *out_close) {
    size_t total_consumed = 0;
    int max_pipe = (int)eng->db->cfg->max_pipeline_depth;
    int frames = 0;

    while (conn->rbuf.len - total_consumed >= EDB_BIN_HDR_SIZE && frames < max_pipe) {
        const uint8_t *p = conn->rbuf.data + total_consumed;
        if (p[0] != EDB_BIN_REQ_MAGIC) {
            *out_close = true;
            return -1;
        }
        uint8_t op = p[1];
        uint32_t plen = read_u32_le(p + 4);
        size_t max_q = (size_t)eng->db->cfg->max_query_size_mb * 1024ULL * 1024ULL;
        if (plen > max_q) {
            *out_close = true;
            return -1;
        }
        if (conn->rbuf.len - total_consumed < EDB_BIN_HDR_SIZE + plen) break;

        const uint8_t *payload = p + EDB_BIN_HDR_SIZE;
        if (eng->db->metrics) edb_atomic_add_u64(&eng->db->metrics->total_requests, 1);

        if (op == EDB_OP_PING) {
            encode_bin_resp_hdr(&conn->wbuf, EDB_OK, 0, (const uint8_t *)"PONG", 4);
        } else if (op == EDB_OP_AUTH) {
            /* payload: [u16 ulen][user][u16 plen][pass] */
            bool ok = false;
            if (plen >= 4) {
                uint16_t ul = read_u16_le(payload);
                if ((uint32_t)(2 + ul + 2) <= plen) {
                    uint16_t pw_l = read_u16_le(payload + 2 + ul);
                    if ((uint32_t)(4 + ul + pw_l) <= plen) {
                        char user[128] = {0}, pass[256] = {0};
                        memcpy(user, payload + 2, ul < 127 ? ul : 127);
                        memcpy(pass, payload + 4 + ul, pw_l < 255 ? pw_l : 255);
                        ok = check_auth_credentials(eng->db->cfg, user, pass);
                    }
                }
            }
            if (ok) {
                conn->authenticated = true;
                encode_bin_resp_hdr(&conn->wbuf, EDB_OK, 0, NULL, 0);
            } else {
                conn->auth_failures++;
                edb_result_t r;
                edb_result_init(&r);
                r.status = EDB_ERR_AUTH;
                snprintf(r.err_msg, sizeof(r.err_msg), "Authentication failed");
                encode_result_binary(&r, &conn->wbuf);
                if (conn->auth_failures >= eng->db->cfg->max_auth_failures) *out_close = true;
            }
        } else if (eng->db->cfg->auth_enabled && !conn->authenticated) {
            edb_result_t r;
            edb_result_init(&r);
            r.status = EDB_ERR_AUTH;
            snprintf(r.err_msg, sizeof(r.err_msg), "Authentication required");
            encode_result_binary(&r, &conn->wbuf);
        } else if (op == EDB_OP_QUERY) {
            edb_result_t r;
            edb_execute_sql(eng, &conn->sess, (const char *)payload, plen, &r);
            encode_result_binary(&r, &conn->wbuf);
            edb_result_free(&r);
        } else if (op == EDB_OP_BATCH) {
            const char *sql = (const char *)payload;
            size_t pos = 0;
            edb_result_t last_r;
            edb_result_init(&last_r);
            while (pos < plen) {
                while (pos < plen && (isspace((unsigned char)sql[pos]) || sql[pos] == ';')) pos++;
                if (pos >= plen) break;
                size_t st = pos;
                bool in_s = false;
                while (pos < plen) {
                    if (sql[pos] == '\'') in_s = !in_s;
                    else if (!in_s && sql[pos] == ';') break;
                    pos++;
                }
                edb_result_free(&last_r);
                edb_execute_sql(eng, &conn->sess, sql + st, pos - st, &last_r);
                if (last_r.status != EDB_OK) break;
            }
            encode_result_binary(&last_r, &conn->wbuf);
            edb_result_free(&last_r);
        } else {
            edb_result_t r;
            edb_result_init(&r);
            r.status = EDB_ERR_INVALID;
            snprintf(r.err_msg, sizeof(r.err_msg), "Unknown binary opcode 0x%02x", op);
            encode_result_binary(&r, &conn->wbuf);
        }

        total_consumed += EDB_BIN_HDR_SIZE + plen;
        frames++;
        if (*out_close) break;
    }

    return (int)total_consumed;
}

/* ---- Text Protocol Handler ---- */
static int process_text(edb_engine_t *eng, edb_conn_t *conn, bool *out_close) {
    size_t total_consumed = 0;
    const char *buf = (const char *)conn->rbuf.data;
    size_t len = conn->rbuf.len;

    while (total_consumed < len) {
        const char *nl = (const char *)memchr(buf + total_consumed, '\n', len - total_consumed);
        if (!nl) break;
        size_t line_len = (size_t)(nl - (buf + total_consumed));
        const char *line = buf + total_consumed;
        total_consumed += line_len + 1;

        while (line_len > 0 && (line[line_len - 1] == '\r' || isspace((unsigned char)line[line_len - 1]))) line_len--;
        while (line_len > 0 && isspace((unsigned char)*line)) { line++; line_len--; }
        if (line_len == 0) continue;

        if (eng->db->metrics) edb_atomic_add_u64(&eng->db->metrics->total_requests, 1);

        if (line_len == 4 && edb_strncasecmp(line, "QUIT", 4) == 0) {
            edb_buf_append_str(&conn->wbuf, "BYE\n");
            *out_close = true;
            break;
        }
        if (line_len >= 5 && edb_strncasecmp(line, "AUTH ", 5) == 0) {
            char tmp[512] = {0};
            size_t cpy = line_len - 5;
            if (cpy >= sizeof(tmp)) cpy = sizeof(tmp) - 1;
            memcpy(tmp, line + 5, cpy);
            char user[128] = {0}, pass[256] = {0};
            sscanf(tmp, "%127s %255s", user, pass);
            if (check_auth_credentials(eng->db->cfg, user, pass)) {
                conn->authenticated = true;
                edb_buf_append_str(&conn->wbuf, "OK\n");
            } else {
                edb_buf_append_str(&conn->wbuf, "ERR: auth failed\n");
            }
            continue;
        }
        if (eng->db->cfg->auth_enabled && !conn->authenticated) {
            edb_buf_append_str(&conn->wbuf, "ERR: authentication required\n");
            continue;
        }

        edb_result_t r;
        edb_execute_sql(eng, &conn->sess, line, line_len, &r);
        edb_result_to_text(&r, &conn->wbuf);
        edb_result_free(&r);
    }

    return (int)total_consumed;
}

int edb_protocol_process(edb_engine_t *eng, edb_conn_t *conn, bool *out_close) {
    *out_close = false;
    if (conn->rbuf.len == 0) return 0;

    if (conn->proto == PROTO_UNKNOWN) {
        const char *p = (const char *)conn->rbuf.data;
        if (conn->rbuf.data[0] == EDB_BIN_REQ_MAGIC && eng->db->cfg->enable_binary_protocol) {
            conn->proto = PROTO_BINARY;
        } else if (eng->db->cfg->enable_http && conn->rbuf.len >= 4 &&
                   (strncmp(p, "GET ", 4) == 0 || strncmp(p, "POST", 4) == 0 ||
                    strncmp(p, "HEAD", 4) == 0 || strncmp(p, "PUT ", 4) == 0 ||
                    strncmp(p, "DELE", 4) == 0 || strncmp(p, "OPTI", 4) == 0)) {
            conn->proto = PROTO_HTTP;
        } else if (eng->db->cfg->enable_text_protocol) {
            conn->proto = PROTO_TEXT;
        } else {
            *out_close = true;
            return -1;
        }
    }

    int consumed = 0;
    if (conn->proto == PROTO_BINARY) consumed = process_binary(eng, conn, out_close);
    else if (conn->proto == PROTO_HTTP) consumed = process_http(eng, conn, out_close);
    else consumed = process_text(eng, conn, out_close);

    if (consumed > 0) {
        edb_buf_consume(&conn->rbuf, (size_t)consumed);
    }
    return consumed;
}
