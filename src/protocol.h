#ifndef EDB_PROTOCOL_H
#define EDB_PROTOCOL_H

#include "sql_exec.h"

#define EDB_BIN_REQ_MAGIC  0x45 /* 'E' */
#define EDB_BIN_RESP_MAGIC 0x52 /* 'R' */
#define EDB_BIN_HDR_SIZE   8

typedef enum {
    EDB_OP_PING    = 0x01,
    EDB_OP_QUERY   = 0x02,
    EDB_OP_BATCH   = 0x03,
    EDB_OP_AUTH    = 0x04,
    EDB_OP_PREPARE = 0x05,
    EDB_OP_EXEC    = 0x06
} edb_bin_op_t;

typedef enum {
    PROTO_UNKNOWN = 0,
    PROTO_BINARY  = 1,
    PROTO_HTTP    = 2,
    PROTO_TEXT    = 3
} edb_proto_t;

typedef struct edb_conn {
    edb_socket_t sock;
    uint64_t id;
    edb_proto_t proto;
    bool authenticated;
    int auth_failures;
    uint64_t last_active_ms;

    edb_buf_t rbuf;
    edb_buf_t wbuf;
    size_t wpos;

    edb_session_t sess;

    /* Concurrency & lifetime */
    edb_atomic_u32 refcount;
    edb_atomic_u32 worker_busy;
    edb_atomic_u32 send_active;
    edb_atomic_u32 closing;
    void *reactor_ctx; /* OS-specific reactor/IOCP state */
} edb_conn_t;

/* Returns how many bytes of conn->rbuf were consumed, or -1 if protocol error / close requested */
int edb_protocol_process(edb_engine_t *eng, edb_conn_t *conn, bool *out_close);

/* Client-side binary codec helpers (reused by CLI and bench) */
void edb_bin_encode_frame(edb_buf_t *out, edb_bin_op_t op, uint16_t flags, const void *payload, uint32_t len);
bool edb_bin_decode_response(const uint8_t *payload, size_t len, uint8_t status, uint16_t flags, edb_result_t *out);

#endif /* EDB_PROTOCOL_H */
