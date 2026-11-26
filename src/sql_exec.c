#include "sql_exec.h"
#include <ctype.h>
#include <math.h>

/* ---- Result Helpers ---- */
void edb_result_init(edb_result_t *r) {
    memset(r, 0, sizeof(*r));
    r->status = EDB_OK;
}

void edb_result_free(edb_result_t *r) {
    if (!r) return;
    if (r->col_names) {
        for (uint32_t i = 0; i < r->col_count; i++) free(r->col_names[i]);
        free(r->col_names);
    }
    free(r->col_types);
    if (r->rows) {
        for (uint32_t i = 0; i < r->row_count; i++) edb_row_free(&r->rows[i]);
        free(r->rows);
    }
    memset(r, 0, sizeof(*r));
}

static edb_result_t result_clone(const edb_result_t *src) {
    edb_result_t dst;
    edb_result_init(&dst);
    dst.status = src->status;
    memcpy(dst.err_msg, src->err_msg, sizeof(dst.err_msg));
    dst.col_count = src->col_count;
    dst.rows_affected = src->rows_affected;
    dst.last_insert_id = src->last_insert_id;
    dst.elapsed_us = src->elapsed_us;
    dst.is_select_like = src->is_select_like;
    if (src->col_count > 0) {
        dst.col_names = (char **)calloc(src->col_count, sizeof(char *));
        dst.col_types = (edb_type_t *)calloc(src->col_count, sizeof(edb_type_t));
        for (uint32_t i = 0; i < src->col_count; i++) {
            dst.col_names[i] = edb_strdup(src->col_names[i]);
            dst.col_types[i] = src->col_types[i];
        }
    }
    if (src->row_count > 0) {
        dst.row_count = src->row_count;
        dst.row_cap = src->row_count;
        dst.rows = (edb_row_t *)calloc(src->row_count, sizeof(edb_row_t));
        for (uint32_t i = 0; i < src->row_count; i++) {
            dst.rows[i] = edb_row_clone(&src->rows[i]);
        }
    }
    return dst;
}

static void result_set_err(edb_result_t *r, edb_status_t st, const char *fmt, ...) {
    r->status = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err_msg, sizeof(r->err_msg), fmt, ap);
    va_end(ap);
}

static void result_setup_cols(edb_result_t *r, uint32_t count) {
    r->col_count = count;
    r->col_names = (char **)calloc(count, sizeof(char *));
    r->col_types = (edb_type_t *)calloc(count, sizeof(edb_type_t));
    r->is_select_like = true;
}

static void result_add_row(edb_result_t *r, edb_row_t row) {
    if (r->row_count == r->row_cap) {
        uint32_t nc = r->row_cap == 0 ? 16 : r->row_cap * 2;
        edb_row_t *nr = (edb_row_t *)realloc(r->rows, nc * sizeof(edb_row_t));
        if (!nr) { edb_row_free(&row); return; }
        r->rows = nr;
        r->row_cap = nc;
    }
    r->rows[r->row_count++] = row;
}

/* ---- Engine & Session Lifecycle ---- */
void edb_engine_init(edb_engine_t *eng, edb_db_t *db) {
    memset(eng, 0, sizeof(*eng));
    eng->db = db;
    size_t per_plan = (size_t)db->cfg->plan_cache_entries / EDB_CACHE_SHARDS;
    if (per_plan < 4) per_plan = 4;
    size_t per_res_bytes = ((size_t)db->cfg->result_cache_mb * 1024ULL * 1024ULL) / EDB_CACHE_SHARDS;
    if (per_res_bytes < 16384) per_res_bytes = 16384;
    for (int i = 0; i < EDB_CACHE_SHARDS; i++) {
        edb_mutex_init(&eng->plan_shards[i].mu);
        eng->plan_shards[i].max_entries = per_plan;
        edb_mutex_init(&eng->res_shards[i].mu);
        eng->res_shards[i].max_bytes = per_res_bytes;
    }
}

void edb_engine_destroy(edb_engine_t *eng) {
    for (int i = 0; i < EDB_CACHE_SHARDS; i++) {
        edb_plan_entry_t *pe = eng->plan_shards[i].head;
        while (pe) {
            edb_plan_entry_t *nxt = pe->next;
            free(pe->sql);
            edb_arena_free(&pe->arena);
            free(pe);
            pe = nxt;
        }
        edb_mutex_destroy(&eng->plan_shards[i].mu);

        edb_res_entry_t *re = eng->res_shards[i].head;
        while (re) {
            edb_res_entry_t *nxt = re->next;
            free(re->sql);
            edb_result_free(&re->result);
            free(re);
            re = nxt;
        }
        edb_mutex_destroy(&eng->res_shards[i].mu);
    }
}

void edb_session_init(edb_session_t *s) {
    memset(s, 0, sizeof(*s));
}

void edb_session_destroy(edb_engine_t *eng, edb_session_t *s) {
    if (!s) return;
    if (s->txn && eng && eng->db) {
        edb_txn_rollback(eng->db, s->txn);
        s->txn = NULL;
    }
    for (size_t i = 0; i < s->prep_count; i++) {
        free(s->preps[i].sql);
    }
    free(s->preps);
    memset(s, 0, sizeof(*s));
}

/* ---- Expression Evaluation ---- */
typedef struct {
    const char *table_name;
    const char *alias;
    const edb_table_t *table;
    const edb_row_t *row;
} eval_source_t;

typedef struct {
    eval_source_t *sources;
    size_t source_count;
    const edb_val_t *params;
    size_t param_count;
    /* For HAVING / aggregate evaluation over a group */
    const edb_row_t *const **group_tuples;
    size_t group_row_count;
    bool in_aggregate;
} eval_ctx_t;

static double val_as_double(const edb_val_t *v) {
    if (v->type == EDB_TYPE_INT || v->type == EDB_TYPE_TIMESTAMP) return (double)v->v.i64;
    if (v->type == EDB_TYPE_REAL) return v->v.f64;
    if (v->type == EDB_TYPE_BOOL) return v->v.b ? 1.0 : 0.0;
    if (v->type == EDB_TYPE_TEXT && v->v.str.ptr) return strtod(v->v.str.ptr, NULL);
    return 0.0;
}

static bool val_is_truthy(const edb_val_t *v) {
    switch (v->type) {
    case EDB_TYPE_NULL: return false;
    case EDB_TYPE_BOOL: return v->v.b;
    case EDB_TYPE_INT:
    case EDB_TYPE_TIMESTAMP: return v->v.i64 != 0;
    case EDB_TYPE_REAL: return v->v.f64 != 0.0;
    case EDB_TYPE_TEXT: return v->v.str.len > 0;
    default: return true;
    }
}

static void val_to_str(const edb_val_t *v, edb_buf_t *b) {
    switch (v->type) {
    case EDB_TYPE_NULL: edb_buf_append_str(b, "NULL"); break;
    case EDB_TYPE_INT:
    case EDB_TYPE_TIMESTAMP: edb_buf_printf(b, "%lld", (long long)v->v.i64); break;
    case EDB_TYPE_REAL: edb_buf_printf(b, "%g", v->v.f64); break;
    case EDB_TYPE_BOOL: edb_buf_append_str(b, v->v.b ? "true" : "false"); break;
    case EDB_TYPE_TEXT:
        if (v->v.str.ptr && v->v.str.len > 0) edb_buf_append(b, v->v.str.ptr, v->v.str.len);
        break;
    case EDB_TYPE_BLOB:
        edb_buf_printf(b, "<blob %u bytes>", v->v.blob.len);
        break;
    }
}

static bool resolve_ident_col(const eval_ctx_t *ctx, const char *tbl, const char *col,
                              size_t *out_src, uint32_t *out_col) {
    for (size_t s = 0; s < ctx->source_count; s++) {
        const eval_source_t *src = &ctx->sources[s];
        if (tbl && *tbl) {
            bool match = (src->alias && edb_strcasecmp(src->alias, tbl) == 0) ||
                         (src->table_name && edb_strcasecmp(src->table_name, tbl) == 0);
            if (!match) continue;
        }
        int32_t ci = edb_table_col_index(src->table, col);
        if (ci >= 0) {
            *out_src = s;
            *out_col = (uint32_t)ci;
            return true;
        }
    }
    return false;
}

static edb_val_t eval_expr(const edb_expr_t *e, eval_ctx_t *ctx);

static bool is_agg_func(const char *name) {
    return edb_strcasecmp(name, "COUNT") == 0 ||
           edb_strcasecmp(name, "SUM") == 0 ||
           edb_strcasecmp(name, "AVG") == 0 ||
           edb_strcasecmp(name, "MIN") == 0 ||
           edb_strcasecmp(name, "MAX") == 0;
}

static bool expr_has_agg(const edb_expr_t *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_CALL:
        if (is_agg_func(e->u.call.func_name)) return true;
        for (size_t i = 0; i < e->u.call.arg_count; i++) {
            if (expr_has_agg(e->u.call.args[i])) return true;
        }
        return false;
    case EXPR_UNARY: return expr_has_agg(e->u.unary.operand);
    case EXPR_BINARY: return expr_has_agg(e->u.binary.left) || expr_has_agg(e->u.binary.right);
    case EXPR_IS_NULL: return expr_has_agg(e->u.is_null.expr);
    case EXPR_BETWEEN: return expr_has_agg(e->u.between.expr);
    case EXPR_CASE:
        for (size_t i = 0; i < e->u.case_expr.when_count; i++) {
            if (expr_has_agg(e->u.case_expr.whens[i].cond) ||
                expr_has_agg(e->u.case_expr.whens[i].then_expr)) return true;
        }
        return expr_has_agg(e->u.case_expr.else_expr);
    default:
        return false;
    }
}

static bool expr_has_now(const edb_expr_t *e) {
    if (!e) return false;
    if (e->kind == EXPR_CALL && edb_strcasecmp(e->u.call.func_name, "NOW") == 0) return true;
    if (e->kind == EXPR_UNARY) return expr_has_now(e->u.unary.operand);
    if (e->kind == EXPR_BINARY) return expr_has_now(e->u.binary.left) || expr_has_now(e->u.binary.right);
    return false;
}

static edb_val_t eval_aggregate(const edb_expr_t *call, eval_ctx_t *ctx) {
    const char *fn = call->u.call.func_name;
    if (edb_strcasecmp(fn, "COUNT") == 0 && call->u.call.star) {
        return edb_val_int((int64_t)ctx->group_row_count);
    }

    int64_t count = 0;
    double sum_f = 0.0;
    int64_t sum_i = 0;
    bool all_int = true;
    edb_val_t best = edb_val_null();
    bool has_best = false;

    /* For COUNT(DISTINCT ...) */
    edb_val_t *seen = NULL;
    size_t seen_cnt = 0;
    if (call->u.call.distinct && ctx->group_row_count > 0) {
        seen = (edb_val_t *)calloc(ctx->group_row_count, sizeof(edb_val_t));
    }

    const edb_row_t **saved_rows = ctx->source_count > 0
        ? (const edb_row_t **)calloc(ctx->source_count, sizeof(edb_row_t *)) : NULL;
    for (size_t s = 0; s < ctx->source_count; s++) saved_rows[s] = ctx->sources[s].row;
    for (size_t i = 0; i < ctx->group_row_count; i++) {
        for (size_t s = 0; s < ctx->source_count; s++) {
            ctx->sources[s].row = ctx->group_tuples[i][s];
        }
        edb_val_t v = call->u.call.arg_count > 0 ? eval_expr(call->u.call.args[0], ctx) : edb_val_null();
        if (v.type == EDB_TYPE_NULL) {
            edb_val_free(&v);
            continue;
        }
        if (seen) {
            bool dup = false;
            for (size_t k = 0; k < seen_cnt; k++) {
                if (edb_val_cmp(&seen[k], &v) == 0) { dup = true; break; }
            }
            if (dup) { edb_val_free(&v); continue; }
            seen[seen_cnt++] = edb_val_clone(&v);
        }

        count++;
        if (edb_strcasecmp(fn, "SUM") == 0 || edb_strcasecmp(fn, "AVG") == 0) {
            if (v.type == EDB_TYPE_INT) sum_i += v.v.i64;
            else all_int = false;
            sum_f += val_as_double(&v);
        } else if (edb_strcasecmp(fn, "MIN") == 0) {
            if (!has_best || edb_val_cmp(&v, &best) < 0) {
                edb_val_free(&best);
                best = edb_val_clone(&v);
                has_best = true;
            }
        } else if (edb_strcasecmp(fn, "MAX") == 0) {
            if (!has_best || edb_val_cmp(&v, &best) > 0) {
                edb_val_free(&best);
                best = edb_val_clone(&v);
                has_best = true;
            }
        }
        edb_val_free(&v);
    }
    for (size_t s = 0; s < ctx->source_count; s++) ctx->sources[s].row = saved_rows[s];
    free(saved_rows);
    if (seen) {
        for (size_t k = 0; k < seen_cnt; k++) edb_val_free(&seen[k]);
        free(seen);
    }

    if (edb_strcasecmp(fn, "COUNT") == 0) { edb_val_free(&best); return edb_val_int(count); }
    if (edb_strcasecmp(fn, "SUM") == 0) {
        edb_val_free(&best);
        if (count == 0) return edb_val_null();
        return all_int ? edb_val_int(sum_i) : edb_val_real(sum_f);
    }
    if (edb_strcasecmp(fn, "AVG") == 0) {
        edb_val_free(&best);
        if (count == 0) return edb_val_null();
        return edb_val_real(sum_f / (double)count);
    }
    return has_best ? best : edb_val_null();
}

