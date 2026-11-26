#ifndef EDB_SQL_AST_H
#define EDB_SQL_AST_H

#include "storage.h"

typedef enum {
    EXPR_LITERAL = 1,
    EXPR_IDENT,
    EXPR_STAR,
    EXPR_PARAM,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_IS_NULL,
    EXPR_IN,
    EXPR_BETWEEN,
    EXPR_LIKE,
    EXPR_CALL,
    EXPR_CASE
} edb_expr_kind_t;

typedef enum {
    OP_ADD = 1,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_CONCAT,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_AND,
    OP_OR,
    OP_NEG,
    OP_NOT
} edb_op_t;

typedef struct edb_expr edb_expr_t;

typedef struct {
    edb_expr_t *cond;
    edb_expr_t *then_expr;
} edb_case_when_t;

struct edb_expr {
    edb_expr_kind_t kind;
    union {
        edb_val_t lit;
        struct {
            char *table; /* optional */
            char *col;
        } ident;
        struct {
            int index; /* 1-based parameter index */
        } param;
        struct {
            edb_op_t op;
            edb_expr_t *operand;
        } unary;
        struct {
            edb_op_t op;
            edb_expr_t *left;
            edb_expr_t *right;
        } binary;
        struct {
            edb_expr_t *expr;
            bool is_not;
        } is_null;
        struct {
            edb_expr_t *expr;
            edb_expr_t **items;
            size_t count;
            bool is_not;
        } in_list;
        struct {
            edb_expr_t *expr;
            edb_expr_t *lo;
            edb_expr_t *hi;
            bool is_not;
        } between;
        struct {
            edb_expr_t *expr;
            edb_expr_t *pattern;
            bool is_not;
        } like;
        struct {
            char *func_name;
            edb_expr_t **args;
            size_t arg_count;
            bool star;
            bool distinct;
        } call;
        struct {
            edb_case_when_t *whens;
            size_t when_count;
            edb_expr_t *else_expr;
        } case_expr;
    } u;
};

typedef struct {
    edb_expr_t *expr;
    char *alias;
} edb_select_item_t;

typedef enum {
    JOIN_INNER = 0,
    JOIN_LEFT = 1,
    JOIN_RIGHT = 2,
    JOIN_CROSS = 3
} edb_join_type_t;

typedef struct {
    edb_join_type_t type;
    char *table_name;
    char *alias;
    edb_expr_t *on_expr;
} edb_join_clause_t;

typedef struct {
    edb_expr_t *expr;
    bool desc;
} edb_order_item_t;

typedef struct {
    char *col;
    edb_expr_t *val;
} edb_assign_t;

typedef enum {
    STMT_SELECT = 1,
    STMT_INSERT,
    STMT_UPDATE,
    STMT_DELETE,
    STMT_CREATE_TABLE,
    STMT_DROP_TABLE,
    STMT_ALTER_ADD_COL,
    STMT_CREATE_INDEX,
    STMT_DROP_INDEX,
    STMT_BEGIN,
    STMT_COMMIT,
    STMT_ROLLBACK,
    STMT_EXPLAIN,
    STMT_SHOW_TABLES,
    STMT_DESCRIBE,
    STMT_SHOW_CONFIG,
    STMT_SHOW_STATS,
    STMT_SET_CONFIG,
    STMT_CHECKPOINT,
    STMT_VACUUM,
    STMT_PING,
    STMT_PREPARE,
    STMT_EXECUTE,
    STMT_DEALLOCATE
} edb_stmt_kind_t;

typedef struct edb_stmt {
    edb_stmt_kind_t kind;
    union {
        struct {
            bool distinct;
            edb_select_item_t *items;
            size_t item_count;
            char *from_table;
            char *from_alias;
            edb_join_clause_t *joins;
            size_t join_count;
            edb_expr_t *where_expr;
            edb_expr_t **group_by;
            size_t group_count;
            edb_expr_t *having_expr;
            edb_order_item_t *order_by;
            size_t order_count;
            int64_t limit;  /* -1 = unspecified */
            int64_t offset; /* 0 = none */
        } select;
        struct {
            char *table_name;
            char **col_names;
            size_t col_count;
            edb_expr_t ***rows;
            size_t row_count;
            size_t row_width;
        } insert;
        struct {
            char *table_name;
            edb_assign_t *assigns;
            size_t assign_count;
            edb_expr_t *where_expr;
        } update;
        struct {
            char *table_name;
            edb_expr_t *where_expr;
        } del;
        struct {
            char *table_name;
            bool if_not_exists;
            edb_col_def_t *cols;
            size_t col_count;
        } create_table;
        struct {
            char *table_name;
            bool if_exists;
        } drop_table;
        struct {
            char *table_name;
            edb_col_def_t col;
        } alter_add;
        struct {
            char *index_name;
            char *table_name;
            char *col_name;
            edb_index_type_t idx_type;
            bool unique;
            bool if_not_exists;
        } create_index;
        struct {
            char *index_name;
            bool if_exists;
        } drop_index;
        struct {
            struct edb_stmt *inner;
        } explain;
        struct {
            char *table_name;
        } describe;
        struct {
            char *like_pattern;
        } show_config;
        struct {
            char *key;
            char *val;
        } set_cfg;
        struct {
            char *name;
            const char *sql_start;
            size_t sql_len;
        } prepare;
        struct {
            char *name;
            edb_expr_t **args;
            size_t arg_count;
        } execute;
        struct {
            char *name;
        } deallocate;
    } u;
} edb_stmt_t;

bool edb_sql_parse(const char *sql, size_t sql_len, edb_arena_t *arena, edb_stmt_t **out_stmt, char *err, size_t err_cap);

#endif /* EDB_SQL_AST_H */
