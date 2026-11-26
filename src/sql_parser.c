#include "sql_ast.h"
#include <ctype.h>

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_STRING,
    TOK_INT,
    TOK_REAL,
    TOK_PARAM,
    TOK_COMMA,
    TOK_DOT,
    TOK_SEMI,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_CONCAT,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_ERR
} tok_kind_t;

typedef struct {
    tok_kind_t kind;
    const char *start;
    size_t len;
    int64_t i64;
    double f64;
    int param_idx;
    char *str_val; /* unescaped in arena for strings/idents */
} token_t;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    edb_arena_t *arena;
    token_t cur;
    int next_param;
    char err[256];
    bool has_err;
} parser_t;

static void set_err(parser_t *p, const char *fmt, ...) {
    if (p->has_err) return;
    p->has_err = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
}

static void skip_ws_comments(parser_t *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (isspace((unsigned char)c)) {
            p->pos++;
        } else if (c == '-' && p->pos + 1 < p->len && p->src[p->pos + 1] == '-') {
            p->pos += 2;
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
        } else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len && !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/')) {
                p->pos++;
            }
            if (p->pos + 1 < p->len) p->pos += 2;
        } else {
            break;
        }
    }
}

static void next_tok(parser_t *p) {
    skip_ws_comments(p);
    token_t *t = &p->cur;
    memset(t, 0, sizeof(*t));
    if (p->pos >= p->len) {
        t->kind = TOK_EOF;
        t->start = p->src + p->len;
        return;
    }

    const char *s = p->src + p->pos;
    char c = *s;
    t->start = s;

    if (c == ',' ) { p->pos++; t->kind = TOK_COMMA; t->len = 1; return; }
    if (c == '.' ) { p->pos++; t->kind = TOK_DOT; t->len = 1; return; }
    if (c == ';' ) { p->pos++; t->kind = TOK_SEMI; t->len = 1; return; }
    if (c == '(' ) { p->pos++; t->kind = TOK_LPAREN; t->len = 1; return; }
    if (c == ')' ) { p->pos++; t->kind = TOK_RPAREN; t->len = 1; return; }
    if (c == '+' ) { p->pos++; t->kind = TOK_PLUS; t->len = 1; return; }
    if (c == '-' ) { p->pos++; t->kind = TOK_MINUS; t->len = 1; return; }
    if (c == '*' ) { p->pos++; t->kind = TOK_STAR; t->len = 1; return; }
    if (c == '/' ) { p->pos++; t->kind = TOK_SLASH; t->len = 1; return; }
    if (c == '%' ) { p->pos++; t->kind = TOK_PERCENT; t->len = 1; return; }
    if (c == '=' ) { p->pos++; t->kind = TOK_EQ; t->len = 1; return; }
    if (c == '|' && p->pos + 1 < p->len && p->src[p->pos + 1] == '|') {
        p->pos += 2; t->kind = TOK_CONCAT; t->len = 2; return;
    }
    if (c == '!' && p->pos + 1 < p->len && p->src[p->pos + 1] == '=') {
        p->pos += 2; t->kind = TOK_NE; t->len = 2; return;
    }
    if (c == '<') {
        if (p->pos + 1 < p->len && p->src[p->pos + 1] == '=') { p->pos += 2; t->kind = TOK_LE; t->len = 2; return; }
        if (p->pos + 1 < p->len && p->src[p->pos + 1] == '>') { p->pos += 2; t->kind = TOK_NE; t->len = 2; return; }
        p->pos++; t->kind = TOK_LT; t->len = 1; return;
    }
    if (c == '>') {
        if (p->pos + 1 < p->len && p->src[p->pos + 1] == '=') { p->pos += 2; t->kind = TOK_GE; t->len = 2; return; }
        p->pos++; t->kind = TOK_GT; t->len = 1; return;
    }
    if (c == '?') {
        p->pos++;
        t->kind = TOK_PARAM;
        t->param_idx = p->next_param++;
        t->len = 1;
        return;
    }
    if (c == '$' && p->pos + 1 < p->len && isdigit((unsigned char)p->src[p->pos + 1])) {
        p->pos++;
        int idx = 0;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) {
            idx = idx * 10 + (p->src[p->pos] - '0');
            p->pos++;
        }
        t->kind = TOK_PARAM;
        t->param_idx = idx;
        t->len = (size_t)((p->src + p->pos) - t->start);
        return;
    }

    /* Single-quoted string */
    if (c == '\'') {
        p->pos++;
        edb_buf_t tmp;
        edb_buf_init(&tmp);
        bool closed = false;
        while (p->pos < p->len) {
            char ch = p->src[p->pos++];
            if (ch == '\'') {
                if (p->pos < p->len && p->src[p->pos] == '\'') {
                    edb_buf_append_char(&tmp, '\'');
                    p->pos++;
                } else {
                    closed = true;
                    break;
                }
            } else if (ch == '\\' && p->pos < p->len) {
                char esc = p->src[p->pos++];
                if (esc == 'n') edb_buf_append_char(&tmp, '\n');
                else if (esc == 'r') edb_buf_append_char(&tmp, '\r');
                else if (esc == 't') edb_buf_append_char(&tmp, '\t');
                else edb_buf_append_char(&tmp, esc);
            } else {
                edb_buf_append_char(&tmp, ch);
            }
        }
        if (!closed) {
            edb_buf_free(&tmp);
            set_err(p, "Unterminated string literal");
            t->kind = TOK_ERR;
            return;
        }
        t->kind = TOK_STRING;
        t->len = tmp.len;
        t->str_val = edb_arena_strndup(p->arena, tmp.data ? (const char *)tmp.data : "", tmp.len);
        edb_buf_free(&tmp);
        return;
    }

    /* Quoted identifier "..." or `...` */
    if (c == '"' || c == '`') {
        char q = c;
        p->pos++;
        size_t start = p->pos;
        while (p->pos < p->len && p->src[p->pos] != q) p->pos++;
        if (p->pos >= p->len) {
            set_err(p, "Unterminated quoted identifier");
            t->kind = TOK_ERR;
            return;
        }
        size_t ilen = p->pos - start;
        p->pos++;
        t->kind = TOK_IDENT;
        t->len = ilen;
        t->str_val = edb_arena_strndup(p->arena, p->src + start, ilen);
        return;
    }

    /* Number */
    if (isdigit((unsigned char)c)) {
        size_t start = p->pos;
        bool is_real = false;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        if (p->pos < p->len && p->src[p->pos] == '.' &&
            p->pos + 1 < p->len && isdigit((unsigned char)p->src[p->pos + 1])) {
            is_real = true;
            p->pos++;
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        }
        if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
            is_real = true;
            p->pos++;
            if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        }
        size_t nlen = p->pos - start;
        char *numstr = edb_arena_strndup(p->arena, p->src + start, nlen);
        t->len = nlen;
        t->str_val = numstr;
        if (is_real) {
            t->kind = TOK_REAL;
            t->f64 = strtod(numstr, NULL);
        } else {
            t->kind = TOK_INT;
            t->i64 = strtoll(numstr, NULL, 10);
        }
        return;
    }

    /* Identifier / keyword */
    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = p->pos;
        while (p->pos < p->len && (isalnum((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_')) {
            p->pos++;
        }
        size_t ilen = p->pos - start;
        t->kind = TOK_IDENT;
        t->len = ilen;
        t->str_val = edb_arena_strndup(p->arena, p->src + start, ilen);
        return;
    }

    set_err(p, "Unexpected character '%c'", c);
    t->kind = TOK_ERR;
}

static bool tok_is_kw(const parser_t *p, const char *kw) {
    return p->cur.kind == TOK_IDENT && edb_strcasecmp(p->cur.str_val, kw) == 0;
}

static bool eat_kw(parser_t *p, const char *kw) {
    if (tok_is_kw(p, kw)) {
        next_tok(p);
        return true;
    }
    return false;
}

static bool expect_kw(parser_t *p, const char *kw) {
    if (!eat_kw(p, kw)) {
        set_err(p, "Expected '%s'", kw);
        return false;
    }
    return true;
}

static bool eat_tok(parser_t *p, tok_kind_t k) {
    if (p->cur.kind == k) {
        next_tok(p);
        return true;
    }
    return false;
}

static bool expect_tok(parser_t *p, tok_kind_t k, const char *msg) {
    if (!eat_tok(p, k)) {
        set_err(p, "Expected %s", msg);
        return false;
    }
    return true;
}

static char *expect_ident(parser_t *p, const char *what) {
    if (p->cur.kind != TOK_IDENT) {
        set_err(p, "Expected %s", what);
        return NULL;
    }
    char *s = p->cur.str_val;
    next_tok(p);
    return s;
}

/* ---- Expression Parser ---- */
static edb_expr_t *parse_expr(parser_t *p);

static edb_expr_t *alloc_expr(parser_t *p, edb_expr_kind_t kind) {
    edb_expr_t *e = (edb_expr_t *)edb_arena_calloc(p->arena, 1, sizeof(edb_expr_t));
    if (e) e->kind = kind;
    return e;
}

static edb_expr_t *parse_primary(parser_t *p) {
    if (p->has_err) return NULL;
    if (p->cur.kind == TOK_INT) {
        edb_expr_t *e = alloc_expr(p, EXPR_LITERAL);
        e->u.lit = edb_val_int(p->cur.i64);
        next_tok(p);
        return e;
    }
    if (p->cur.kind == TOK_REAL) {
        edb_expr_t *e = alloc_expr(p, EXPR_LITERAL);
        e->u.lit = edb_val_real(p->cur.f64);
        next_tok(p);
        return e;
    }
    if (p->cur.kind == TOK_STRING) {
        edb_expr_t *e = alloc_expr(p, EXPR_LITERAL);
        e->u.lit.type = EDB_TYPE_TEXT;
        e->u.lit.v.str.ptr = p->cur.str_val;
        e->u.lit.v.str.len = (uint32_t)p->cur.len;
        next_tok(p);
        return e;
    }
    if (p->cur.kind == TOK_PARAM) {
        edb_expr_t *e = alloc_expr(p, EXPR_PARAM);
        e->u.param.index = p->cur.param_idx;
        next_tok(p);
        return e;
    }
    if (p->cur.kind == TOK_STAR) {
        next_tok(p);
        return alloc_expr(p, EXPR_STAR);
    }
    if (eat_tok(p, TOK_LPAREN)) {
        edb_expr_t *inner = parse_expr(p);
        expect_tok(p, TOK_RPAREN, "')'");
        return inner;
    }
    if (eat_kw(p, "NULL")) {
        edb_expr_t *e = alloc_expr(p, EXPR_LITERAL);
        e->u.lit = edb_val_null();
        return e;
    }
    if (eat_kw(p, "TRUE")) {
        edb_expr_t *e = alloc_expr(p, EXPR_LITERAL);
        e->u.lit = edb_val_bool(true);
        return e;
    }
    if (eat_kw(p, "FALSE")) {
        edb_expr_t *e = alloc_expr(p, EXPR_LITERAL);
        e->u.lit = edb_val_bool(false);
        return e;
    }
    if (eat_kw(p, "CASE")) {
        edb_expr_t *e = alloc_expr(p, EXPR_CASE);
        size_t cap = 4;
        e->u.case_expr.whens = (edb_case_when_t *)edb_arena_calloc(p->arena, cap, sizeof(edb_case_when_t));
        while (eat_kw(p, "WHEN")) {
            edb_expr_t *cond = parse_expr(p);
            expect_kw(p, "THEN");
            edb_expr_t *then_e = parse_expr(p);
            if (e->u.case_expr.when_count == cap) {
                size_t ncap = cap * 2;
                edb_case_when_t *nw = (edb_case_when_t *)edb_arena_calloc(p->arena, ncap, sizeof(edb_case_when_t));
                memcpy(nw, e->u.case_expr.whens, cap * sizeof(edb_case_when_t));
                e->u.case_expr.whens = nw;
                cap = ncap;
            }
            e->u.case_expr.whens[e->u.case_expr.when_count].cond = cond;
            e->u.case_expr.whens[e->u.case_expr.when_count].then_expr = then_e;
            e->u.case_expr.when_count++;
        }
        if (eat_kw(p, "ELSE")) {
            e->u.case_expr.else_expr = parse_expr(p);
        }
        expect_kw(p, "END");
        return e;
    }
    if (p->cur.kind == TOK_IDENT) {
        char *name = p->cur.str_val;
        next_tok(p);
        if (eat_tok(p, TOK_LPAREN)) {
            edb_expr_t *e = alloc_expr(p, EXPR_CALL);
            e->u.call.func_name = name;
            if (eat_tok(p, TOK_RPAREN)) return e;
            if (eat_kw(p, "DISTINCT")) e->u.call.distinct = true;
            if (p->cur.kind == TOK_STAR) {
                next_tok(p);
                e->u.call.star = true;
                expect_tok(p, TOK_RPAREN, "')'");
                return e;
            }
            size_t cap = 4;
            e->u.call.args = (edb_expr_t **)edb_arena_calloc(p->arena, cap, sizeof(edb_expr_t *));
            do {
                edb_expr_t *arg = parse_expr(p);
                if (e->u.call.arg_count == cap) {
                    size_t ncap = cap * 2;
                    edb_expr_t **na = (edb_expr_t **)edb_arena_calloc(p->arena, ncap, sizeof(edb_expr_t *));
                    memcpy(na, e->u.call.args, cap * sizeof(edb_expr_t *));
                    e->u.call.args = na;
                    cap = ncap;
                }
                e->u.call.args[e->u.call.arg_count++] = arg;
            } while (eat_tok(p, TOK_COMMA));
            expect_tok(p, TOK_RPAREN, "')'");
            return e;
        }
        if (eat_tok(p, TOK_DOT)) {
            if (eat_tok(p, TOK_STAR)) {
                edb_expr_t *e = alloc_expr(p, EXPR_STAR);
                return e;
            }
            char *col = expect_ident(p, "column name");
            edb_expr_t *e = alloc_expr(p, EXPR_IDENT);
            e->u.ident.table = name;
            e->u.ident.col = col;
            return e;
        }
        edb_expr_t *e = alloc_expr(p, EXPR_IDENT);
        e->u.ident.col = name;
        return e;
    }

    set_err(p, "Unexpected token in expression");
    return NULL;
}

static edb_expr_t *parse_unary(parser_t *p) {
    if (eat_tok(p, TOK_MINUS)) {
        edb_expr_t *e = alloc_expr(p, EXPR_UNARY);
        e->u.unary.op = OP_NEG;
        e->u.unary.operand = parse_unary(p);
        return e;
    }
    if (eat_tok(p, TOK_PLUS)) return parse_unary(p);
    if (eat_kw(p, "NOT")) {
        edb_expr_t *e = alloc_expr(p, EXPR_UNARY);
        e->u.unary.op = OP_NOT;
        e->u.unary.operand = parse_unary(p);
        return e;
    }
    return parse_primary(p);
}

static edb_expr_t *parse_mul(parser_t *p) {
    edb_expr_t *left = parse_unary(p);
    while (!p->has_err) {
        edb_op_t op = 0;
        if (eat_tok(p, TOK_STAR)) op = OP_MUL;
        else if (eat_tok(p, TOK_SLASH)) op = OP_DIV;
        else if (eat_tok(p, TOK_PERCENT)) op = OP_MOD;
        else break;
        edb_expr_t *b = alloc_expr(p, EXPR_BINARY);
        b->u.binary.op = op;
        b->u.binary.left = left;
        b->u.binary.right = parse_unary(p);
        left = b;
    }
    return left;
}

static edb_expr_t *parse_add(parser_t *p) {
    edb_expr_t *left = parse_mul(p);
    while (!p->has_err) {
        edb_op_t op = 0;
        if (eat_tok(p, TOK_PLUS)) op = OP_ADD;
        else if (eat_tok(p, TOK_MINUS)) op = OP_SUB;
        else if (eat_tok(p, TOK_CONCAT)) op = OP_CONCAT;
        else break;
        edb_expr_t *b = alloc_expr(p, EXPR_BINARY);
        b->u.binary.op = op;
        b->u.binary.left = left;
        b->u.binary.right = parse_mul(p);
        left = b;
    }
    return left;
}

static edb_expr_t *parse_cmp(parser_t *p) {
    edb_expr_t *left = parse_add(p);
    if (p->has_err) return left;

    if (eat_kw(p, "IS")) {
        bool is_not = eat_kw(p, "NOT");
        expect_kw(p, "NULL");
        edb_expr_t *e = alloc_expr(p, EXPR_IS_NULL);
        e->u.is_null.expr = left;
        e->u.is_null.is_not = is_not;
        return e;
    }

    bool negated = false;
    if (tok_is_kw(p, "NOT")) {
        /* Peek ahead by checking if followed by IN/BETWEEN/LIKE */
        size_t save_pos = p->pos;
        token_t save_cur = p->cur;
        next_tok(p);
        if (tok_is_kw(p, "IN") || tok_is_kw(p, "BETWEEN") || tok_is_kw(p, "LIKE")) {
            negated = true;
        } else {
            p->pos = save_pos;
            p->cur = save_cur;
        }
    }

    if (eat_kw(p, "IN")) {
        expect_tok(p, TOK_LPAREN, "'('");
        edb_expr_t *e = alloc_expr(p, EXPR_IN);
        e->u.in_list.expr = left;
        e->u.in_list.is_not = negated;
        size_t cap = 8;
        e->u.in_list.items = (edb_expr_t **)edb_arena_calloc(p->arena, cap, sizeof(edb_expr_t *));
        if (p->cur.kind != TOK_RPAREN) {
            do {
                edb_expr_t *it = parse_add(p);
                if (e->u.in_list.count == cap) {
                    size_t nc = cap * 2;
                    edb_expr_t **ni = (edb_expr_t **)edb_arena_calloc(p->arena, nc, sizeof(edb_expr_t *));
                    memcpy(ni, e->u.in_list.items, cap * sizeof(edb_expr_t *));
                    e->u.in_list.items = ni;
                    cap = nc;
                }
                e->u.in_list.items[e->u.in_list.count++] = it;
            } while (eat_tok(p, TOK_COMMA));
        }
        expect_tok(p, TOK_RPAREN, "')'");
        return e;
    }

    if (eat_kw(p, "BETWEEN")) {
        edb_expr_t *lo = parse_add(p);
        expect_kw(p, "AND");
        edb_expr_t *hi = parse_add(p);
        edb_expr_t *e = alloc_expr(p, EXPR_BETWEEN);
        e->u.between.expr = left;
        e->u.between.lo = lo;
        e->u.between.hi = hi;
        e->u.between.is_not = negated;
        return e;
    }

    if (eat_kw(p, "LIKE")) {
        edb_expr_t *pat = parse_add(p);
        edb_expr_t *e = alloc_expr(p, EXPR_LIKE);
        e->u.like.expr = left;
        e->u.like.pattern = pat;
        e->u.like.is_not = negated;
        return e;
    }

    edb_op_t op = 0;
    if (eat_tok(p, TOK_EQ)) op = OP_EQ;
    else if (eat_tok(p, TOK_NE)) op = OP_NE;
    else if (eat_tok(p, TOK_LT)) op = OP_LT;
    else if (eat_tok(p, TOK_LE)) op = OP_LE;
    else if (eat_tok(p, TOK_GT)) op = OP_GT;
    else if (eat_tok(p, TOK_GE)) op = OP_GE;

    if (op != 0) {
        edb_expr_t *b = alloc_expr(p, EXPR_BINARY);
        b->u.binary.op = op;
        b->u.binary.left = left;
        b->u.binary.right = parse_add(p);
        return b;
    }
    return left;
}

static edb_expr_t *parse_and(parser_t *p) {
    edb_expr_t *left = parse_cmp(p);
    while (!p->has_err && eat_kw(p, "AND")) {
        edb_expr_t *b = alloc_expr(p, EXPR_BINARY);
        b->u.binary.op = OP_AND;
        b->u.binary.left = left;
        b->u.binary.right = parse_cmp(p);
        left = b;
    }
    return left;
}

static edb_expr_t *parse_expr(parser_t *p) {
    edb_expr_t *left = parse_and(p);
    while (!p->has_err && eat_kw(p, "OR")) {
        edb_expr_t *b = alloc_expr(p, EXPR_BINARY);
        b->u.binary.op = OP_OR;
        b->u.binary.left = left;
        b->u.binary.right = parse_and(p);
        left = b;
    }
    return left;
}

/* ---- Statement Parsers ---- */
static edb_stmt_t *alloc_stmt(parser_t *p, edb_stmt_kind_t k) {
    edb_stmt_t *s = (edb_stmt_t *)edb_arena_calloc(p->arena, 1, sizeof(edb_stmt_t));
    if (s) s->kind = k;
    return s;
}

static bool parse_col_def(parser_t *p, edb_col_def_t *col) {
    memset(col, 0, sizeof(*col));
    char *cname = expect_ident(p, "column name");
    if (!cname) return false;
    snprintf(col->name, sizeof(col->name), "%s", cname);

    char *tname = expect_ident(p, "column type");
    if (!tname) return false;
    if (!edb_type_from_str(tname, &col->type)) {
        set_err(p, "Unknown column type '%s'", tname);
        return false;
    }
    /* Optional type length e.g. VARCHAR(255) or DECIMAL(10,2) */
    if (eat_tok(p, TOK_LPAREN)) {
        while (p->cur.kind != TOK_EOF && p->cur.kind != TOK_RPAREN) next_tok(p);
        expect_tok(p, TOK_RPAREN, "')'");
    }

    while (!p->has_err && p->cur.kind == TOK_IDENT) {
        if (eat_kw(p, "PRIMARY")) {
            expect_kw(p, "KEY");
            col->primary_key = true;
            col->not_null = true;
            col->unique = true;
        } else if (eat_kw(p, "AUTO_INCREMENT") || eat_kw(p, "AUTOINCREMENT")) {
            col->auto_increment = true;
        } else if (eat_kw(p, "NOT")) {
            expect_kw(p, "NULL");
            col->not_null = true;
        } else if (eat_kw(p, "UNIQUE")) {
            col->unique = true;
        } else if (eat_kw(p, "DEFAULT")) {
            col->has_default = true;
            if (p->cur.kind == TOK_INT) { col->default_val = edb_val_int(p->cur.i64); next_tok(p); }
            else if (p->cur.kind == TOK_REAL) { col->default_val = edb_val_real(p->cur.f64); next_tok(p); }
            else if (p->cur.kind == TOK_STRING) {
                col->default_val = edb_val_text_copy(p->cur.str_val, (uint32_t)p->cur.len);
                next_tok(p);
            } else if (eat_kw(p, "TRUE")) col->default_val = edb_val_bool(true);
            else if (eat_kw(p, "FALSE")) col->default_val = edb_val_bool(false);
            else if (eat_kw(p, "NULL")) col->default_val = edb_val_null();
            else { set_err(p, "Invalid DEFAULT value"); return false; }
        } else {
            break;
        }
    }
    return !p->has_err;
}

static bool is_select_terminator(const parser_t *p) {
    if (p->cur.kind == TOK_EOF || p->cur.kind == TOK_SEMI || p->cur.kind == TOK_RPAREN) return true;
    return tok_is_kw(p, "FROM") || tok_is_kw(p, "WHERE") || tok_is_kw(p, "GROUP") ||
           tok_is_kw(p, "HAVING") || tok_is_kw(p, "ORDER") || tok_is_kw(p, "LIMIT") ||
           tok_is_kw(p, "OFFSET") || tok_is_kw(p, "INNER") || tok_is_kw(p, "LEFT") ||
           tok_is_kw(p, "RIGHT") || tok_is_kw(p, "CROSS") || tok_is_kw(p, "JOIN") ||
           tok_is_kw(p, "ON");
}

static edb_stmt_t *parse_select(parser_t *p) {
    edb_stmt_t *s = alloc_stmt(p, STMT_SELECT);
    s->u.select.limit = -1;
    if (eat_kw(p, "DISTINCT")) s->u.select.distinct = true;

    size_t cap = 8;
    s->u.select.items = (edb_select_item_t *)edb_arena_calloc(p->arena, cap, sizeof(edb_select_item_t));
    do {
        edb_expr_t *ex = parse_expr(p);
        char *alias = NULL;
        if (eat_kw(p, "AS")) {
            alias = expect_ident(p, "column alias");
        } else if (p->cur.kind == TOK_IDENT && !is_select_terminator(p)) {
            alias = expect_ident(p, "column alias");
        }
        if (s->u.select.item_count == cap) {
            size_t nc = cap * 2;
            edb_select_item_t *ni = (edb_select_item_t *)edb_arena_calloc(p->arena, nc, sizeof(edb_select_item_t));
            memcpy(ni, s->u.select.items, cap * sizeof(edb_select_item_t));
            s->u.select.items = ni;
            cap = nc;
        }
        s->u.select.items[s->u.select.item_count].expr = ex;
        s->u.select.items[s->u.select.item_count].alias = alias;
        s->u.select.item_count++;
    } while (!p->has_err && eat_tok(p, TOK_COMMA));

    if (eat_kw(p, "FROM")) {
        s->u.select.from_table = expect_ident(p, "table name");
        if (eat_kw(p, "AS")) {
            s->u.select.from_alias = expect_ident(p, "table alias");
        } else if (p->cur.kind == TOK_IDENT && !is_select_terminator(p)) {
            s->u.select.from_alias = expect_ident(p, "table alias");
        }

        size_t jcap = 4;
        s->u.select.joins = (edb_join_clause_t *)edb_arena_calloc(p->arena, jcap, sizeof(edb_join_clause_t));
        while (!p->has_err) {
            edb_join_type_t jtype = JOIN_INNER;
            bool is_join = false;
            if (eat_kw(p, "INNER")) { expect_kw(p, "JOIN"); jtype = JOIN_INNER; is_join = true; }
            else if (eat_kw(p, "LEFT")) { eat_kw(p, "OUTER"); expect_kw(p, "JOIN"); jtype = JOIN_LEFT; is_join = true; }
            else if (eat_kw(p, "RIGHT")) { eat_kw(p, "OUTER"); expect_kw(p, "JOIN"); jtype = JOIN_RIGHT; is_join = true; }
            else if (eat_kw(p, "CROSS")) { expect_kw(p, "JOIN"); jtype = JOIN_CROSS; is_join = true; }
            else if (eat_kw(p, "JOIN")) { jtype = JOIN_INNER; is_join = true; }
            if (!is_join) break;

            char *jt = expect_ident(p, "join table");
            char *ja = NULL;
            if (eat_kw(p, "AS")) ja = expect_ident(p, "join alias");
            else if (p->cur.kind == TOK_IDENT && !is_select_terminator(p)) ja = expect_ident(p, "join alias");
            edb_expr_t *on_ex = NULL;
            if (jtype != JOIN_CROSS && eat_kw(p, "ON")) {
                on_ex = parse_expr(p);
            }
            if (s->u.select.join_count == jcap) {
                size_t nc = jcap * 2;
                edb_join_clause_t *nj = (edb_join_clause_t *)edb_arena_calloc(p->arena, nc, sizeof(edb_join_clause_t));
                memcpy(nj, s->u.select.joins, jcap * sizeof(edb_join_clause_t));
                s->u.select.joins = nj;
                jcap = nc;
            }
            s->u.select.joins[s->u.select.join_count].type = jtype;
            s->u.select.joins[s->u.select.join_count].table_name = jt;
            s->u.select.joins[s->u.select.join_count].alias = ja;
            s->u.select.joins[s->u.select.join_count].on_expr = on_ex;
            s->u.select.join_count++;
        }
    }

    if (eat_kw(p, "WHERE")) {
        s->u.select.where_expr = parse_expr(p);
    }
    if (eat_kw(p, "GROUP")) {
        expect_kw(p, "BY");
        size_t gcap = 4;
        s->u.select.group_by = (edb_expr_t **)edb_arena_calloc(p->arena, gcap, sizeof(edb_expr_t *));
        do {
            edb_expr_t *ge = parse_expr(p);
            if (s->u.select.group_count == gcap) {
                size_t nc = gcap * 2;
                edb_expr_t **ng = (edb_expr_t **)edb_arena_calloc(p->arena, nc, sizeof(edb_expr_t *));
                memcpy(ng, s->u.select.group_by, gcap * sizeof(edb_expr_t *));
                s->u.select.group_by = ng;
                gcap = nc;
            }
            s->u.select.group_by[s->u.select.group_count++] = ge;
        } while (!p->has_err && eat_tok(p, TOK_COMMA));
        if (eat_kw(p, "HAVING")) {
            s->u.select.having_expr = parse_expr(p);
        }
    }
    if (eat_kw(p, "ORDER")) {
        expect_kw(p, "BY");
        size_t ocap = 4;
        s->u.select.order_by = (edb_order_item_t *)edb_arena_calloc(p->arena, ocap, sizeof(edb_order_item_t));
        do {
            edb_expr_t *oe = parse_expr(p);
            bool desc = false;
            if (eat_kw(p, "DESC")) desc = true;
            else eat_kw(p, "ASC");
            if (s->u.select.order_count == ocap) {
                size_t nc = ocap * 2;
                edb_order_item_t *no = (edb_order_item_t *)edb_arena_calloc(p->arena, nc, sizeof(edb_order_item_t));
                memcpy(no, s->u.select.order_by, ocap * sizeof(edb_order_item_t));
                s->u.select.order_by = no;
                ocap = nc;
            }
            s->u.select.order_by[s->u.select.order_count].expr = oe;
            s->u.select.order_by[s->u.select.order_count].desc = desc;
            s->u.select.order_count++;
        } while (!p->has_err && eat_tok(p, TOK_COMMA));
    }
    if (eat_kw(p, "LIMIT")) {
        if (p->cur.kind != TOK_INT) { set_err(p, "Expected integer after LIMIT"); return NULL; }
        s->u.select.limit = p->cur.i64;
        next_tok(p);
        if (eat_kw(p, "OFFSET")) {
            if (p->cur.kind != TOK_INT) { set_err(p, "Expected integer after OFFSET"); return NULL; }
            s->u.select.offset = p->cur.i64;
            next_tok(p);
        }
    }
    return s;
}

static edb_stmt_t *parse_stmt_internal(parser_t *p);

static edb_stmt_t *parse_stmt_internal(parser_t *p) {
    if (eat_kw(p, "SELECT")) return parse_select(p);

    if (eat_kw(p, "INSERT")) {
        expect_kw(p, "INTO");
        edb_stmt_t *s = alloc_stmt(p, STMT_INSERT);
        s->u.insert.table_name = expect_ident(p, "table name");
        if (eat_tok(p, TOK_LPAREN)) {
            size_t ccap = 8;
            s->u.insert.col_names = (char **)edb_arena_calloc(p->arena, ccap, sizeof(char *));
            do {
                char *cn = expect_ident(p, "column name");
                if (s->u.insert.col_count == ccap) {
                    size_t nc = ccap * 2;
                    char **ncn = (char **)edb_arena_calloc(p->arena, nc, sizeof(char *));
                    memcpy(ncn, s->u.insert.col_names, ccap * sizeof(char *));
                    s->u.insert.col_names = ncn;
                    ccap = nc;
                }
                s->u.insert.col_names[s->u.insert.col_count++] = cn;
            } while (!p->has_err && eat_tok(p, TOK_COMMA));
            expect_tok(p, TOK_RPAREN, "')'");
        }
        expect_kw(p, "VALUES");
        size_t rcap = 8;
        s->u.insert.rows = (edb_expr_t ***)edb_arena_calloc(p->arena, rcap, sizeof(edb_expr_t **));
        do {
            expect_tok(p, TOK_LPAREN, "'('");
            size_t vcap = 16, vcnt = 0;
            edb_expr_t **vals = (edb_expr_t **)edb_arena_calloc(p->arena, vcap, sizeof(edb_expr_t *));
            do {
                edb_expr_t *vx = parse_expr(p);
                if (vcnt == vcap) {
                    size_t nc = vcap * 2;
                    edb_expr_t **nv = (edb_expr_t **)edb_arena_calloc(p->arena, nc, sizeof(edb_expr_t *));
                    memcpy(nv, vals, vcap * sizeof(edb_expr_t *));
                    vals = nv;
                    vcap = nc;
                }
                vals[vcnt++] = vx;
            } while (!p->has_err && eat_tok(p, TOK_COMMA));
            expect_tok(p, TOK_RPAREN, "')'");
            if (s->u.insert.row_count == 0) s->u.insert.row_width = vcnt;
            else if (vcnt != s->u.insert.row_width) {
                set_err(p, "Inconsistent VALUES width");
                return NULL;
            }
            if (s->u.insert.row_count == rcap) {
                size_t nc = rcap * 2;
                edb_expr_t ***nr = (edb_expr_t ***)edb_arena_calloc(p->arena, nc, sizeof(edb_expr_t **));
                memcpy(nr, s->u.insert.rows, rcap * sizeof(edb_expr_t **));
                s->u.insert.rows = nr;
                rcap = nc;
            }
            s->u.insert.rows[s->u.insert.row_count++] = vals;
        } while (!p->has_err && eat_tok(p, TOK_COMMA));
        return s;
    }

    if (eat_kw(p, "UPDATE")) {
        edb_stmt_t *s = alloc_stmt(p, STMT_UPDATE);
        s->u.update.table_name = expect_ident(p, "table name");
        expect_kw(p, "SET");
        size_t cap = 8;
        s->u.update.assigns = (edb_assign_t *)edb_arena_calloc(p->arena, cap, sizeof(edb_assign_t));
        do {
            char *col = expect_ident(p, "column name");
            expect_tok(p, TOK_EQ, "'='");
            edb_expr_t *val = parse_expr(p);
            if (s->u.update.assign_count == cap) {
                size_t nc = cap * 2;
                edb_assign_t *na = (edb_assign_t *)edb_arena_calloc(p->arena, nc, sizeof(edb_assign_t));
                memcpy(na, s->u.update.assigns, cap * sizeof(edb_assign_t));
                s->u.update.assigns = na;
                cap = nc;
            }
            s->u.update.assigns[s->u.update.assign_count].col = col;
            s->u.update.assigns[s->u.update.assign_count].val = val;
            s->u.update.assign_count++;
        } while (!p->has_err && eat_tok(p, TOK_COMMA));
        if (eat_kw(p, "WHERE")) s->u.update.where_expr = parse_expr(p);
        return s;
    }

    if (eat_kw(p, "DELETE")) {
        expect_kw(p, "FROM");
        edb_stmt_t *s = alloc_stmt(p, STMT_DELETE);
        s->u.del.table_name = expect_ident(p, "table name");
        if (eat_kw(p, "WHERE")) s->u.del.where_expr = parse_expr(p);
        return s;
    }

    if (eat_kw(p, "CREATE")) {
        bool unique = eat_kw(p, "UNIQUE");
        if (!unique && eat_kw(p, "TABLE")) {
            edb_stmt_t *s = alloc_stmt(p, STMT_CREATE_TABLE);
            if (eat_kw(p, "IF")) { expect_kw(p, "NOT"); expect_kw(p, "EXISTS"); s->u.create_table.if_not_exists = true; }
            s->u.create_table.table_name = expect_ident(p, "table name");
            expect_tok(p, TOK_LPAREN, "'('");
            size_t cap = 16;
            s->u.create_table.cols = (edb_col_def_t *)edb_arena_calloc(p->arena, cap, sizeof(edb_col_def_t));
            do {
                edb_col_def_t cdef;
                if (!parse_col_def(p, &cdef)) break;
                if (s->u.create_table.col_count == cap) {
                    size_t nc = cap * 2;
                    edb_col_def_t *ncd = (edb_col_def_t *)edb_arena_calloc(p->arena, nc, sizeof(edb_col_def_t));
                    memcpy(ncd, s->u.create_table.cols, cap * sizeof(edb_col_def_t));
                    s->u.create_table.cols = ncd;
                    cap = nc;
                }
                s->u.create_table.cols[s->u.create_table.col_count++] = cdef;
            } while (!p->has_err && eat_tok(p, TOK_COMMA));
            expect_tok(p, TOK_RPAREN, "')'");
            return s;
        }
        if (eat_kw(p, "INDEX")) {
            edb_stmt_t *s = alloc_stmt(p, STMT_CREATE_INDEX);
            s->u.create_index.unique = unique;
            s->u.create_index.idx_type = EDB_IDX_BTREE;
            if (eat_kw(p, "IF")) { expect_kw(p, "NOT"); expect_kw(p, "EXISTS"); s->u.create_index.if_not_exists = true; }
            s->u.create_index.index_name = expect_ident(p, "index name");
            expect_kw(p, "ON");
            s->u.create_index.table_name = expect_ident(p, "table name");
            expect_tok(p, TOK_LPAREN, "'('");
            s->u.create_index.col_name = expect_ident(p, "column name");
            expect_tok(p, TOK_RPAREN, "')'");
            if (eat_kw(p, "USING")) {
                char *ut = expect_ident(p, "index type");
                if (ut && edb_strcasecmp(ut, "HASH") == 0) s->u.create_index.idx_type = EDB_IDX_HASH;
                else if (ut && edb_strcasecmp(ut, "BTREE") == 0) s->u.create_index.idx_type = EDB_IDX_BTREE;
                else set_err(p, "Unknown index type '%s'", ut ? ut : "");
            }
            return s;
        }
        set_err(p, "Expected TABLE or INDEX after CREATE");
        return NULL;
    }

    if (eat_kw(p, "DROP")) {
        if (eat_kw(p, "TABLE")) {
            edb_stmt_t *s = alloc_stmt(p, STMT_DROP_TABLE);
            if (eat_kw(p, "IF")) { expect_kw(p, "EXISTS"); s->u.drop_table.if_exists = true; }
            s->u.drop_table.table_name = expect_ident(p, "table name");
            return s;
        }
        if (eat_kw(p, "INDEX")) {
            edb_stmt_t *s = alloc_stmt(p, STMT_DROP_INDEX);
            if (eat_kw(p, "IF")) { expect_kw(p, "EXISTS"); s->u.drop_index.if_exists = true; }
            s->u.drop_index.index_name = expect_ident(p, "index name");
            return s;
        }
        set_err(p, "Expected TABLE or INDEX after DROP");
        return NULL;
    }

    if (eat_kw(p, "ALTER")) {
        expect_kw(p, "TABLE");
        edb_stmt_t *s = alloc_stmt(p, STMT_ALTER_ADD_COL);
        s->u.alter_add.table_name = expect_ident(p, "table name");
        expect_kw(p, "ADD");
        eat_kw(p, "COLUMN");
        parse_col_def(p, &s->u.alter_add.col);
        return s;
    }

    if (eat_kw(p, "BEGIN")) { eat_kw(p, "TRANSACTION"); return alloc_stmt(p, STMT_BEGIN); }
    if (eat_kw(p, "COMMIT")) { return alloc_stmt(p, STMT_COMMIT); }
    if (eat_kw(p, "ROLLBACK")) { return alloc_stmt(p, STMT_ROLLBACK); }
    if (eat_kw(p, "CHECKPOINT")) { return alloc_stmt(p, STMT_CHECKPOINT); }
    if (eat_kw(p, "VACUUM")) { return alloc_stmt(p, STMT_VACUUM); }
    if (eat_kw(p, "PING")) { return alloc_stmt(p, STMT_PING); }

    if (eat_kw(p, "EXPLAIN")) {
        edb_stmt_t *s = alloc_stmt(p, STMT_EXPLAIN);
        s->u.explain.inner = parse_stmt_internal(p);
        return s;
    }

    if (eat_kw(p, "DESCRIBE") || eat_kw(p, "DESC")) {
        edb_stmt_t *s = alloc_stmt(p, STMT_DESCRIBE);
        s->u.describe.table_name = expect_ident(p, "table name");
        return s;
    }

    if (eat_kw(p, "SHOW")) {
        if (eat_kw(p, "TABLES")) return alloc_stmt(p, STMT_SHOW_TABLES);
        if (eat_kw(p, "STATS") || eat_kw(p, "STATUS")) return alloc_stmt(p, STMT_SHOW_STATS);
        if (eat_kw(p, "CONFIG") || eat_kw(p, "VARIABLES")) {
            edb_stmt_t *s = alloc_stmt(p, STMT_SHOW_CONFIG);
            if (eat_kw(p, "LIKE")) {
                if (p->cur.kind == TOK_STRING) {
                    s->u.show_config.like_pattern = p->cur.str_val;
                    next_tok(p);
                } else {
                    set_err(p, "Expected pattern string after LIKE");
                }
            }
            return s;
        }
        set_err(p, "Expected TABLES, CONFIG, or STATS after SHOW");
        return NULL;
    }

    if (eat_kw(p, "SET")) {
        edb_stmt_t *s = alloc_stmt(p, STMT_SET_CONFIG);
        char *part1 = expect_ident(p, "config key");
        if (eat_tok(p, TOK_DOT)) {
            char *part2 = expect_ident(p, "config key");
            if (part1 && part2) {
                size_t l = strlen(part1) + 1 + strlen(part2) + 1;
                s->u.set_cfg.key = (char *)edb_arena_alloc(p->arena, l);
                snprintf(s->u.set_cfg.key, l, "%s.%s", part1, part2);
            }
        } else {
            s->u.set_cfg.key = part1;
        }
        expect_tok(p, TOK_EQ, "'='");
        if (p->cur.kind == TOK_STRING || p->cur.kind == TOK_IDENT ||
            p->cur.kind == TOK_INT || p->cur.kind == TOK_REAL) {
            s->u.set_cfg.val = p->cur.str_val;
            next_tok(p);
        } else {
            set_err(p, "Expected value after '='");
        }
        return s;
    }

    if (eat_kw(p, "PREPARE")) {
        edb_stmt_t *s = alloc_stmt(p, STMT_PREPARE);
        s->u.prepare.name = expect_ident(p, "statement name");
        expect_kw(p, "AS");
        skip_ws_comments(p);
        if (p->cur.start) {
            /* Capture remaining SQL from start of current token */
            size_t off = (size_t)(p->cur.start - p->src);
            s->u.prepare.sql_start = p->src + off;
            s->u.prepare.sql_len = p->len - off;
            p->pos = p->len;
            p->cur.kind = TOK_EOF;
        }
        return s;
    }

    if (eat_kw(p, "EXECUTE")) {
        edb_stmt_t *s = alloc_stmt(p, STMT_EXECUTE);
        s->u.execute.name = expect_ident(p, "statement name");
        if (eat_tok(p, TOK_LPAREN)) {
            size_t cap = 8;
            s->u.execute.args = (edb_expr_t **)edb_arena_calloc(p->arena, cap, sizeof(edb_expr_t *));
            if (p->cur.kind != TOK_RPAREN) {
                do {
                    edb_expr_t *arg = parse_expr(p);
                    if (s->u.execute.arg_count == cap) {
                        size_t nc = cap * 2;
                        edb_expr_t **na = (edb_expr_t **)edb_arena_calloc(p->arena, nc, sizeof(edb_expr_t *));
                        memcpy(na, s->u.execute.args, cap * sizeof(edb_expr_t *));
                        s->u.execute.args = na;
                        cap = nc;
                    }
                    s->u.execute.args[s->u.execute.arg_count++] = arg;
                } while (!p->has_err && eat_tok(p, TOK_COMMA));
            }
            expect_tok(p, TOK_RPAREN, "')'");
        }
        return s;
    }

    if (eat_kw(p, "DEALLOCATE")) {
        edb_stmt_t *s = alloc_stmt(p, STMT_DEALLOCATE);
        s->u.deallocate.name = expect_ident(p, "statement name");
        return s;
    }

    set_err(p, "Unknown SQL statement");
    return NULL;
}

bool edb_sql_parse(const char *sql, size_t sql_len, edb_arena_t *arena, edb_stmt_t **out_stmt, char *err, size_t err_cap) {
    parser_t p;
    memset(&p, 0, sizeof(p));
    p.src = sql;
    p.len = sql_len;
    p.pos = 0;
    p.arena = arena;
    p.next_param = 1;

    next_tok(&p);
    if (p.cur.kind == TOK_EOF) {
        if (err) snprintf(err, err_cap, "Empty SQL statement");
        return false;
    }

    edb_stmt_t *stmt = parse_stmt_internal(&p);
    if (!p.has_err) {
        while (eat_tok(&p, TOK_SEMI)) {}
        if (p.cur.kind != TOK_EOF) {
            set_err(&p, "Unexpected trailing tokens");
        }
    }

    if (p.has_err || !stmt) {
        if (err) snprintf(err, err_cap, "%s", p.err[0] ? p.err : "SQL syntax error");
        return false;
    }

    *out_stmt = stmt;
    return true;
}