static edb_val_t eval_expr(const edb_expr_t *e, eval_ctx_t *ctx) {
    if (!e) return edb_val_null();
    switch (e->kind) {
    case EXPR_LITERAL:
        return edb_val_clone(&e->u.lit);
    case EXPR_PARAM: {
        int idx = e->u.param.index;
        if (idx >= 1 && (size_t)(idx - 1) < ctx->param_count) {
            return edb_val_clone(&ctx->params[idx - 1]);
        }
        return edb_val_null();
    }
    case EXPR_IDENT: {
        size_t s = 0;
        uint32_t c = 0;
        if (resolve_ident_col(ctx, e->u.ident.table, e->u.ident.col, &s, &c)) {
            const edb_row_t *row = ctx->sources[s].row;
            if (row && c < row->col_count) {
                return edb_val_clone(&row->vals[c]);
            }
        }
        return edb_val_null();
    }
    case EXPR_UNARY: {
        edb_val_t v = eval_expr(e->u.unary.operand, ctx);
        if (e->u.unary.op == OP_NOT) {
            if (v.type == EDB_TYPE_NULL) return v;
            bool res = !val_is_truthy(&v);
            edb_val_free(&v);
            return edb_val_bool(res);
        }
        if (e->u.unary.op == OP_NEG) {
            if (v.type == EDB_TYPE_INT) return edb_val_int(-v.v.i64);
            if (v.type == EDB_TYPE_REAL) return edb_val_real(-v.v.f64);
            edb_val_free(&v);
            return edb_val_null();
        }
        return v;
    }
    case EXPR_BINARY: {
        if (e->u.binary.op == OP_AND) {
            edb_val_t l = eval_expr(e->u.binary.left, ctx);
            if (l.type != EDB_TYPE_NULL && !val_is_truthy(&l)) { edb_val_free(&l); return edb_val_bool(false); }
            edb_val_t r = eval_expr(e->u.binary.right, ctx);
            bool lt = val_is_truthy(&l), rt = val_is_truthy(&r);
            bool ln = (l.type == EDB_TYPE_NULL), rn = (r.type == EDB_TYPE_NULL);
            edb_val_free(&l); edb_val_free(&r);
            if (!rn && !rt) return edb_val_bool(false);
            if (ln || rn) return edb_val_null();
            return edb_val_bool(lt && rt);
        }
        if (e->u.binary.op == OP_OR) {
            edb_val_t l = eval_expr(e->u.binary.left, ctx);
            if (val_is_truthy(&l)) { edb_val_free(&l); return edb_val_bool(true); }
            edb_val_t r = eval_expr(e->u.binary.right, ctx);
            bool rt = val_is_truthy(&r);
            bool ln = (l.type == EDB_TYPE_NULL), rn = (r.type == EDB_TYPE_NULL);
            edb_val_free(&l); edb_val_free(&r);
            if (rt) return edb_val_bool(true);
            if (ln || rn) return edb_val_null();
            return edb_val_bool(false);
        }

        edb_val_t l = eval_expr(e->u.binary.left, ctx);
        edb_val_t r = eval_expr(e->u.binary.right, ctx);
        if (l.type == EDB_TYPE_NULL || r.type == EDB_TYPE_NULL) {
            edb_val_free(&l); edb_val_free(&r);
            return edb_val_null();
        }

        edb_op_t op = e->u.binary.op;
        if (op == OP_CONCAT) {
            edb_buf_t b;
            edb_buf_init(&b);
            val_to_str(&l, &b);
            val_to_str(&r, &b);
            edb_val_t out = edb_val_text_copy(b.data ? (const char *)b.data : "", (uint32_t)b.len);
            edb_buf_free(&b);
            edb_val_free(&l); edb_val_free(&r);
            return out;
        }
        if (op >= OP_EQ && op <= OP_GE) {
            int c = edb_val_cmp(&l, &r);
            edb_val_free(&l); edb_val_free(&r);
            bool res = false;
            if (op == OP_EQ) res = (c == 0);
            else if (op == OP_NE) res = (c != 0);
            else if (op == OP_LT) res = (c < 0);
            else if (op == OP_LE) res = (c <= 0);
            else if (op == OP_GT) res = (c > 0);
            else if (op == OP_GE) res = (c >= 0);
            return edb_val_bool(res);
        }
        bool both_int = (l.type == EDB_TYPE_INT && r.type == EDB_TYPE_INT);
        if (both_int) {
            int64_t a = l.v.i64, b = r.v.i64;
            if (op == OP_ADD) return edb_val_int(a + b);
            if (op == OP_SUB) return edb_val_int(a - b);
            if (op == OP_MUL) return edb_val_int(a * b);
            if (op == OP_DIV) return b != 0 ? edb_val_int(a / b) : edb_val_null();
            if (op == OP_MOD) return b != 0 ? edb_val_int(a % b) : edb_val_null();
        } else {
            double a = val_as_double(&l), b = val_as_double(&r);
            edb_val_free(&l); edb_val_free(&r);
            if (op == OP_ADD) return edb_val_real(a + b);
            if (op == OP_SUB) return edb_val_real(a - b);
            if (op == OP_MUL) return edb_val_real(a * b);
            if (op == OP_DIV) return b != 0.0 ? edb_val_real(a / b) : edb_val_null();
            if (op == OP_MOD) return b != 0.0 ? edb_val_real(fmod(a, b)) : edb_val_null();
        }
        edb_val_free(&l); edb_val_free(&r);
        return edb_val_null();
    }
    case EXPR_IS_NULL: {
        edb_val_t v = eval_expr(e->u.is_null.expr, ctx);
        bool is_n = (v.type == EDB_TYPE_NULL);
        edb_val_free(&v);
        return edb_val_bool(e->u.is_null.is_not ? !is_n : is_n);
    }
    case EXPR_IN: {
        edb_val_t v = eval_expr(e->u.in_list.expr, ctx);
        if (v.type == EDB_TYPE_NULL) return edb_val_null();
        bool found = false;
        for (size_t i = 0; i < e->u.in_list.count; i++) {
            edb_val_t it = eval_expr(e->u.in_list.items[i], ctx);
            if (edb_val_cmp(&v, &it) == 0) found = true;
            edb_val_free(&it);
            if (found) break;
        }
        edb_val_free(&v);
        return edb_val_bool(e->u.in_list.is_not ? !found : found);
    }
    case EXPR_BETWEEN: {
        edb_val_t v = eval_expr(e->u.between.expr, ctx);
        edb_val_t lo = eval_expr(e->u.between.lo, ctx);
        edb_val_t hi = eval_expr(e->u.between.hi, ctx);
        if (v.type == EDB_TYPE_NULL || lo.type == EDB_TYPE_NULL || hi.type == EDB_TYPE_NULL) {
            edb_val_free(&v); edb_val_free(&lo); edb_val_free(&hi);
            return edb_val_null();
        }
        bool in_rng = (edb_val_cmp(&v, &lo) >= 0 && edb_val_cmp(&v, &hi) <= 0);
        edb_val_free(&v); edb_val_free(&lo); edb_val_free(&hi);
        return edb_val_bool(e->u.between.is_not ? !in_rng : in_rng);
    }
    case EXPR_LIKE: {
        edb_val_t v = eval_expr(e->u.like.expr, ctx);
        edb_val_t p = eval_expr(e->u.like.pattern, ctx);
        if (v.type != EDB_TYPE_TEXT || p.type != EDB_TYPE_TEXT) {
            edb_val_free(&v); edb_val_free(&p);
            return edb_val_bool(false);
        }
        bool m = edb_like_match(v.v.str.ptr ? v.v.str.ptr : "", p.v.str.ptr ? p.v.str.ptr : "", false);
        edb_val_free(&v); edb_val_free(&p);
        return edb_val_bool(e->u.like.is_not ? !m : m);
    }
    case EXPR_CASE: {
        for (size_t i = 0; i < e->u.case_expr.when_count; i++) {
            edb_val_t c = eval_expr(e->u.case_expr.whens[i].cond, ctx);
            bool ok = val_is_truthy(&c);
            edb_val_free(&c);
            if (ok) return eval_expr(e->u.case_expr.whens[i].then_expr, ctx);
        }
        if (e->u.case_expr.else_expr) return eval_expr(e->u.case_expr.else_expr, ctx);
        return edb_val_null();
    }
    case EXPR_CALL: {
        const char *fn = e->u.call.func_name;
        if (is_agg_func(fn) && ctx->in_aggregate) {
            return eval_aggregate(e, ctx);
        }
        if (edb_strcasecmp(fn, "NOW") == 0) {
            return edb_val_int((int64_t)edb_wall_sec());
        }
        if (edb_strcasecmp(fn, "COALESCE") == 0) {
            for (size_t i = 0; i < e->u.call.arg_count; i++) {
                edb_val_t a = eval_expr(e->u.call.args[i], ctx);
                if (a.type != EDB_TYPE_NULL) return a;
            }
            return edb_val_null();
        }
        if (edb_strcasecmp(fn, "LOWER") == 0 && e->u.call.arg_count >= 1) {
            edb_val_t a = eval_expr(e->u.call.args[0], ctx);
            if (a.type == EDB_TYPE_TEXT && a.v.str.ptr) {
                for (uint32_t i = 0; i < a.v.str.len; i++) {
                    a.v.str.ptr[i] = (char)tolower((unsigned char)a.v.str.ptr[i]);
                }
            }
            return a;
        }
        if (edb_strcasecmp(fn, "UPPER") == 0 && e->u.call.arg_count >= 1) {
            edb_val_t a = eval_expr(e->u.call.args[0], ctx);
            if (a.type == EDB_TYPE_TEXT && a.v.str.ptr) {
                for (uint32_t i = 0; i < a.v.str.len; i++) {
                    a.v.str.ptr[i] = (char)toupper((unsigned char)a.v.str.ptr[i]);
                }
            }
            return a;
        }
        if (edb_strcasecmp(fn, "LENGTH") == 0 && e->u.call.arg_count >= 1) {
            edb_val_t a = eval_expr(e->u.call.args[0], ctx);
            int64_t l = (a.type == EDB_TYPE_TEXT) ? (int64_t)a.v.str.len : 0;
            edb_val_free(&a);
            return edb_val_int(l);
        }
        if (edb_strcasecmp(fn, "ABS") == 0 && e->u.call.arg_count >= 1) {
            edb_val_t a = eval_expr(e->u.call.args[0], ctx);
            if (a.type == EDB_TYPE_INT) return edb_val_int(a.v.i64 < 0 ? -a.v.i64 : a.v.i64);
            if (a.type == EDB_TYPE_REAL) return edb_val_real(fabs(a.v.f64));
            return a;
        }
        if (edb_strcasecmp(fn, "ROUND") == 0 && e->u.call.arg_count >= 1) {
            edb_val_t a = eval_expr(e->u.call.args[0], ctx);
            double d = val_as_double(&a);
            edb_val_free(&a);
            return edb_val_real(round(d));
        }
        if (edb_strcasecmp(fn, "SUBSTR") == 0 && e->u.call.arg_count >= 2) {
            edb_val_t s = eval_expr(e->u.call.args[0], ctx);
            edb_val_t st = eval_expr(e->u.call.args[1], ctx);
            int64_t start = st.type == EDB_TYPE_INT ? st.v.i64 : 1;
            if (start < 1) start = 1;
            int64_t len = -1;
            if (e->u.call.arg_count >= 3) {
                edb_val_t ln = eval_expr(e->u.call.args[2], ctx);
                len = ln.type == EDB_TYPE_INT ? ln.v.i64 : -1;
                edb_val_free(&ln);
            }
            edb_val_free(&st);
            if (s.type != EDB_TYPE_TEXT || !s.v.str.ptr) { edb_val_free(&s); return edb_val_null(); }
            uint32_t off = (uint32_t)(start - 1);
            if (off >= s.v.str.len) { edb_val_free(&s); return edb_val_text_copy("", 0); }
            uint32_t avail = s.v.str.len - off;
            uint32_t take = (len >= 0 && (uint32_t)len < avail) ? (uint32_t)len : avail;
            edb_val_t out = edb_val_text_copy(s.v.str.ptr + off, take);
            edb_val_free(&s);
            return out;
        }
        return edb_val_null();
    }
    default:
        return edb_val_null();
    }
}

/* ---- Index Scan Extractor ---- */
typedef enum {
    SCAN_FULL = 0,
    SCAN_INDEX_EQ = 1,
    SCAN_INDEX_RANGE = 2
} scan_plan_kind_t;

