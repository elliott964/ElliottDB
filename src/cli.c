#include "protocol.h"
#include "pal.h"
#include <ctype.h>

static uint16_t cli_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t cli_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool send_and_recv_bin(edb_socket_t s, edb_bin_op_t op, const void *payload, uint32_t len, edb_result_t *res) {
    edb_buf_t frame;
    edb_buf_init(&frame);
    edb_bin_encode_frame(&frame, op, 0, payload, len);
    int sr = edb_sock_send_all(s, frame.data, frame.len);
    edb_buf_free(&frame);
    if (sr != 0) return false;

    uint8_t hdr[EDB_BIN_HDR_SIZE];
    if (edb_sock_recv_exact(s, hdr, EDB_BIN_HDR_SIZE) != 0) return false;
    if (hdr[0] != EDB_BIN_RESP_MAGIC) return false;
    uint8_t status = hdr[1];
    uint16_t flags = cli_u16(hdr + 2);
    uint32_t rlen = cli_u32(hdr + 4);
    uint8_t *rbuf = rlen > 0 ? (uint8_t *)malloc(rlen) : NULL;
    if (rlen > 0 && edb_sock_recv_exact(s, rbuf, rlen) != 0) {
        free(rbuf);
        return false;
    }
    if (op == EDB_OP_PING || (rlen == 4 && memcmp(rbuf, "PONG", 4) == 0)) {
        edb_result_init(res);
        res->status = (edb_status_t)status;
        free(rbuf);
        return true;
    }
    bool ok = edb_bin_decode_response(rbuf ? rbuf : (const uint8_t *)"", rlen, status, flags, res);
    free(rbuf);
    return ok;
}

static void print_result_csv(const edb_result_t *r) {
    if (r->status != EDB_OK) {
        fprintf(stderr, "ERROR [%s]: %s\n", edb_status_str(r->status), r->err_msg);
        return;
    }
    if (!r->is_select_like) {
        printf("ok,%llu\n", (unsigned long long)r->rows_affected);
        return;
    }
    for (uint32_t c = 0; c < r->col_count; c++) {
        printf("%s%s", c > 0 ? "," : "", r->col_names[c]);
    }
    printf("\n");
    for (uint32_t i = 0; i < r->row_count; i++) {
        for (uint32_t c = 0; c < r->col_count; c++) {
            const edb_val_t *v = &r->rows[i].vals[c];
            if (c > 0) printf(",");
            switch (v->type) {
            case EDB_TYPE_NULL: break;
            case EDB_TYPE_INT:
            case EDB_TYPE_TIMESTAMP: printf("%lld", (long long)v->v.i64); break;
            case EDB_TYPE_REAL: printf("%g", v->v.f64); break;
            case EDB_TYPE_BOOL: printf("%s", v->v.b ? "true" : "false"); break;
            case EDB_TYPE_TEXT:
                printf("\"%.*s\"", (int)v->v.str.len, v->v.str.ptr ? v->v.str.ptr : "");
                break;
            default: break;
            }
        }
        printf("\n");
    }
}

static void print_formatted(const edb_result_t *r, const char *fmt) {
    if (edb_strcasecmp(fmt, "json") == 0) {
        edb_buf_t b;
        edb_buf_init(&b);
        edb_result_to_json(r, &b);
        printf("%s\n", b.data ? (const char *)b.data : "{}");
        edb_buf_free(&b);
    } else if (edb_strcasecmp(fmt, "csv") == 0) {
        print_result_csv(r);
    } else {
        edb_buf_t b;
        edb_buf_init(&b);
        edb_result_to_text(r, &b);
        printf("%s", b.data ? (const char *)b.data : "");
        edb_buf_free(&b);
    }
}

int edb_cli_main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    uint16_t port = 7432;
    const char *user = NULL;
    const char *pass = "";
    const char *cmd = NULL;
    const char *fmt = "table";

    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) host = argv[++i];
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if ((strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--user") == 0) && i + 1 < argc) user = argv[++i];
        else if ((strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--password") == 0) && i + 1 < argc) pass = argv[++i];
        else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--command") == 0) && i + 1 < argc) cmd = argv[++i];
        else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--format") == 0) && i + 1 < argc) fmt = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: elliottdb cli [-H host] [-p port] [-u user] [-P pass] [-c \"SQL\"] [-f table|json|csv]\n");
            return 0;
        }
    }

    edb_net_init();
    edb_socket_t s = edb_sock_connect_tcp(host, port, 5000);
    if (s == EDB_INVALID_SOCK) {
        fprintf(stderr, "Failed to connect to ElliottDB at %s:%u\n", host, (unsigned)port);
        edb_net_cleanup();
        return 1;
    }

    if (user) {
        edb_buf_t auth;
        edb_buf_init(&auth);
        edb_buf_append_u16(&auth, (uint16_t)strlen(user));
        edb_buf_append(&auth, user, strlen(user));
        edb_buf_append_u16(&auth, (uint16_t)strlen(pass));
        edb_buf_append(&auth, pass, strlen(pass));
        edb_result_t ar;
        if (!send_and_recv_bin(s, EDB_OP_AUTH, auth.data, (uint32_t)auth.len, &ar) || ar.status != EDB_OK) {
            fprintf(stderr, "Authentication failed: %s\n", ar.err_msg);
            edb_result_free(&ar);
            edb_buf_free(&auth);
            edb_sock_close(s);
            edb_net_cleanup();
            return 1;
        }
        edb_result_free(&ar);
        edb_buf_free(&auth);
    }

    if (cmd) {
        edb_result_t r;
        edb_bin_op_t op = (strchr(cmd, ';') != NULL) ? EDB_OP_BATCH : EDB_OP_QUERY;
        if (!send_and_recv_bin(s, op, cmd, (uint32_t)strlen(cmd), &r)) {
            fprintf(stderr, "Connection closed or protocol error\n");
            edb_sock_close(s);
            edb_net_cleanup();
            return 1;
        }
        print_formatted(&r, fmt);
        int rc = (r.status == EDB_OK) ? 0 : 1;
        edb_result_free(&r);
        edb_sock_close(s);
        edb_net_cleanup();
        return rc;
    }

    printf("Connected to ElliottDB v%s at %s:%u\n", ELLIOTTDB_VERSION, host, (unsigned)port);
    printf("Type SQL statements or \\q to quit.\n");

    char line[8192];
    while (true) {
        printf("elliottdb> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        edb_str_trim(line);
        if (line[0] == '\0') continue;
        if (strcmp(line, "\\q") == 0 || edb_strcasecmp(line, "exit") == 0 || edb_strcasecmp(line, "quit") == 0) {
            break;
        }
        if (strcmp(line, "\\h") == 0 || edb_strcasecmp(line, "help") == 0) {
            printf("Commands: SELECT, INSERT, UPDATE, DELETE, CREATE TABLE/INDEX, DROP, SHOW TABLES/CONFIG/STATS, DESCRIBE, EXPLAIN, BEGIN/COMMIT/ROLLBACK, CHECKPOINT, \\q\n");
            continue;
        }
        edb_result_t r;
        edb_bin_op_t op = (strchr(line, ';') != NULL) ? EDB_OP_BATCH : EDB_OP_QUERY;
        if (!send_and_recv_bin(s, op, line, (uint32_t)strlen(line), &r)) {
            fprintf(stderr, "Disconnected from server\n");
            break;
        }
        print_formatted(&r, fmt);
        edb_result_free(&r);
    }

    edb_sock_close(s);
    edb_net_cleanup();
    return 0;
}