typedef struct {
    scan_plan_kind_t kind;
    edb_index_t *index;
    edb_val_t lo;
    bool lo_set;
    bool lo_inc;
    edb_val_t hi;
    bool hi_set;
    bool hi_inc;
} scan_plan_t;

static void free_scan_plan(scan_plan_t *sp) {
    if (sp->lo_set) edb_val_free(&sp->lo);
    if (sp->hi_set) edb_val_free(&sp->hi);
}

static bool extract_index_plan(const edb_table_t *t, const edb_expr_t *where,
                               const edb_val_t *params, size_t param_count,
                               scan_plan_t *plan) {
    memset(plan, 0, sizeof(*plan));
    if (!where || !t || t->index_count == 0) return false;

    if (where->kind == EXPR_BINARY && where->u.binary.op == OP_AND) {
        if (extract_index_plan(t, where->u.binary.left, params, param_count, plan)) return true;
        return extract_index_plan(t, where->u.binary.right, params, param_count, plan);
    }

    eval_ctx_t tmp_ctx;
    memset(&tmp_ctx, 0, sizeof(tmp_ctx));
    tmp_ctx.params = params;
    tmp_ctx.param_count = param_count;

    if (where->kind == EXPR_BINARY) {
        const edb_expr_t *l = where->u.binary.left;
        const edb_expr_t *r = where->u.binary.right;
        edb_op_t op = where->u.binary.op;

        const edb_expr_t *id_e = NULL;
        const edb_expr_t *val_e = NULL;
        bool flipped = false;
        if (l->kind == EXPR_IDENT && (r->kind == EXPR_LITERAL || r->kind == EXPR_PARAM)) {
            id_e = l; val_e = r;
        } else if (r->kind == EXPR_IDENT && (l->kind == EXPR_LITERAL || l->kind == EXPR_PARAM)) {
            id_e = r; val_e = l; flipped = true;
        }
        if (!id_e) return false;
        int32_t cidx = edb_table_col_index(t, id_e->u.ident.col);
        if (cidx < 0) return false;
        edb_index_t *idx = edb_table_find_index_for_col(t, (uint32_t)cidx);
        if (!idx) return false;

        edb_val_t v = eval_expr(val_e, &tmp_ctx);
        if (v.type == EDB_TYPE_NULL) { edb_val_free(&v); return false; }

        if (op == OP_EQ) {
            plan->kind = SCAN_INDEX_EQ;
            plan->index = idx;
            plan->lo = v;
            plan->lo_set = true;
            plan->lo_inc = true;
            return true;
        }
        if (idx->type == EDB_IDX_BTREE) {
            if (flipped) {
                if (op == OP_LT) op = OP_GT;
                else if (op == OP_LE) op = OP_GE;
                else if (op == OP_GT) op = OP_LT;
                else if (op == OP_GE) op = OP_LE;
            }
            if (op == OP_GT || op == OP_GE) {
                plan->kind = SCAN_INDEX_RANGE;
                plan->index = idx;
                plan->lo = v;
                plan->lo_set = true;
                plan->lo_inc = (op == OP_GE);
                return true;
            }
            if (op == OP_LT || op == OP_LE) {
                plan->kind = SCAN_INDEX_RANGE;
                plan->index = idx;
                plan->hi = v;
                plan->hi_set = true;
                plan->hi_inc = (op == OP_LE);
                return true;
            }
        }
        edb_val_free(&v);
    } else if (where->kind == EXPR_BETWEEN && !where->u.between.is_not &&
               where->u.between.expr->kind == EXPR_IDENT) {
        int32_t cidx = edb_table_col_index(t, where->u.between.expr->u.ident.col);
        if (cidx < 0) return false;
        edb_index_t *idx = edb_table_find_index_for_col(t, (uint32_t)cidx);
        if (!idx || idx->type != EDB_IDX_BTREE) return false;
        if ((where->u.between.lo->kind == EXPR_LITERAL || where->u.between.lo->kind == EXPR_PARAM) &&
            (where->u.between.hi->kind == EXPR_LITERAL || where->u.between.hi->kind == EXPR_PARAM)) {
            plan->kind = SCAN_INDEX_RANGE;
            plan->index = idx;
            plan->lo = eval_expr(where->u.between.lo, &tmp_ctx);
            plan->lo_set = true;
            plan->lo_inc = true;
            plan->hi = eval_expr(where->u.between.hi, &tmp_ctx);
            plan->hi_set = true;
            plan->hi_inc = true;
            return true;
        }
    }
    return false;
}

typedef struct {
    edb_rid_t *rids;
    edb_row_t *rows;
    size_t count;
    size_t cap;
    const edb_expr_t *where;
    eval_ctx_t *ctx;
    int64_t early_limit; /* -1 = unlimited */
} scan_collect_t;

static bool collect_matching_cb(edb_rid_t rid, const edb_row_t *row, void *vctx) {
    scan_collect_t *sc = (scan_collect_t *)vctx;
    sc->ctx->sources[0].row = row;
    if (sc->where) {
        edb_val_t cond = eval_expr(sc->where, sc->ctx);
        bool ok = val_is_truthy(&cond);
        edb_val_free(&cond);
        if (!ok) return true;
    }
    if (sc->count == sc->cap) {
        size_t nc = sc->cap == 0 ? 32 : sc->cap * 2;
        sc->rids = (edb_rid_t *)realloc(sc->rids, nc * sizeof(edb_rid_t));
        sc->rows = (edb_row_t *)realloc(sc->rows, nc * sizeof(edb_row_t));
        sc->cap = nc;
    }
    sc->rids[sc->count] = rid;
    sc->rows[sc->count] = edb_row_clone(row);
    sc->count++;
    if (sc->early_limit >= 0 && (int64_t)sc->count >= sc->early_limit) return false;
    return true;
}

static void collect_table_rows(edb_db_t *db, const edb_table_t *t, const edb_expr_t *where,
                               const edb_val_t *params, size_t param_count,
                               int64_t early_limit, scan_collect_t *out) {
    memset(out, 0, sizeof(*out));
    out->early_limit = early_limit;

    eval_source_t src;
    src.table_name = t->name;
    src.alias = NULL;
    src.table = t;
    src.row = NULL;
    eval_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sources = &src;
    ctx.source_count = 1;
    ctx.params = params;
    ctx.param_count = param_count;
    out->where = where;
    out->ctx = &ctx;

    scan_plan_t sp;
    memset(&sp, 0, sizeof(sp));
    bool use_idx = db->cfg->index_scan_enabled &&
                   extract_index_plan(t, where, params, param_count, &sp);

    if (use_idx) {
        edb_rid_list_t hits;
        edb_rid_list_init(&hits);
        if (sp.kind == SCAN_INDEX_EQ) {
            edb_index_find_eq(sp.index, &sp.lo, &hits);
        } else {
            edb_index_find_range(sp.index,
                                 sp.lo_set ? &sp.lo : NULL, sp.lo_inc,
                                 sp.hi_set ? &sp.hi : NULL, sp.hi_inc, &hits);
        }
        for (size_t i = 0; i < hits.count; i++) {
            edb_row_t row;
            if (edb_table_fetch_row(db, t, hits.items[i], &row)) {
                bool cont = collect_matching_cb(hits.items[i], &row, out);
                edb_row_free(&row);
                if (!cont) break;
            }
        }
        edb_rid_list_free(&hits);
        free_scan_plan(&sp);
    } else {
        edb_table_scan(db, t, collect_matching_cb, out);
    }
}

static void free_scan_collect(scan_collect_t *sc) {
    for (size_t i = 0; i < sc->count; i++) edb_row_free(&sc->rows[i]);
    free(sc->rids);
    free(sc->rows);
}

/* ---- ORDER BY Comparator ---- */
typedef struct {
    edb_row_t row;
    edb_val_t *sort_keys;
} sortable_row_t;

typedef struct {
    size_t key_count;
    bool *desc;
} sort_spec_t;

static sort_spec_t g_sort_spec;
#ifdef _WIN32
static int __cdecl sort_cmp_fn(void *ctx, const void *a, const void *b)
#else
static int sort_cmp_fn(const void *a, const void *b, void *ctx)
#endif
{
    const sort_spec_t *sp = (const sort_spec_t *)ctx;
    const sortable_row_t *ra = (const sortable_row_t *)a;
    const sortable_row_t *rb = (const sortable_row_t *)b;
    for (size_t i = 0; i < sp->key_count; i++) {
        int c = edb_val_cmp(&ra->sort_keys[i], &rb->sort_keys[i]);
        if (c != 0) return sp->desc[i] ? -c : c;
    }
    return 0;
}

static void sort_rows_portable(sortable_row_t *arr, size_t count, const sort_spec_t *sp) {
    if (count < 2) return;
#ifdef _WIN32
    qsort_s(arr, count, sizeof(sortable_row_t), sort_cmp_fn, (void *)sp);
#else
    /* Simple merge/insertion-safe qsort_r or fallback */
    (void)g_sort_spec;
    qsort_r(arr, count, sizeof(sortable_row_t), sort_cmp_fn, (void *)sp);
#endif
}

/* ---- SELECT Execution ---- */
static void expr_display_name(const edb_expr_t *e, char *out, size_t cap) {
    if (!e) { snprintf(out, cap, "col"); return; }
    if (e->kind == EXPR_IDENT) snprintf(out, cap, "%s", e->u.ident.col);
    else if (e->kind == EXPR_CALL) snprintf(out, cap, "%s", e->u.call.func_name);
    else snprintf(out, cap, "expr");
}

static bool extract_equi_join_cols(const edb_expr_t *on_expr,
                                   const char *left_name, const char *left_alias,
                                   const char *right_name, const char *right_alias,
                                   const edb_table_t *lt, const edb_table_t *rt,
                                   int32_t *out_lcol, int32_t *out_rcol) {
    if (!on_expr || on_expr->kind != EXPR_BINARY || on_expr->u.binary.op != OP_EQ) return false;
    const edb_expr_t *a = on_expr->u.binary.left;
    const edb_expr_t *b = on_expr->u.binary.right;
    if (a->kind != EXPR_IDENT || b->kind != EXPR_IDENT) return false;

    bool a_is_left = a->u.ident.table &&
                     ((left_alias && edb_strcasecmp(a->u.ident.table, left_alias) == 0) ||
                      (left_name && edb_strcasecmp(a->u.ident.table, left_name) == 0));
    bool b_is_right = b->u.ident.table &&
                      ((right_alias && edb_strcasecmp(b->u.ident.table, right_alias) == 0) ||
                       (right_name && edb_strcasecmp(b->u.ident.table, right_name) == 0));
    if (a_is_left && b_is_right) {
        *out_lcol = edb_table_col_index(lt, a->u.ident.col);
        *out_rcol = edb_table_col_index(rt, b->u.ident.col);
        return (*out_lcol >= 0 && *out_rcol >= 0);
    }

    bool b_is_left = b->u.ident.table &&
                     ((left_alias && edb_strcasecmp(b->u.ident.table, left_alias) == 0) ||
                      (left_name && edb_strcasecmp(b->u.ident.table, left_name) == 0));
    bool a_is_right = a->u.ident.table &&
                      ((right_alias && edb_strcasecmp(a->u.ident.table, right_alias) == 0) ||
                       (right_name && edb_strcasecmp(a->u.ident.table, right_name) == 0));
    if (b_is_left && a_is_right) {
        *out_lcol = edb_table_col_index(lt, b->u.ident.col);
        *out_rcol = edb_table_col_index(rt, a->u.ident.col);
        return (*out_lcol >= 0 && *out_rcol >= 0);
    }
    return false;
}

static edb_status_t exec_select(edb_engine_t *eng, const edb_stmt_t *stmt,
                                const edb_val_t *params, size_t param_count,
                                edb_result_t *res) {
    edb_db_t *db = eng->db;
    if (db->metrics) edb_atomic_add_u64(&db->metrics->select_count, 1);

    /* SELECT without FROM */
    if (!stmt->u.select.from_table) {
        eval_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.params = params;
        ctx.param_count = param_count;
        result_setup_cols(res, (uint32_t)stmt->u.select.item_count);
        edb_row_t row;
        edb_row_init(&row, (uint32_t)stmt->u.select.item_count);
        for (size_t i = 0; i < stmt->u.select.item_count; i++) {
            char cname[EDB_MAX_NAME];
            if (stmt->u.select.items[i].alias) snprintf(cname, sizeof(cname), "%s", stmt->u.select.items[i].alias);
            else expr_display_name(stmt->u.select.items[i].expr, cname, sizeof(cname));
            res->col_names[i] = edb_strdup(cname);
            row.vals[i] = eval_expr(stmt->u.select.items[i].expr, &ctx);
            res->col_types[i] = row.vals[i].type;
        }
        result_add_row(res, row);
        return EDB_OK;
    }

    size_t src_count = 1 + stmt->u.select.join_count;
    edb_table_t **tables = (edb_table_t **)calloc(src_count, sizeof(edb_table_t *));
    const char **aliases = (const char **)calloc(src_count, sizeof(const char *));
    scan_collect_t *scans = (scan_collect_t *)calloc(src_count, sizeof(scan_collect_t));

    edb_rwlock_rdlock(&db->catalog_lock);
    tables[0] = edb_find_table(db, stmt->u.select.from_table);
    aliases[0] = stmt->u.select.from_alias;
    if (!tables[0]) {
        edb_rwlock_rdunlock(&db->catalog_lock);
        result_set_err(res, EDB_ERR_NOT_FOUND, "Table '%s' not found", stmt->u.select.from_table);
        free(tables); free(aliases); free(scans);
        return EDB_ERR_NOT_FOUND;
    }
    for (size_t j = 0; j < stmt->u.select.join_count; j++) {
        tables[j + 1] = edb_find_table(db, stmt->u.select.joins[j].table_name);
        aliases[j + 1] = stmt->u.select.joins[j].alias;
        if (!tables[j + 1]) {
            edb_rwlock_rdunlock(&db->catalog_lock);
            result_set_err(res, EDB_ERR_NOT_FOUND, "Table '%s' not found", stmt->u.select.joins[j].table_name);
            free(tables); free(aliases); free(scans);
            return EDB_ERR_NOT_FOUND;
        }
    }
    for (size_t s = 0; s < src_count; s++) edb_rwlock_rdlock(&tables[s]->lock);

    bool has_agg = (stmt->u.select.group_count > 0);
    for (size_t i = 0; i < stmt->u.select.item_count && !has_agg; i++) {
        if (expr_has_agg(stmt->u.select.items[i].expr)) has_agg = true;
    }

    int64_t eff_limit = stmt->u.select.limit;
    if (eff_limit < 0 && db->cfg->default_limit > 0) eff_limit = db->cfg->default_limit;
    int64_t early_limit = -1;
    if (src_count == 1 && !has_agg && !stmt->u.select.distinct &&
        stmt->u.select.order_count == 0 && eff_limit >= 0) {
        early_limit = eff_limit + stmt->u.select.offset;
    }

    /* Scan base table */
    collect_table_rows(db, tables[0], (src_count == 1) ? stmt->u.select.where_expr : NULL,
                       params, param_count, early_limit, &scans[0]);

    /* Materialize combined rows across joins */
    typedef struct { const edb_row_t **parts; } joined_tuple_t;
    size_t jt_count = scans[0].count;
    size_t jt_cap = jt_count > 0 ? jt_count : 16;
    joined_tuple_t *tuples = (joined_tuple_t *)calloc(jt_cap, sizeof(joined_tuple_t));
    for (size_t i = 0; i < jt_count; i++) {
        tuples[i].parts = (const edb_row_t **)calloc(src_count, sizeof(edb_row_t *));
        tuples[i].parts[0] = &scans[0].rows[i];
    }

    edb_row_t *null_rows = (edb_row_t *)calloc(src_count, sizeof(edb_row_t));
    for (size_t s = 0; s < src_count; s++) {
        edb_row_init(&null_rows[s], tables[s]->col_count);
        for (uint32_t c = 0; c < tables[s]->col_count; c++) null_rows[s].vals[c] = edb_val_null();
    }

    eval_source_t *eval_srcs = (eval_source_t *)calloc(src_count, sizeof(eval_source_t));
    for (size_t s = 0; s < src_count; s++) {
        eval_srcs[s].table_name = tables[s]->name;
        eval_srcs[s].alias = aliases[s];
        eval_srcs[s].table = tables[s];
    }
    eval_ctx_t jctx;
    memset(&jctx, 0, sizeof(jctx));
    jctx.sources = eval_srcs;
    jctx.params = params;
    jctx.param_count = param_count;

    for (size_t j = 0; j < stmt->u.select.join_count; j++) {
        size_t sidx = j + 1;
        collect_table_rows(db, tables[sidx], NULL, params, param_count, -1, &scans[sidx]);
        edb_join_clause_t *jc = &stmt->u.select.joins[j];
        jctx.source_count = sidx + 1;

        size_t next_cap = 16, next_cnt = 0;
        joined_tuple_t *next_tuples = (joined_tuple_t *)calloc(next_cap, sizeof(joined_tuple_t));

        /* Check if Hash Join is applicable */
        int32_t lcol = -1, rcol = -1;
        bool use_hash = db->cfg->hash_join_enabled && jc->type != JOIN_CROSS && jc->type != JOIN_RIGHT &&
                        extract_equi_join_cols(jc->on_expr, tables[0]->name, aliases[0],
                                               tables[sidx]->name, aliases[sidx],
                                               tables[0], tables[sidx], &lcol, &rcol);

        if (use_hash && scans[sidx].count > 0) {
            size_t hcap = 64;
            while (hcap < scans[sidx].count * 2) hcap *= 2;
            typedef struct hnode { uint64_t h; size_t ridx; struct hnode *next; } hnode_t;
            hnode_t **buckets = (hnode_t **)calloc(hcap, sizeof(hnode_t *));
            hnode_t *pool = (hnode_t *)calloc(scans[sidx].count, sizeof(hnode_t));
            size_t mask = hcap - 1;
            for (size_t r = 0; r < scans[sidx].count; r++) {
                uint64_t h = edb_val_hash(&scans[sidx].rows[r].vals[rcol]);
                pool[r].h = h;
                pool[r].ridx = r;
                pool[r].next = buckets[h & mask];
                buckets[h & mask] = &pool[r];
            }
            for (size_t li = 0; li < jt_count; li++) {
                bool matched = false;
                const edb_val_t *lk = &tuples[li].parts[0]->vals[lcol];
                if (lk->type != EDB_TYPE_NULL) {
                    uint64_t h = edb_val_hash(lk);
                    for (hnode_t *hn = buckets[h & mask]; hn; hn = hn->next) {
                        if (hn->h == h && edb_val_cmp(lk, &scans[sidx].rows[hn->ridx].vals[rcol]) == 0) {
                            matched = true;
                            if (next_cnt == next_cap) {
                                next_cap *= 2;
                                next_tuples = (joined_tuple_t *)realloc(next_tuples, next_cap * sizeof(joined_tuple_t));
                            }
                            next_tuples[next_cnt].parts = (const edb_row_t **)calloc(src_count, sizeof(edb_row_t *));
                            memcpy(next_tuples[next_cnt].parts, tuples[li].parts, sidx * sizeof(edb_row_t *));
                            next_tuples[next_cnt].parts[sidx] = &scans[sidx].rows[hn->ridx];
                            next_cnt++;
                        }
                    }
                }
                if (!matched && jc->type == JOIN_LEFT) {
                    if (next_cnt == next_cap) {
                        next_cap *= 2;
                        next_tuples = (joined_tuple_t *)realloc(next_tuples, next_cap * sizeof(joined_tuple_t));
                    }
                    next_tuples[next_cnt].parts = (const edb_row_t **)calloc(src_count, sizeof(edb_row_t *));
                    memcpy(next_tuples[next_cnt].parts, tuples[li].parts, sidx * sizeof(edb_row_t *));
                    next_tuples[next_cnt].parts[sidx] = &null_rows[sidx];
                    next_cnt++;
                }
            }
            free(buckets);
            free(pool);
        } else {
            bool *right_matched = (jc->type == JOIN_RIGHT && scans[sidx].count > 0)
                                  ? (bool *)calloc(scans[sidx].count, sizeof(bool)) : NULL;
            for (size_t li = 0; li < jt_count; li++) {
                for (size_t k = 0; k < sidx; k++) eval_srcs[k].row = tuples[li].parts[k];
                bool l_matched = false;
                for (size_t ri = 0; ri < scans[sidx].count; ri++) {
                    eval_srcs[sidx].row = &scans[sidx].rows[ri];
                    bool ok = true;
                    if (jc->on_expr && jc->type != JOIN_CROSS) {
                        edb_val_t v = eval_expr(jc->on_expr, &jctx);
                        ok = val_is_truthy(&v);
                        edb_val_free(&v);
                    }
                    if (ok) {
                        l_matched = true;
                        if (right_matched) right_matched[ri] = true;
                        if (next_cnt == next_cap) {
                            next_cap *= 2;
                            next_tuples = (joined_tuple_t *)realloc(next_tuples, next_cap * sizeof(joined_tuple_t));
                        }
                        next_tuples[next_cnt].parts = (const edb_row_t **)calloc(src_count, sizeof(edb_row_t *));
                        memcpy(next_tuples[next_cnt].parts, tuples[li].parts, sidx * sizeof(edb_row_t *));
                        next_tuples[next_cnt].parts[sidx] = &scans[sidx].rows[ri];
                        next_cnt++;
                    }
                }
                if (!l_matched && jc->type == JOIN_LEFT) {
                    if (next_cnt == next_cap) {
                        next_cap *= 2;
                        next_tuples = (joined_tuple_t *)realloc(next_tuples, next_cap * sizeof(joined_tuple_t));
                    }
                    next_tuples[next_cnt].parts = (const edb_row_t **)calloc(src_count, sizeof(edb_row_t *));
                    memcpy(next_tuples[next_cnt].parts, tuples[li].parts, sidx * sizeof(edb_row_t *));
                    next_tuples[next_cnt].parts[sidx] = &null_rows[sidx];
                    next_cnt++;
                }
            }
            if (right_matched) {
                for (size_t ri = 0; ri < scans[sidx].count; ri++) {
                    if (!right_matched[ri]) {
                        if (next_cnt == next_cap) {
                            next_cap *= 2;
                            next_tuples = (joined_tuple_t *)realloc(next_tuples, next_cap * sizeof(joined_tuple_t));
                        }
                        next_tuples[next_cnt].parts = (const edb_row_t **)calloc(src_count, sizeof(edb_row_t *));
                        for (size_t k = 0; k < sidx; k++) next_tuples[next_cnt].parts[k] = &null_rows[k];
                        next_tuples[next_cnt].parts[sidx] = &scans[sidx].rows[ri];
                        next_cnt++;
                    }
                }
                free(right_matched);
            }
        }

        for (size_t i = 0; i < jt_count; i++) free(tuples[i].parts);
        free(tuples);
        tuples = next_tuples;
        jt_count = next_cnt;
    }

    /* Apply WHERE on multi-table join */
    jctx.source_count = src_count;
    if (src_count > 1 && stmt->u.select.where_expr) {
        size_t kept = 0;
        for (size_t i = 0; i < jt_count; i++) {
            for (size_t s = 0; s < src_count; s++) eval_srcs[s].row = tuples[i].parts[s];
            edb_val_t v = eval_expr(stmt->u.select.where_expr, &jctx);
            bool ok = val_is_truthy(&v);
            edb_val_free(&v);
            if (ok) {
                tuples[kept++] = tuples[i];
            } else {
                free(tuples[i].parts);
            }
        }
        jt_count = kept;
    }

    /* Expand '*' in SELECT items */
    typedef struct {
        bool is_col_ref;
        size_t src_idx;
        uint32_t col_idx;
        const edb_expr_t *expr;
        char name[EDB_MAX_NAME];
        edb_type_t type;
    } proj_col_t;

    size_t proj_cap = stmt->u.select.item_count + 16;
    size_t proj_cnt = 0;
    proj_col_t *projs = (proj_col_t *)calloc(proj_cap, sizeof(proj_col_t));
    for (size_t i = 0; i < stmt->u.select.item_count; i++) {
        const edb_expr_t *ex = stmt->u.select.items[i].expr;
        if (ex->kind == EXPR_STAR) {
            for (size_t s = 0; s < src_count; s++) {
                for (uint32_t c = 0; c < tables[s]->col_count; c++) {
                    if (proj_cnt == proj_cap) {
                        proj_cap *= 2;
                        projs = (proj_col_t *)realloc(projs, proj_cap * sizeof(proj_col_t));
                    }
                    projs[proj_cnt].is_col_ref = true;
                    projs[proj_cnt].src_idx = s;
                    projs[proj_cnt].col_idx = c;
                    projs[proj_cnt].expr = NULL;
                    snprintf(projs[proj_cnt].name, EDB_MAX_NAME, "%s", tables[s]->cols[c].name);
                    projs[proj_cnt].type = tables[s]->cols[c].type;
                    proj_cnt++;
                }
            }
        } else {
            if (proj_cnt == proj_cap) {
                proj_cap *= 2;
                projs = (proj_col_t *)realloc(projs, proj_cap * sizeof(proj_col_t));
            }
            projs[proj_cnt].is_col_ref = false;
            projs[proj_cnt].expr = ex;
            projs[proj_cnt].type = EDB_TYPE_NULL;
            if (stmt->u.select.items[i].alias) {
                snprintf(projs[proj_cnt].name, EDB_MAX_NAME, "%s", stmt->u.select.items[i].alias);
            } else {
                expr_display_name(ex, projs[proj_cnt].name, EDB_MAX_NAME);
            }
            if (ex->kind == EXPR_IDENT) {
                size_t si = 0; uint32_t ci = 0;
                if (resolve_ident_col(&jctx, ex->u.ident.table, ex->u.ident.col, &si, &ci)) {
                    projs[proj_cnt].type = tables[si]->cols[ci].type;
                }
            }
            proj_cnt++;
        }
    }

    result_setup_cols(res, (uint32_t)proj_cnt);
    for (size_t i = 0; i < proj_cnt; i++) {
        res->col_names[i] = edb_strdup(projs[i].name);
        res->col_types[i] = projs[i].type;
    }

    /* Project & Group/Aggregate */
    size_t out_cnt = 0;
    size_t out_cap = jt_count > 0 ? jt_count : 16;
    sortable_row_t *out_rows = (sortable_row_t *)calloc(out_cap, sizeof(sortable_row_t));

    if (has_agg) {
        typedef struct {
            edb_val_t *gkeys;
            const edb_row_t *const **tuples;
            size_t rcnt;
            size_t rcap;
        } group_t;
        size_t gcap = 16, gcnt = 0;
        group_t *groups = (group_t *)calloc(gcap, sizeof(group_t));
        size_t gcols = stmt->u.select.group_count;

        if (jt_count == 0 && gcols == 0) {
            /* Aggregate over 0 rows produces 1 row (COUNT=0, SUM=NULL) */
            groups[0].rcnt = 0;
            gcnt = 1;
        } else {
            for (size_t i = 0; i < jt_count; i++) {
                for (size_t s = 0; s < src_count; s++) eval_srcs[s].row = tuples[i].parts[s];
                edb_val_t *gk = gcols > 0 ? (edb_val_t *)calloc(gcols, sizeof(edb_val_t)) : NULL;
                for (size_t g = 0; g < gcols; g++) gk[g] = eval_expr(stmt->u.select.group_by[g], &jctx);

                size_t found_g = (size_t)-1;
                for (size_t gi = 0; gi < gcnt; gi++) {
                    bool eq = true;
                    for (size_t g = 0; g < gcols; g++) {
                        if (edb_val_cmp(&groups[gi].gkeys[g], &gk[g]) != 0) { eq = false; break; }
                    }
                    if (eq) { found_g = gi; break; }
                }
                if (found_g == (size_t)-1) {
                    if (gcnt == gcap) {
                        gcap *= 2;
                        groups = (group_t *)realloc(groups, gcap * sizeof(group_t));
                    }
                    found_g = gcnt++;
                    groups[found_g].gkeys = gk;
                    groups[found_g].rcap = 8;
                    groups[found_g].rcnt = 0;
                    groups[found_g].tuples = (const edb_row_t *const **)calloc(8, sizeof(const edb_row_t **));
                } else {
                    for (size_t g = 0; g < gcols; g++) edb_val_free(&gk[g]);
                    free(gk);
                }
                group_t *grp = &groups[found_g];
                if (grp->rcnt == grp->rcap) {
                    grp->rcap *= 2;
                    grp->tuples = (const edb_row_t *const **)realloc(grp->tuples, grp->rcap * sizeof(const edb_row_t **));
                }
                grp->tuples[grp->rcnt++] = (const edb_row_t *const *)tuples[i].parts;
            }
        }

        for (size_t gi = 0; gi < gcnt; gi++) {
            jctx.in_aggregate = true;
            jctx.group_tuples = groups[gi].tuples;
            jctx.group_row_count = groups[gi].rcnt;
            if (groups[gi].rcnt > 0) {
                for (size_t s = 0; s < src_count; s++) eval_srcs[s].row = groups[gi].tuples[0][s];
            }

            bool pass_having = true;
            if (stmt->u.select.having_expr) {
                edb_val_t hv = eval_expr(stmt->u.select.having_expr, &jctx);
                pass_having = val_is_truthy(&hv);
                edb_val_free(&hv);
            }
            if (pass_having) {
                if (out_cnt == out_cap) {
                    out_cap *= 2;
                    out_rows = (sortable_row_t *)realloc(out_rows, out_cap * sizeof(sortable_row_t));
                }
                edb_row_init(&out_rows[out_cnt].row, (uint32_t)proj_cnt);
                for (size_t p = 0; p < proj_cnt; p++) {
                    if (projs[p].is_col_ref) {
                        out_rows[out_cnt].row.vals[p] = groups[gi].rcnt > 0
                            ? edb_val_clone(&groups[gi].tuples[0][projs[p].src_idx]->vals[projs[p].col_idx])
                            : edb_val_null();
                    } else {
                        out_rows[out_cnt].row.vals[p] = eval_expr(projs[p].expr, &jctx);
                    }
                }
                if (stmt->u.select.order_count > 0) {
                    out_rows[out_cnt].sort_keys = (edb_val_t *)calloc(stmt->u.select.order_count, sizeof(edb_val_t));
                    for (size_t o = 0; o < stmt->u.select.order_count; o++) {
                        const edb_expr_t *oe = stmt->u.select.order_by[o].expr;
                        bool matched_alias = false;
                        if (oe->kind == EXPR_IDENT && !oe->u.ident.table) {
                            for (size_t p = 0; p < proj_cnt; p++) {
                                if (edb_strcasecmp(projs[p].name, oe->u.ident.col) == 0) {
                                    out_rows[out_cnt].sort_keys[o] = edb_val_clone(&out_rows[out_cnt].row.vals[p]);
                                    matched_alias = true;
                                    break;
                                }
                            }
                        }
                        if (!matched_alias) {
                            out_rows[out_cnt].sort_keys[o] = eval_expr(oe, &jctx);
                        }
                    }
                }
                out_cnt++;
            }
            jctx.in_aggregate = false;
            for (size_t g = 0; g < gcols; g++) edb_val_free(&groups[gi].gkeys[g]);
            free(groups[gi].gkeys);
            free((void *)groups[gi].tuples);
        }
        free(groups);
    } else {
        for (size_t i = 0; i < jt_count; i++) {
            for (size_t s = 0; s < src_count; s++) eval_srcs[s].row = tuples[i].parts[s];
            if (out_cnt == out_cap) {
                out_cap *= 2;
                out_rows = (sortable_row_t *)realloc(out_rows, out_cap * sizeof(sortable_row_t));
            }
            edb_row_init(&out_rows[out_cnt].row, (uint32_t)proj_cnt);
            for (size_t p = 0; p < proj_cnt; p++) {
                if (projs[p].is_col_ref) {
                    const edb_row_t *sr = tuples[i].parts[projs[p].src_idx];
                    out_rows[out_cnt].row.vals[p] = edb_val_clone(&sr->vals[projs[p].col_idx]);
                } else {
                    out_rows[out_cnt].row.vals[p] = eval_expr(projs[p].expr, &jctx);
                }
            }
            if (stmt->u.select.order_count > 0) {
                out_rows[out_cnt].sort_keys = (edb_val_t *)calloc(stmt->u.select.order_count, sizeof(edb_val_t));
                for (size_t o = 0; o < stmt->u.select.order_count; o++) {
                    const edb_expr_t *oe = stmt->u.select.order_by[o].expr;
                    bool matched_alias = false;
                    if (oe->kind == EXPR_IDENT && !oe->u.ident.table) {
                        for (size_t p = 0; p < proj_cnt; p++) {
                            if (edb_strcasecmp(projs[p].name, oe->u.ident.col) == 0) {
                                out_rows[out_cnt].sort_keys[o] = edb_val_clone(&out_rows[out_cnt].row.vals[p]);
                                matched_alias = true;
                                break;
                            }
                        }
                    }
                    if (!matched_alias) {
                        out_rows[out_cnt].sort_keys[o] = eval_expr(oe, &jctx);
                    }
                }
            }
            out_cnt++;
        }
    }

    /* Release table locks early after materializing rows */
    for (size_t s = 0; s < src_count; s++) edb_rwlock_rdunlock(&tables[s]->lock);
    edb_rwlock_rdunlock(&db->catalog_lock);

    /* DISTINCT */
    if (stmt->u.select.distinct && out_cnt > 1) {
        size_t uniq = 0;
        for (size_t i = 0; i < out_cnt; i++) {
            bool dup = false;
            for (size_t k = 0; k < uniq; k++) {
                bool eq = true;
                for (size_t c = 0; c < proj_cnt; c++) {
                    if (edb_val_cmp(&out_rows[k].row.vals[c], &out_rows[i].row.vals[c]) != 0) {
                        eq = false;
                        break;
                    }
                }
                if (eq) { dup = true; break; }
            }
            if (!dup) {
                out_rows[uniq++] = out_rows[i];
            } else {
                edb_row_free(&out_rows[i].row);
                for (size_t o = 0; o < stmt->u.select.order_count; o++) edb_val_free(&out_rows[i].sort_keys[o]);
                free(out_rows[i].sort_keys);
            }
        }
        out_cnt = uniq;
    }

    /* ORDER BY */
    if (stmt->u.select.order_count > 0 && out_cnt > 1) {
        sort_spec_t sp;
        sp.key_count = stmt->u.select.order_count;
        sp.desc = (bool *)calloc(sp.key_count, sizeof(bool));
        for (size_t o = 0; o < sp.key_count; o++) sp.desc[o] = stmt->u.select.order_by[o].desc;
        sort_rows_portable(out_rows, out_cnt, &sp);
        free(sp.desc);
    }

    /* OFFSET & LIMIT & max_rows_returned */
    size_t start_idx = stmt->u.select.offset > 0 ? (size_t)stmt->u.select.offset : 0;
    size_t end_idx = out_cnt;
    if (eff_limit >= 0 && start_idx + (size_t)eff_limit < end_idx) {
        end_idx = start_idx + (size_t)eff_limit;
    }
    if (db->cfg->max_rows_returned > 0 && end_idx > start_idx &&
        (end_idx - start_idx) > (size_t)db->cfg->max_rows_returned) {
        end_idx = start_idx + (size_t)db->cfg->max_rows_returned;
    }

    for (size_t i = 0; i < out_cnt; i++) {
        for (size_t o = 0; o < stmt->u.select.order_count; o++) edb_val_free(&out_rows[i].sort_keys[o]);
        free(out_rows[i].sort_keys);
        if (i >= start_idx && i < end_idx) {
            result_add_row(res, out_rows[i].row);
        } else {
            edb_row_free(&out_rows[i].row);
        }
    }

    if (res->row_count > 0) {
        for (uint32_t c = 0; c < res->col_count; c++) {
            if (res->col_types[c] == EDB_TYPE_NULL) res->col_types[c] = res->rows[0].vals[c].type;
        }
    }

    free(out_rows);
    free(projs);
    for (size_t i = 0; i < jt_count; i++) free(tuples[i].parts);
    free(tuples);
    for (size_t s = 0; s < src_count; s++) {
        edb_row_free(&null_rows[s]);
        free_scan_collect(&scans[s]);
    }
    free(null_rows);
    free(eval_srcs);
    free(tables);
    free(aliases);
    free(scans);
    return EDB_OK;
}

/* ---- Statement Dispatch ---- */
static bool stmt_collect_tables(const edb_stmt_t *s, edb_db_t *db,
                                uint32_t *tids, uint64_t *tvers, uint32_t *tcnt) {
    *tcnt = 0;
    if (s->kind != STMT_SELECT || !s->u.select.from_table) return true;
    edb_rwlock_rdlock(&db->catalog_lock);
    edb_table_t *t = edb_find_table(db, s->u.select.from_table);
    if (!t) { edb_rwlock_rdunlock(&db->catalog_lock); return false; }
    tids[*tcnt] = t->id;
    tvers[*tcnt] = edb_atomic_load_u64(&t->version);
    (*tcnt)++;
    for (size_t j = 0; j < s->u.select.join_count && *tcnt < 8; j++) {
        edb_table_t *jt = edb_find_table(db, s->u.select.joins[j].table_name);
        if (!jt) { edb_rwlock_rdunlock(&db->catalog_lock); return false; }
        tids[*tcnt] = jt->id;
        tvers[*tcnt] = edb_atomic_load_u64(&jt->version);
        (*tcnt)++;
    }
    edb_rwlock_rdunlock(&db->catalog_lock);
    return true;
}

static bool select_is_cacheable(const edb_stmt_t *s) {
    if (s->kind != STMT_SELECT || !s->u.select.from_table) return false;
    if (expr_has_now(s->u.select.where_expr)) return false;
    for (size_t i = 0; i < s->u.select.item_count; i++) {
        if (expr_has_now(s->u.select.items[i].expr)) return false;
    }
    return true;
}

static edb_stmt_t *get_or_parse_stmt(edb_engine_t *eng, const char *sql, size_t sql_len,
                                     edb_arena_t *fallback_arena, bool *out_cached_plan,
                                     char *err, size_t err_cap) {
    *out_cached_plan = false;
    uint64_t h = edb_hash64(sql, sql_len);
    if (eng->db->cfg->plan_cache_enabled) {
        edb_plan_shard_t *sh = &eng->plan_shards[h & (EDB_CACHE_SHARDS - 1)];
        edb_mutex_lock(&sh->mu);
        for (edb_plan_entry_t *e = sh->head; e; e = e->next) {
            if (e->hash == h && e->sql_len == sql_len && memcmp(e->sql, sql, sql_len) == 0) {
                e->last_used = ++sh->seq;
                *out_cached_plan = true;
                if (eng->db->metrics) edb_atomic_add_u64(&eng->db->metrics->plan_cache_hits, 1);
                edb_stmt_t *st = e->stmt;
                edb_mutex_unlock(&sh->mu);
                return st;
            }
        }
        edb_mutex_unlock(&sh->mu);
        if (eng->db->metrics) edb_atomic_add_u64(&eng->db->metrics->plan_cache_misses, 1);

        edb_plan_entry_t *ne = (edb_plan_entry_t *)calloc(1, sizeof(edb_plan_entry_t));
        edb_arena_init(&ne->arena);
        edb_stmt_t *parsed = NULL;
        if (!edb_sql_parse(sql, sql_len, &ne->arena, &parsed, err, err_cap)) {
            edb_arena_free(&ne->arena);
            free(ne);
            return NULL;
        }
        ne->hash = h;
        ne->sql = edb_strndup(sql, sql_len);
        ne->sql_len = sql_len;
        ne->stmt = parsed;

        edb_mutex_lock(&sh->mu);
        if (sh->count >= sh->max_entries && sh->head) {
            /* Evict tail/oldest */
            edb_plan_entry_t **pp = &sh->head;
            while ((*pp)->next) pp = &(*pp)->next;
            edb_plan_entry_t *victim = *pp;
            *pp = NULL;
            free(victim->sql);
            edb_arena_free(&victim->arena);
            free(victim);
            sh->count--;
        }
        ne->last_used = ++sh->seq;
        ne->next = sh->head;
        sh->head = ne;
        sh->count++;
        edb_mutex_unlock(&sh->mu);
        *out_cached_plan = true;
        return parsed;
    }

    edb_stmt_t *parsed = NULL;
    if (!edb_sql_parse(sql, sql_len, fallback_arena, &parsed, err, err_cap)) return NULL;
    return parsed;
}

static edb_status_t exec_stmt(edb_engine_t *eng, edb_session_t *sess, const edb_stmt_t *stmt,
                              const edb_val_t *params, size_t param_count,
                              const char *raw_sql, size_t raw_len, edb_result_t *res) {
    edb_db_t *db = eng->db;
    char err[256] = {0};

    /* Read-only enforcement for mutating statements */
    if (db->cfg->read_only &&
        (stmt->kind == STMT_INSERT || stmt->kind == STMT_UPDATE || stmt->kind == STMT_DELETE ||
         stmt->kind == STMT_CREATE_TABLE || stmt->kind == STMT_DROP_TABLE ||
         stmt->kind == STMT_ALTER_ADD_COL || stmt->kind == STMT_CREATE_INDEX ||
         stmt->kind == STMT_DROP_INDEX)) {
        result_set_err(res, EDB_ERR_READONLY, "Database is in read-only mode");
        return EDB_ERR_READONLY;
    }

    switch (stmt->kind) {
    case STMT_PING: {
        result_setup_cols(res, 1);
        res->col_names[0] = edb_strdup("pong");
        res->col_types[0] = EDB_TYPE_TEXT;
        edb_row_t r;
        edb_row_init(&r, 1);
        r.vals[0] = edb_val_text_copy("PONG", 4);
        result_add_row(res, r);
        return EDB_OK;
    }
    case STMT_BEGIN: {
        if (sess->txn) {
            result_set_err(res, EDB_ERR_TXN, "Transaction already active");
            return EDB_ERR_TXN;
        }
        sess->txn = edb_txn_begin(db);
        return sess->txn ? EDB_OK : EDB_ERR_OOM;
    }
    case STMT_COMMIT: {
        if (!sess->txn) return EDB_OK;
        edb_status_t st = edb_txn_commit(db, sess->txn);
        sess->txn = NULL;
        return st;
    }
    case STMT_ROLLBACK: {
        if (!sess->txn) return EDB_OK;
        edb_status_t st = edb_txn_rollback(db, sess->txn);
        sess->txn = NULL;
        return st;
    }
    case STMT_CHECKPOINT:
        return edb_db_checkpoint(db);
    case STMT_VACUUM:
        return edb_db_vacuum(db);

    case STMT_CREATE_TABLE: {
        if (db->metrics) edb_atomic_add_u64(&db->metrics->ddl_count, 1);
        edb_status_t st = edb_create_table(db, stmt->u.create_table.table_name,
                                           stmt->u.create_table.cols,
                                           (uint32_t)stmt->u.create_table.col_count,
                                           stmt->u.create_table.if_not_exists, err, sizeof(err));
        if (st != EDB_OK) result_set_err(res, st, "%s", err);
        return st;
    }
    case STMT_DROP_TABLE: {
        if (db->metrics) edb_atomic_add_u64(&db->metrics->ddl_count, 1);
        edb_status_t st = edb_drop_table(db, stmt->u.drop_table.table_name,
                                         stmt->u.drop_table.if_exists, err, sizeof(err));
        if (st != EDB_OK) result_set_err(res, st, "%s", err);
        return st;
    }
    case STMT_ALTER_ADD_COL: {
        if (db->metrics) edb_atomic_add_u64(&db->metrics->ddl_count, 1);
        edb_status_t st = edb_alter_add_column(db, stmt->u.alter_add.table_name,
                                               &stmt->u.alter_add.col, err, sizeof(err));
        if (st != EDB_OK) result_set_err(res, st, "%s", err);
        return st;
    }
    case STMT_CREATE_INDEX: {
        if (db->metrics) edb_atomic_add_u64(&db->metrics->ddl_count, 1);
        edb_status_t st = edb_create_index(db, stmt->u.create_index.index_name,
                                           stmt->u.create_index.table_name,
                                           stmt->u.create_index.col_name,
                                           stmt->u.create_index.idx_type,
                                           stmt->u.create_index.unique,
                                           stmt->u.create_index.if_not_exists, err, sizeof(err));
        if (st != EDB_OK) result_set_err(res, st, "%s", err);
        return st;
    }
    case STMT_DROP_INDEX: {
        if (db->metrics) edb_atomic_add_u64(&db->metrics->ddl_count, 1);
        edb_status_t st = edb_drop_index(db, stmt->u.drop_index.index_name,
                                         stmt->u.drop_index.if_exists, err, sizeof(err));
        if (st != EDB_OK) result_set_err(res, st, "%s", err);
        return st;
    }

    case STMT_INSERT: {
        if (db->metrics) edb_atomic_add_u64(&db->metrics->insert_count, 1);
        edb_rwlock_rdlock(&db->catalog_lock);
        edb_table_t *t = edb_find_table(db, stmt->u.insert.table_name);
        if (!t) {
            edb_rwlock_rdunlock(&db->catalog_lock);
            result_set_err(res, EDB_ERR_NOT_FOUND, "Table '%s' not found", stmt->u.insert.table_name);
            return EDB_ERR_NOT_FOUND;
        }
        edb_rwlock_wrlock(&t->lock);

        uint32_t *col_map = (uint32_t *)calloc(stmt->u.insert.row_width, sizeof(uint32_t));
        if (stmt->u.insert.col_count > 0) {
            if (stmt->u.insert.col_count != stmt->u.insert.row_width) {
                free(col_map);
                edb_rwlock_wrunlock(&t->lock);
                edb_rwlock_rdunlock(&db->catalog_lock);
                result_set_err(res, EDB_ERR_SYNTAX, "Column count does not match VALUES count");
                return EDB_ERR_SYNTAX;
            }
            for (size_t c = 0; c < stmt->u.insert.col_count; c++) {
                int32_t ci = edb_table_col_index(t, stmt->u.insert.col_names[c]);
                if (ci < 0) {
                    free(col_map);
                    edb_rwlock_wrunlock(&t->lock);
                    edb_rwlock_rdunlock(&db->catalog_lock);
                    result_set_err(res, EDB_ERR_NOT_FOUND, "Column '%s' not found", stmt->u.insert.col_names[c]);
                    return EDB_ERR_NOT_FOUND;
                }
                col_map[c] = (uint32_t)ci;
            }
        } else {
            if (stmt->u.insert.row_width > t->col_count) {
                free(col_map);
                edb_rwlock_wrunlock(&t->lock);
                edb_rwlock_rdunlock(&db->catalog_lock);
                result_set_err(res, EDB_ERR_SYNTAX, "Too many values for table '%s'", t->name);
                return EDB_ERR_SYNTAX;
            }
            for (size_t c = 0; c < stmt->u.insert.row_width; c++) col_map[c] = (uint32_t)c;
        }

        eval_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.params = params;
        ctx.param_count = param_count;

        edb_status_t st = EDB_OK;
        for (size_t r = 0; r < stmt->u.insert.row_count; r++) {
            edb_row_t row;
            edb_row_init(&row, t->col_count);
            for (size_t c = 0; c < stmt->u.insert.row_width; c++) {
                row.vals[col_map[c]] = eval_expr(stmt->u.insert.rows[r][c], &ctx);
            }
            edb_rid_t rid = 0;
            int64_t inc_id = 0;
            st = edb_table_insert(db, t, sess->txn, &row, &rid, &inc_id, err, sizeof(err));
            edb_row_free(&row);
            if (st != EDB_OK) {
                result_set_err(res, st, "%s", err);
                break;
            }
            res->rows_affected++;
            if (inc_id > 0) res->last_insert_id = (uint64_t)inc_id;
        }
        free(col_map);
        edb_rwlock_wrunlock(&t->lock);
        edb_rwlock_rdunlock(&db->catalog_lock);
        return st;
    }

    case STMT_UPDATE: {
        if (db->metrics) edb_atomic_add_u64(&db->metrics->update_count, 1);
        edb_rwlock_rdlock(&db->catalog_lock);
        edb_table_t *t = edb_find_table(db, stmt->u.update.table_name);
        if (!t) {
            edb_rwlock_rdunlock(&db->catalog_lock);
            result_set_err(res, EDB_ERR_NOT_FOUND, "Table '%s' not found", stmt->u.update.table_name);
            return EDB_ERR_NOT_FOUND;
        }
        edb_rwlock_wrlock(&t->lock);

        int32_t *col_map = (int32_t *)calloc(stmt->u.update.assign_count, sizeof(int32_t));
        for (size_t a = 0; a < stmt->u.update.assign_count; a++) {
            col_map[a] = edb_table_col_index(t, stmt->u.update.assigns[a].col);
            if (col_map[a] < 0) {
                free(col_map);
                edb_rwlock_wrunlock(&t->lock);
                edb_rwlock_rdunlock(&db->catalog_lock);
                result_set_err(res, EDB_ERR_NOT_FOUND, "Column '%s' not found", stmt->u.update.assigns[a].col);
                return EDB_ERR_NOT_FOUND;
            }
        }

        scan_collect_t targets;
        collect_table_rows(db, t, stmt->u.update.where_expr, params, param_count, -1, &targets);

        eval_source_t src = {t->name, NULL, t, NULL};
        eval_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.sources = &src;
        ctx.source_count = 1;
        ctx.params = params;
        ctx.param_count = param_count;

        edb_status_t st = EDB_OK;
        for (size_t i = 0; i < targets.count; i++) {
            src.row = &targets.rows[i];
            edb_row_t new_row = edb_row_clone(&targets.rows[i]);
            for (size_t a = 0; a < stmt->u.update.assign_count; a++) {
                edb_val_free(&new_row.vals[col_map[a]]);
                new_row.vals[col_map[a]] = eval_expr(stmt->u.update.assigns[a].val, &ctx);
            }
            st = edb_table_update(db, t, sess->txn, targets.rids[i], &new_row, err, sizeof(err));
            edb_row_free(&new_row);
            if (st != EDB_OK) {
                result_set_err(res, st, "%s", err);
                break;
            }
            res->rows_affected++;
        }

        free_scan_collect(&targets);
        free(col_map);
        edb_rwlock_wrunlock(&t->lock);
        edb_rwlock_rdunlock(&db->catalog_lock);
        return st;
    }

    case STMT_DELETE: {
        if (db->metrics) edb_atomic_add_u64(&db->metrics->delete_count, 1);
        edb_rwlock_rdlock(&db->catalog_lock);
        edb_table_t *t = edb_find_table(db, stmt->u.del.table_name);
        if (!t) {
            edb_rwlock_rdunlock(&db->catalog_lock);
            result_set_err(res, EDB_ERR_NOT_FOUND, "Table '%s' not found", stmt->u.del.table_name);
            return EDB_ERR_NOT_FOUND;
        }
        edb_rwlock_wrlock(&t->lock);

        scan_collect_t targets;
        collect_table_rows(db, t, stmt->u.del.where_expr, params, param_count, -1, &targets);
        edb_status_t st = EDB_OK;
        for (size_t i = 0; i < targets.count; i++) {
            st = edb_table_delete(db, t, sess->txn, targets.rids[i], err, sizeof(err));
            if (st != EDB_OK) {
                result_set_err(res, st, "%s", err);
                break;
            }
            res->rows_affected++;
        }
        free_scan_collect(&targets);
        edb_rwlock_wrunlock(&t->lock);
        edb_rwlock_rdunlock(&db->catalog_lock);
        return st;
    }

    case STMT_SELECT: {
        if (!sess->txn && db->cfg->result_cache_enabled && param_count == 0 &&
            raw_sql && select_is_cacheable(stmt)) {
            uint64_t h = edb_hash64(raw_sql, raw_len);
            edb_res_shard_t *sh = &eng->res_shards[h & (EDB_CACHE_SHARDS - 1)];
            uint64_t now_ms = edb_now_ms();
            uint64_t cur_schema = edb_atomic_load_u64(&db->schema_version);

            edb_mutex_lock(&sh->mu);
            edb_res_entry_t *prev = NULL, *cur = sh->head;
            while (cur) {
                if (cur->hash == h && cur->sql_len == raw_len && memcmp(cur->sql, raw_sql, raw_len) == 0) {
                    bool valid = (cur->schema_version == cur_schema);
                    if (valid && db->cfg->result_cache_ttl_ms > 0 &&
                        (now_ms - cur->created_ms) > (uint64_t)db->cfg->result_cache_ttl_ms) {
                        valid = false;
                    }
                    if (valid) {
                        edb_rwlock_rdlock(&db->catalog_lock);
                        for (uint32_t ti = 0; ti < cur->table_count; ti++) {
                            edb_table_t *t = edb_find_table_by_id(db, cur->table_ids[ti]);
                            if (!t || edb_atomic_load_u64(&t->version) != cur->table_versions[ti]) {
                                valid = false;
                                break;
                            }
                        }
                        edb_rwlock_rdunlock(&db->catalog_lock);
                    }
                    if (valid) {
                        *res = result_clone(&cur->result);
                        res->from_cache = true;
                        edb_mutex_unlock(&sh->mu);
                        if (db->metrics) edb_atomic_add_u64(&db->metrics->result_cache_hits, 1);
                        return EDB_OK;
                    }
                    /* Expired or invalidated: unlink */
                    if (prev) prev->next = cur->next;
                    else sh->head = cur->next;
                    sh->total_bytes -= cur->approx_bytes;
                    free(cur->sql);
                    edb_result_free(&cur->result);
                    free(cur);
                    break;
                }
                prev = cur;
                cur = cur->next;
            }
            edb_mutex_unlock(&sh->mu);
            if (db->metrics) edb_atomic_add_u64(&db->metrics->result_cache_misses, 1);
        }

        edb_status_t st = exec_select(eng, stmt, params, param_count, res);
        if (st == EDB_OK && !sess->txn && db->cfg->result_cache_enabled && param_count == 0 &&
            raw_sql && select_is_cacheable(stmt) && res->row_count <= 10000) {
            uint32_t tids[8]; uint64_t tvers[8]; uint32_t tcnt = 0;
            if (stmt_collect_tables(stmt, db, tids, tvers, &tcnt)) {
                size_t approx = 128 + raw_len + (size_t)res->row_count * (res->col_count * 24 + 16);
                uint64_t h = edb_hash64(raw_sql, raw_len);
                edb_res_shard_t *sh = &eng->res_shards[h & (EDB_CACHE_SHARDS - 1)];
                if (approx <= sh->max_bytes / 2) {
                    edb_res_entry_t *ne = (edb_res_entry_t *)calloc(1, sizeof(edb_res_entry_t));
                    ne->hash = h;
                    ne->sql = edb_strndup(raw_sql, raw_len);
                    ne->sql_len = raw_len;
                    ne->schema_version = edb_atomic_load_u64(&db->schema_version);
                    ne->table_count = tcnt;
                    memcpy(ne->table_ids, tids, sizeof(tids));
                    memcpy(ne->table_versions, tvers, sizeof(tvers));
                    ne->created_ms = edb_now_ms();
                    ne->approx_bytes = approx;
                    ne->result = result_clone(res);

                    edb_mutex_lock(&sh->mu);
                    while (sh->total_bytes + approx > sh->max_bytes && sh->head) {
                        edb_res_entry_t **pp = &sh->head;
                        while ((*pp)->next) pp = &(*pp)->next;
                        edb_res_entry_t *v = *pp;
                        *pp = NULL;
                        sh->total_bytes -= v->approx_bytes;
                        free(v->sql);
                        edb_result_free(&v->result);
                        free(v);
                    }
                    ne->next = sh->head;
                    sh->head = ne;
                    sh->total_bytes += approx;
                    edb_mutex_unlock(&sh->mu);
                }
            }
        }
        return st;
    }

    case STMT_SHOW_TABLES: {
        result_setup_cols(res, 4);
        res->col_names[0] = edb_strdup("table_name"); res->col_types[0] = EDB_TYPE_TEXT;
        res->col_names[1] = edb_strdup("columns");    res->col_types[1] = EDB_TYPE_INT;
        res->col_names[2] = edb_strdup("rows");       res->col_types[2] = EDB_TYPE_INT;
        res->col_names[3] = edb_strdup("indexes");    res->col_types[3] = EDB_TYPE_INT;
        edb_rwlock_rdlock(&db->catalog_lock);
        for (size_t i = 0; i < db->table_count; i++) {
            edb_table_t *t = db->tables[i];
            edb_row_t r;
            edb_row_init(&r, 4);
            r.vals[0] = edb_val_text_copy(t->name, (uint32_t)strlen(t->name));
            r.vals[1] = edb_val_int((int64_t)t->col_count);
            r.vals[2] = edb_val_int((int64_t)t->row_count);
            r.vals[3] = edb_val_int((int64_t)t->index_count);
            result_add_row(res, r);
        }
        edb_rwlock_rdunlock(&db->catalog_lock);
        return EDB_OK;
    }

    case STMT_DESCRIBE: {
        edb_rwlock_rdlock(&db->catalog_lock);
        edb_table_t *t = edb_find_table(db, stmt->u.describe.table_name);
        if (!t) {
            edb_rwlock_rdunlock(&db->catalog_lock);
            result_set_err(res, EDB_ERR_NOT_FOUND, "Table '%s' not found", stmt->u.describe.table_name);
            return EDB_ERR_NOT_FOUND;
        }
        result_setup_cols(res, 5);
        res->col_names[0] = edb_strdup("column");      res->col_types[0] = EDB_TYPE_TEXT;
        res->col_names[1] = edb_strdup("type");        res->col_types[1] = EDB_TYPE_TEXT;
        res->col_names[2] = edb_strdup("nullable");    res->col_types[2] = EDB_TYPE_BOOL;
        res->col_names[3] = edb_strdup("primary_key"); res->col_types[3] = EDB_TYPE_BOOL;
        res->col_names[4] = edb_strdup("indexed");     res->col_types[4] = EDB_TYPE_TEXT;
        edb_rwlock_rdlock(&t->lock);
        for (uint32_t i = 0; i < t->col_count; i++) {
            edb_row_t r;
            edb_row_init(&r, 5);
            const char *tn = edb_type_name(t->cols[i].type);
            edb_index_t *idx = edb_table_find_index_for_col(t, i);
            const char *in = idx ? (idx->type == EDB_IDX_BTREE ? "BTREE" : "HASH") : "";
            r.vals[0] = edb_val_text_copy(t->cols[i].name, (uint32_t)strlen(t->cols[i].name));
            r.vals[1] = edb_val_text_copy(tn, (uint32_t)strlen(tn));
            r.vals[2] = edb_val_bool(!t->cols[i].not_null);
            r.vals[3] = edb_val_bool(t->cols[i].primary_key);
            r.vals[4] = edb_val_text_copy(in, (uint32_t)strlen(in));
            result_add_row(res, r);
        }
        edb_rwlock_rdunlock(&t->lock);
        edb_rwlock_rdunlock(&db->catalog_lock);
        return EDB_OK;
    }

    case STMT_SHOW_CONFIG: {
        result_setup_cols(res, 3);
        res->col_names[0] = edb_strdup("key");         res->col_types[0] = EDB_TYPE_TEXT;
        res->col_names[1] = edb_strdup("value");       res->col_types[1] = EDB_TYPE_TEXT;
        res->col_names[2] = edb_strdup("description"); res->col_types[2] = EDB_TYPE_TEXT;
        size_t scount = 0;
        const edb_config_spec_t *specs = edb_config_specs(&scount);
        for (size_t i = 0; i < scount; i++) {
            char fullk[192], valb[EDB_MAX_PATH];
            snprintf(fullk, sizeof(fullk), "%s.%s", specs[i].section, specs[i].key);
            if (stmt->u.show_config.like_pattern &&
                !edb_like_match(fullk, stmt->u.show_config.like_pattern, true)) {
                continue;
            }
            edb_config_get_kv_str(db->cfg, &specs[i], valb, sizeof(valb));
            if (strcmp(specs[i].key, "admin_password") == 0 && valb[0] != '\0') {
                snprintf(valb, sizeof(valb), "********");
            }
            edb_row_t r;
            edb_row_init(&r, 3);
            r.vals[0] = edb_val_text_copy(fullk, (uint32_t)strlen(fullk));
            r.vals[1] = edb_val_text_copy(valb, (uint32_t)strlen(valb));
            r.vals[2] = edb_val_text_copy(specs[i].desc, (uint32_t)strlen(specs[i].desc));
            result_add_row(res, r);
        }
        return EDB_OK;
    }

    case STMT_SHOW_STATS: {
        result_setup_cols(res, 2);
        res->col_names[0] = edb_strdup("metric"); res->col_types[0] = EDB_TYPE_TEXT;
        res->col_names[1] = edb_strdup("value");  res->col_types[1] = EDB_TYPE_INT;
        edb_metrics_t *m = db->metrics;
        struct { const char *k; int64_t v; } items[] = {
            {"active_connections", m ? edb_atomic_load_i64(&m->active_connections) : 0},
            {"total_connections",  m ? (int64_t)edb_atomic_load_u64(&m->total_connections) : 0},
            {"total_requests",     m ? (int64_t)edb_atomic_load_u64(&m->total_requests) : 0},
            {"total_queries",      m ? (int64_t)edb_atomic_load_u64(&m->total_queries) : 0},
            {"total_errors",       m ? (int64_t)edb_atomic_load_u64(&m->total_errors) : 0},
            {"select_count",       m ? (int64_t)edb_atomic_load_u64(&m->select_count) : 0},
            {"insert_count",       m ? (int64_t)edb_atomic_load_u64(&m->insert_count) : 0},
            {"update_count",       m ? (int64_t)edb_atomic_load_u64(&m->update_count) : 0},
            {"delete_count",       m ? (int64_t)edb_atomic_load_u64(&m->delete_count) : 0},
            {"plan_cache_hits",    m ? (int64_t)edb_atomic_load_u64(&m->plan_cache_hits) : 0},
            {"result_cache_hits",  m ? (int64_t)edb_atomic_load_u64(&m->result_cache_hits) : 0},
            {"buffer_pool_hits",   m ? (int64_t)edb_atomic_load_u64(&m->buffer_pool_hits) : 0},
            {"buffer_pool_misses", m ? (int64_t)edb_atomic_load_u64(&m->buffer_pool_misses) : 0},
            {"wal_writes",         m ? (int64_t)edb_atomic_load_u64(&m->wal_writes) : 0},
            {"checkpoints",        m ? (int64_t)edb_atomic_load_u64(&m->checkpoints) : 0},
            {"latency_p50_us",     m ? (int64_t)edb_metrics_percentile_us(m, 50.0) : 0},
            {"latency_p99_us",     m ? (int64_t)edb_metrics_percentile_us(m, 99.0) : 0}
        };
        for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
            edb_row_t r;
            edb_row_init(&r, 2);
            r.vals[0] = edb_val_text_copy(items[i].k, (uint32_t)strlen(items[i].k));
            r.vals[1] = edb_val_int(items[i].v);
            result_add_row(res, r);
        }
        return EDB_OK;
    }

    case STMT_SET_CONFIG: {
        if (!edb_config_set_kv(db->cfg, stmt->u.set_cfg.key, stmt->u.set_cfg.val, true, err, sizeof(err))) {
            result_set_err(res, EDB_ERR_INVALID, "%s", err);
            return EDB_ERR_INVALID;
        }
        if (edb_strcasecmp(stmt->u.set_cfg.key, "logging.level") == 0) {
            edb_log_set_level(db->cfg->log_level);
        }
        return EDB_OK;
    }

    case STMT_EXPLAIN: {
        result_setup_cols(res, 2);
        res->col_names[0] = edb_strdup("plan_step"); res->col_types[0] = EDB_TYPE_TEXT;
        res->col_names[1] = edb_strdup("detail");    res->col_types[1] = EDB_TYPE_TEXT;
        const edb_stmt_t *in = stmt->u.explain.inner;
        if (in && in->kind == STMT_SELECT && in->u.select.from_table) {
            edb_rwlock_rdlock(&db->catalog_lock);
            edb_table_t *t = edb_find_table(db, in->u.select.from_table);
            scan_plan_t sp;
            memset(&sp, 0, sizeof(sp));
            bool has_idx = t && db->cfg->index_scan_enabled &&
                           extract_index_plan(t, in->u.select.where_expr, NULL, 0, &sp);
            edb_row_t r1;
            edb_row_init(&r1, 2);
            const char *step = has_idx ? (sp.kind == SCAN_INDEX_EQ ? "INDEX_EQ_SCAN" : "INDEX_RANGE_SCAN")
                                       : "FULL_TABLE_SCAN";
            char detail[256];
            if (has_idx) {
                snprintf(detail, sizeof(detail), "table=%s index=%s type=%s",
                         t->name, sp.index->name, sp.index->type == EDB_IDX_BTREE ? "BTREE" : "HASH");
            } else {
                snprintf(detail, sizeof(detail), "table=%s est_rows=%llu",
                         in->u.select.from_table, (unsigned long long)(t ? t->row_count : 0));
            }
            r1.vals[0] = edb_val_text_copy(step, (uint32_t)strlen(step));
            r1.vals[1] = edb_val_text_copy(detail, (uint32_t)strlen(detail));
            result_add_row(res, r1);
            free_scan_plan(&sp);
            edb_rwlock_rdunlock(&db->catalog_lock);

            for (size_t j = 0; j < in->u.select.join_count; j++) {
                edb_row_t rj;
                edb_row_init(&rj, 2);
                const char *js = db->cfg->hash_join_enabled ? "HASH_OR_NESTED_JOIN" : "NESTED_LOOP_JOIN";
                char jd[256];
                snprintf(jd, sizeof(jd), "join_table=%s", in->u.select.joins[j].table_name);
                rj.vals[0] = edb_val_text_copy(js, (uint32_t)strlen(js));
                rj.vals[1] = edb_val_text_copy(jd, (uint32_t)strlen(jd));
                result_add_row(res, rj);
            }
            if (db->cfg->result_cache_enabled && select_is_cacheable(in)) {
                edb_row_t rc;
                edb_row_init(&rc, 2);
                rc.vals[0] = edb_val_text_copy("RESULT_CACHE", 12);
                rc.vals[1] = edb_val_text_copy("eligible=true", 13);
                result_add_row(res, rc);
            }
        } else {
            edb_row_t r;
            edb_row_init(&r, 2);
            r.vals[0] = edb_val_text_copy("DIRECT_EXEC", 11);
            r.vals[1] = edb_val_text_copy("non-select statement", 20);
            result_add_row(res, r);
        }
        return EDB_OK;
    }

    case STMT_PREPARE: {
        for (size_t i = 0; i < sess->prep_count; i++) {
            if (edb_strcasecmp(sess->preps[i].name, stmt->u.prepare.name) == 0) {
                free(sess->preps[i].sql);
                sess->preps[i].sql = edb_strndup(stmt->u.prepare.sql_start, stmt->u.prepare.sql_len);
                sess->preps[i].sql_len = stmt->u.prepare.sql_len;
                return EDB_OK;
            }
        }
        if (sess->prep_count == sess->prep_cap) {
            size_t nc = sess->prep_cap == 0 ? 8 : sess->prep_cap * 2;
            sess->preps = (edb_prep_stmt_t *)realloc(sess->preps, nc * sizeof(edb_prep_stmt_t));
            sess->prep_cap = nc;
        }
        edb_prep_stmt_t *ps = &sess->preps[sess->prep_count++];
        snprintf(ps->name, sizeof(ps->name), "%s", stmt->u.prepare.name);
        ps->sql = edb_strndup(stmt->u.prepare.sql_start, stmt->u.prepare.sql_len);
        ps->sql_len = stmt->u.prepare.sql_len;
        return EDB_OK;
    }

    case STMT_EXECUTE: {
        edb_prep_stmt_t *ps = NULL;
        for (size_t i = 0; i < sess->prep_count; i++) {
            if (edb_strcasecmp(sess->preps[i].name, stmt->u.execute.name) == 0) {
                ps = &sess->preps[i];
                break;
            }
        }
        if (!ps) {
            result_set_err(res, EDB_ERR_NOT_FOUND, "Prepared statement '%s' not found", stmt->u.execute.name);
            return EDB_ERR_NOT_FOUND;
        }
        eval_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        edb_val_t *pvals = stmt->u.execute.arg_count > 0
                           ? (edb_val_t *)calloc(stmt->u.execute.arg_count, sizeof(edb_val_t)) : NULL;
        for (size_t i = 0; i < stmt->u.execute.arg_count; i++) {
            pvals[i] = eval_expr(stmt->u.execute.args[i], &ctx);
        }
        edb_execute_sql_params(eng, sess, ps->sql, ps->sql_len, pvals, stmt->u.execute.arg_count, res);
        for (size_t i = 0; i < stmt->u.execute.arg_count; i++) edb_val_free(&pvals[i]);
        free(pvals);
        return res->status;
    }

    case STMT_DEALLOCATE: {
        for (size_t i = 0; i < sess->prep_count; i++) {
            if (edb_strcasecmp(sess->preps[i].name, stmt->u.deallocate.name) == 0) {
                free(sess->preps[i].sql);
                sess->preps[i] = sess->preps[sess->prep_count - 1];
                sess->prep_count--;
                return EDB_OK;
            }
        }
        result_set_err(res, EDB_ERR_NOT_FOUND, "Prepared statement '%s' not found", stmt->u.deallocate.name);
        return EDB_ERR_NOT_FOUND;
    }

    default:
        result_set_err(res, EDB_ERR_INTERNAL, "Unsupported statement");
        return EDB_ERR_INTERNAL;
    }
}

void edb_execute_sql_params(edb_engine_t *eng, edb_session_t *sess,
                            const char *sql, size_t sql_len,
                            const edb_val_t *params, size_t param_count,
                            edb_result_t *out) {
    uint64_t t0 = edb_now_us();
    edb_result_init(out);
    if (eng->db->metrics) edb_atomic_add_u64(&eng->db->metrics->total_queries, 1);

    edb_arena_t local_arena;
    edb_arena_init(&local_arena);
    bool cached_plan = false;
    char err[256] = {0};

    edb_stmt_t *stmt = get_or_parse_stmt(eng, sql, sql_len, &local_arena, &cached_plan, err, sizeof(err));
    if (!stmt) {
        result_set_err(out, EDB_ERR_SYNTAX, "%s", err);
    } else {
        exec_stmt(eng, sess, stmt, params, param_count, sql, sql_len, out);
    }
    if (!cached_plan) edb_arena_free(&local_arena);
    else edb_arena_free(&local_arena);

    uint64_t elapsed = edb_now_us() - t0;
    out->elapsed_us = elapsed;

    if (eng->db->metrics) {
        if (out->status != EDB_OK) edb_atomic_add_u64(&eng->db->metrics->total_errors, 1);
        if (eng->db->cfg->track_latency) edb_metrics_record_latency(eng->db->metrics, elapsed);
        if (eng->db->cfg->slow_query_ms > 0 && elapsed >= (uint64_t)eng->db->cfg->slow_query_ms * 1000ULL) {
            edb_atomic_add_u64(&eng->db->metrics->slow_queries, 1);
            if (eng->db->cfg->log_slow_queries) {
                EDB_LOG_W("Slow query (%llu us): %.*s", (unsigned long long)elapsed, (int)(sql_len > 200 ? 200 : sql_len), sql);
            }
        }
    }
    if (eng->db->cfg->log_queries) {
        EDB_LOG_I("SQL (%llu us, %s): %.*s", (unsigned long long)elapsed, edb_status_str(out->status),
                  (int)(sql_len > 200 ? 200 : sql_len), sql);
    }
}

void edb_execute_sql(edb_engine_t *eng, edb_session_t *sess,
                     const char *sql, size_t sql_len, edb_result_t *out) {
    edb_execute_sql_params(eng, sess, sql, sql_len, NULL, 0, out);
}

/* ---- Formatting ---- */
void edb_result_to_json(const edb_result_t *r, edb_buf_t *out) {
    if (r->status != EDB_OK) {
        edb_buf_printf(out, "{\"ok\":false,\"status\":\"%s\",\"error\":", edb_status_str(r->status));
        edb_json_append_escaped(out, r->err_msg, strlen(r->err_msg));
        edb_buf_printf(out, ",\"elapsed_us\":%llu}", (unsigned long long)r->elapsed_us);
        return;
    }
    edb_buf_printf(out, "{\"ok\":true,\"rows_affected\":%llu,\"last_insert_id\":%llu,\"elapsed_us\":%llu,\"cached\":%s",
                   (unsigned long long)r->rows_affected,
                   (unsigned long long)r->last_insert_id,
                   (unsigned long long)r->elapsed_us,
                   r->from_cache ? "true" : "false");
    if (r->is_select_like) {
        edb_buf_append_str(out, ",\"columns\":[");
        for (uint32_t c = 0; c < r->col_count; c++) {
            if (c > 0) edb_buf_append_char(out, ',');
            edb_json_append_escaped(out, r->col_names[c], strlen(r->col_names[c]));
        }
        edb_buf_append_str(out, "],\"rows\":[");
        for (uint32_t i = 0; i < r->row_count; i++) {
            if (i > 0) edb_buf_append_char(out, ',');
            edb_buf_append_char(out, '[');
            for (uint32_t c = 0; c < r->col_count; c++) {
                if (c > 0) edb_buf_append_char(out, ',');
                const edb_val_t *v = &r->rows[i].vals[c];
                switch (v->type) {
                case EDB_TYPE_NULL: edb_buf_append_str(out, "null"); break;
                case EDB_TYPE_INT:
                case EDB_TYPE_TIMESTAMP: edb_buf_printf(out, "%lld", (long long)v->v.i64); break;
                case EDB_TYPE_REAL: edb_buf_printf(out, "%g", v->v.f64); break;
                case EDB_TYPE_BOOL: edb_buf_append_str(out, v->v.b ? "true" : "false"); break;
                case EDB_TYPE_TEXT:
                    edb_json_append_escaped(out, v->v.str.ptr ? v->v.str.ptr : "", v->v.str.len);
                    break;
                default: edb_buf_append_str(out, "null"); break;
                }
            }
            edb_buf_append_char(out, ']');
        }
        edb_buf_append_str(out, "]");
    }
    edb_buf_append_char(out, '}');
}

void edb_result_to_text(const edb_result_t *r, edb_buf_t *out) {
    if (r->status != EDB_OK) {
        edb_buf_printf(out, "ERR [%s]: %s\n", edb_status_str(r->status), r->err_msg);
        return;
    }
    if (!r->is_select_like) {
        edb_buf_printf(out, "OK (%llu rows affected, %llu us)\n",
                       (unsigned long long)r->rows_affected,
                       (unsigned long long)r->elapsed_us);
        return;
    }
    if (r->col_count == 0) {
        edb_buf_append_str(out, "(empty)\n");
        return;
    }

    size_t *widths = (size_t *)calloc(r->col_count, sizeof(size_t));
    for (uint32_t c = 0; c < r->col_count; c++) {
        widths[c] = strlen(r->col_names[c]);
    }
    edb_buf_t cell;
    edb_buf_init(&cell);
    for (uint32_t i = 0; i < r->row_count; i++) {
        for (uint32_t c = 0; c < r->col_count; c++) {
            edb_buf_clear(&cell);
            val_to_str(&r->rows[i].vals[c], &cell);
            if (cell.len > widths[c]) widths[c] = cell.len;
        }
    }

    for (uint32_t c = 0; c < r->col_count; c++) {
        edb_buf_append_str(out, "+-");
        for (size_t k = 0; k < widths[c]; k++) edb_buf_append_char(out, '-');
        edb_buf_append_char(out, '-');
    }
    edb_buf_append_str(out, "+\n");

    for (uint32_t c = 0; c < r->col_count; c++) {
        edb_buf_printf(out, "| %-*s ", (int)widths[c], r->col_names[c]);
    }
    edb_buf_append_str(out, "|\n");

    for (uint32_t c = 0; c < r->col_count; c++) {
        edb_buf_append_str(out, "+-");
        for (size_t k = 0; k < widths[c]; k++) edb_buf_append_char(out, '-');
        edb_buf_append_char(out, '-');
    }
    edb_buf_append_str(out, "+\n");

    for (uint32_t i = 0; i < r->row_count; i++) {
        for (uint32_t c = 0; c < r->col_count; c++) {
            edb_buf_clear(&cell);
            val_to_str(&r->rows[i].vals[c], &cell);
            const char *s = cell.data ? (const char *)cell.data : "";
            edb_buf_printf(out, "| %-*s ", (int)widths[c], s);
        }
        edb_buf_append_str(out, "|\n");
    }

    for (uint32_t c = 0; c < r->col_count; c++) {
        edb_buf_append_str(out, "+-");
        for (size_t k = 0; k < widths[c]; k++) edb_buf_append_char(out, '-');
        edb_buf_append_char(out, '-');
    }
    edb_buf_printf(out, "+\n(%u rows, %llu us%s)\n", r->row_count,
                   (unsigned long long)r->elapsed_us, r->from_cache ? ", cached" : "");
    edb_buf_free(&cell);
    free(widths);
}
