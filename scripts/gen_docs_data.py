#!/usr/bin/env python3
"""
Parses g_specs[] from src/config.c and builds machine- & agent-readable assets:
  - llms.txt & docs/llms.txt (complete Markdown reference)
  - docs/llms-lite.txt (mirror of root llms-lite.txt)
  - docs/api/config-schema.json
  - docs/config-data.js
"""
import json
import os
import re
import shutil

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CONFIG_C = os.path.join(ROOT, "src", "config.c")
DOCS_DIR = os.path.join(ROOT, "docs")
API_DIR = os.path.join(DOCS_DIR, "api")

LINE_RE = re.compile(
    r'^\s*\{"(?P<section>[^"]+)",\s*"(?P<key>[^"]+)",\s*'
    r'(?P<type>EDB_CFG_(?:INT|BOOL|STR)),\s*'
    r'(?:CFG_OFF\([^)]+\),\s*0,\s*(?P<min>-?\d+),\s*(?P<max>-?\d+)|CFG_STR\([^)]+\),\s*0,\s*0),\s*'
    r'(?P<mutable>true|false),\s*'
    r'"(?P<default>(?:[^"\\]|\\.)*)",\s*'
    r'"(?P<desc>(?:[^"\\]|\\.)*)"\},?'
)


def parse_config_specs():
    specs = []
    with open(CONFIG_C, "r", encoding="utf-8") as f:
        in_specs = False
        for line in f:
            if "g_specs[]" in line:
                in_specs = True
                continue
            if in_specs and line.strip().startswith("};"):
                break
            if not in_specs:
                continue
            m = LINE_RE.match(line)
            if m:
                sec = m.group("section")
                key = m.group("key")
                t_raw = m.group("type")
                typ = "int" if t_raw == "EDB_CFG_INT" else ("bool" if t_raw == "EDB_CFG_BOOL" else "string")
                default_raw = m.group("default")
                if typ == "int":
                    default_val = int(default_raw)
                elif typ == "bool":
                    default_val = default_raw == "true"
                else:
                    default_val = default_raw
                specs.append({
                    "section": sec,
                    "key": key,
                    "full_key": f"{sec}.{key}",
                    "type": typ,
                    "default": default_val,
                    "default_str": default_raw,
                    "min": int(m.group("min")) if typ == "int" else None,
                    "max": int(m.group("max")) if typ == "int" else None,
                    "runtime_mutable": m.group("mutable") == "true",
                    "env_var": f"ELLIOTTDB_{sec.upper()}_{key.upper()}",
                    "cli_flag": f"--{sec}.{key}={default_raw}",
                    "description": m.group("desc"),
                })
    return specs


def build_llms_full_markdown(specs):
    sections = {}
    for s in specs:
        sections.setdefault(s["section"], []).append(s)

    lines = [
        "# ElliottDB — Complete Technical Reference (`llms.txt`, v1.0.0)",
        "",
        "> Single-binary, zero-dependency, high-concurrency relational database written in portable **C11** for **Windows** (`elliottdb.exe`) and **Linux** (`elliottdb`).",
        "> Compact version for minimal token budgets: [`llms-lite.txt`](llms-lite.txt) · Live Docs: https://elliott964.github.io/ElliottDB/ · Repo: https://github.com/elliott964/ElliottDB",
        "",
        "---",
        "",
        "## 1. Project Identity & Core Constraints",
        "",
        "- **Release Targets**:",
        "  - Windows x64: `build\\elliottdb.exe` (`382,464` bytes, statically linked `/MT`, OS dependencies `WS2_32.dll` + `KERNEL32.dll` only).",
        "  - Linux x64: `build/elliottdb` (GCC/Clang `-std=c11 -O3`, optional `-static`, `pthread` + `lm` only).",
        "- **Zero-Config UX**: Running `./elliottdb` or `elliottdb.exe` with no arguments binds `0.0.0.0:7432`, creates `./elliottdb_data`, auto-tunes worker/I/O threads and buffer pool (25% RAM, clamped 32 MB–2048 MB), and multiplexes **Binary (`EDB1`)**, **HTTP/1.1 JSON**, and **Text Line** protocols on the same TCP port.",
        "- **Compiler Discipline**: Zero warnings under MSVC `/W4` and GCC/Clang `-Wall -Wextra`.",
        "",
        "### Quick Start & Subcommands",
        "```bash",
        "./elliottdb                              # Start server (zero-config defaults)",
        "elliottdb cli -c \"SHOW TABLES\" -f json   # One-shot SQL over binary EDB1",
        "elliottdb bench --workload ping -c 64 -t 8 -n 200000 --pipeline 32",
        "elliottdb init-config elliottdb.conf     # Write full 92-option INI template",
        "```",
        "",
        "### Verified Windows Benchmarks (MSVC `/O2 /MT`, 8 Threads)",
        "- Embedded `point` (`200,000` reqs): `493,907 req/s` (`p50=6us`, `p95=10us`, `p99=76us`, `err=0`)",
        "- TCP IOCP `ping` (`200,000` reqs, 64 clients, pipeline 32): `466,944 req/s` (`p50=11us`, `p95=26us`, `p99=59us`, `err=0`)",
        "- TCP IOCP `read` (`100,000` reqs, 64 clients, pipeline 16): `171,326 req/s` (`p50=33us`, `p95=91us`, `p99=193us`, `err=0`)",
        "- TCP IOCP `mixed` (`50,000` reqs, 64 clients, pipeline 16): `112,176 req/s` (`p50=54us`, `p95=140us`, `p99=225us`, `err=0`)",
        "",
        "---",
        "",
        "## 2. Repository & Codebase Map",
        "",
        "| File | Responsibility & Key Symbols |",
        "| :--- | :--- |",
        "| `src/edb_common.h`, `src/edb_util.c` | `edb_status_t` (0..12), `edb_buf_t` dynamic byte buffer, `edb_arena_t` (64 KB chunk bump allocator), `edb_crc32c()`, `edb_hash64()`, `edb_hash_str_ci()`, `edb_like_match()`, `edb_json_append_escaped()`, thread-safe text/JSON logger (`edb_log_*`). |",
        "| `src/pal.h` | OS-agnostic atomics (`edb_atomic_i64/u64/u32`), `edb_mutex_t`, `edb_rwlock_t`, `edb_cond_t`, `edb_thread_t`, `edb_file_t`, `edb_socket_t`, `EDB_INVALID_SOCK`, time/CPU/RAM helpers. |",
        "| `src/pal_win.c` | Windows PAL: `Interlocked*`, `CRITICAL_SECTION`, `SRWLOCK`, `CONDITION_VARIABLE`, `_beginthreadex`, `QueryPerformanceCounter`, `CreateFileA` with `io_mu`-protected `SetFilePointerEx`+`ReadFile`/`WriteFile`, `SetConsoleCtrlHandler`, Winsock2. |",
        "| `src/pal_linux.c` | Linux PAL: `__atomic_*`, `pthread_*`, `clock_gettime`, `pread`/`pwrite`/`fdatasync`, `sigaction` (`SIGINT`/`SIGTERM`, `SIG_IGN` on `SIGPIPE`), POSIX sockets (`TCP_KEEPIDLE`, `SO_REUSEPORT`). |",
        f"| `src/config.h`, `src/config.c` | `edb_config_t` & `g_specs[]` (**{len(specs)}** parameters across **{len(sections)}** sections): defaults, INI parser (`elliottdb.conf`), env vars (`ELLIOTTDB_<SECTION>_<KEY>`), CLI args (`--section.key=val`), `edb_config_finalize()`, `edb_config_set_kv()`, `edb_config_write_template()`, `edb_config_dump_json()`. |",
        "| `src/metrics.h`, `src/metrics.c` | Lock-free atomic counters (`active_connections`, `total_requests`, `select_count`, cache hits, WAL/buffer stats) + 16-bucket logarithmic latency histogram (`edb_metrics_percentile_us()`). |",
        "| `src/pager.h`, `src/pager.c` | 64-shard clock-sweep Buffer Pool (`edb_pager_t`), Page 0 `edb_file_header_t`, slotted page header `edb_page_hdr_t`, `edb_rid_t = (page_id << 16) | slot_idx`, pure `in_memory` mode. |",
        "| `src/wal.h`, `src/wal.c` | Append-only WAL (`elliottdb.wal`), `EDB_WAL_MAGIC = 0x45444257`, CRC32C record framing, `off | normal | full` sync modes, Group Commit coalescing (`group_commit_ms`, `group_commit_max_ops`), `edb_wal_replay()`. |",
        "| `src/index.h`, `src/index.c` | `edb_val_t` & `edb_val_cmp()`/`edb_val_hash()`, B+Tree (`EDB_IDX_BTREE` with doubly-linked leaves and `(key, rid)` tie-breaker for non-unique keys), open-addressed Hash index (`EDB_IDX_HASH`). |",
        "| `src/storage.h`, `src/storage.c` | `edb_db_t`, `edb_table_t` (per-table `edb_rwlock_t`, atomic `version`), `edb_txn_t` (undo log `UNDO_INSERT`/`DELETE`/`UPDATE`), catalog page serialization, WAL replay filtering uncommitted txns, `edb_db_checkpoint()`. |",
        "| `src/sql_ast.h`, `src/sql_parser.c` | Hand-written lexer & recursive-descent parser allocating AST nodes from `edb_arena_t`. |",
        "| `src/sql_exec.h`, `src/sql_exec.c` | `edb_engine_t` (64-shard plan cache + 64-shard atomic table-version-invalidated result cache), `edb_session_t`, index scan extractor, Hash Join + Nested Loop Join, `GROUP BY`/aggregates/`HAVING`, `ORDER BY`, `EXPLAIN`, `PREPARE`/`EXECUTE`, JSON/ASCII table formatters. |",
        "| `src/protocol.h`, `src/protocol.c` | Multiplexed protocol auto-detection (`PROTO_BINARY`, `PROTO_HTTP`, `PROTO_TEXT`), `EDB1` encoder/decoder (`edb_bin_encode_frame`, `edb_bin_decode_response`), HTTP/1.1 Keep-Alive router, Basic Auth. |",
        "| `src/reactor.h`, `src/reactor_win.c`, `src/reactor_linux.c` | Slab-pooled `edb_conn_t` lifecycle, atomic `refcount`, Windows IOCP (`WSARecv`/`WSASend` + `PostQueuedCompletionStatus`), Linux `epoll` (`EPOLLET` + `SO_REUSEPORT` + `eventfd`), RX/TX handoff. |",
        "| `src/server.h`, `src/server.c` | `edb_server_t`, sharded work-stealing queues (`edb_work_queue_t`), worker threads, maintenance thread (bg flush, WAL checkpoint, idle reaper, metrics log), graceful shutdown. |",
        "| `src/cli.c`, `src/bench.c`, `src/main.c` | Binary `EDB1` interactive/one-shot CLI, multi-threaded TCP & `--embedded` benchmark runner, CLI subcommand dispatcher & startup banner. |",
        "| `tests/test_suite.py` | End-to-end Python test suite: SQL/protocols, crash recovery (`proc.kill()`), config/auth, 200-client stress test, embedded & TCP IOCP benchmarks. |",
        "| `scripts/gen_docs_data.py` | Parses `g_specs[]` from `src/config.c` to regenerate `llms.txt`, `docs/llms.txt`, `docs/llms-lite.txt`, `docs/api/config-schema.json`, and `docs/config-data.js`. |",
        "",
        "---",
        "",
        "## 3. Critical Portability & Concurrency Invariants",
        "",
        "### Portability Rules (MUST follow on every edit)",
        "1. **Windows header order**: NEVER include `<windows.h>` before `<winsock2.h>` and `<ws2tcpip.h>`. Always include `pal.h` or define `WIN32_LEAN_AND_MEAN` and include `<winsock2.h>` first.",
        "2. **No `<stdatomic.h>`**: MSVC C11 support for `<stdatomic.h>` varies by SDK; always use `edb_atomic_i64`, `edb_atomic_u64`, `edb_atomic_u32` from `src/pal.h`.",
        "3. **No GCC extensions**: Avoid VLAs, `typeof`, statement expressions, and zero-length `[0]` arrays (use `[1]` or pointer allocations).",
        "4. **Linux feature macro**: Keep `#define _GNU_SOURCE` in `pal_linux.c`, `reactor_linux.c`, `CMakeLists.txt`, and `Makefile` (needed for `accept4`, `qsort_r`, `sigaction`, `pthread_rwlock_t`).",
        "5. **Portable `qsort`**: `sql_exec.c` uses `qsort_s(arr, n, sz, cmp, ctx)` on Windows (`cmp(ctx, a, b)`) and `qsort_r(arr, n, sz, cmp, ctx)` on Linux (`cmp(a, b, ctx)`).",
        "",
        "### Concurrency & Memory-Safety Invariants",
        "1. **Connection Reference Counting**: Every `edb_conn_t` starts with `refcount = 1` (owned by the open socket). Each in-flight `WSARecv`, `WSASend`, `IOCP_OP_KICK_SEND`, epoll event dispatch, or worker queue item MUST retain via `edb_reactor_conn_retain()` before queuing and release via `edb_reactor_conn_release()`. `free_conn_final()` runs strictly when `refcount == 0` and invokes `on_close` (`edb_session_destroy`) before returning the slot to `free_list`.",
        "2. **Double-Buffered IOCP Send (`reactor_win.c`)**: Workers append to `ctx->tx_buf` under `ctx->io_mu`. `try_start_send_locked()` swaps `ctx->tx_buf` into `ctx->tx_active` ONLY when `ctx->send_pending == false`. **Never** `realloc` `ctx->tx_active` while `WSASend` is in flight.",
        "3. **Lock Ordering (`io_mu` vs `pool_mu` vs `close_conn_socket`)**:",
        "   - In `alloc_conn()`: pop from `free_list` under `pool_mu`, unlock `pool_mu`, initialize ALL fields (including `last_active_ms = edb_now_ms()`), then re-acquire `pool_mu` to insert into `active_head`. Otherwise `edb_reactor_reap_idle()` can observe `last_active_ms == 0`.",
        "   - In `reactor_win.c` (`try_start_send_locked`) and `reactor_linux.c` (`flush_tx_locked`): never call `close_conn_socket()` while holding `ctx->io_mu`; return `bool need_close` and call `close_conn_socket()` after unlocking `ctx->io_mu`.",
        "4. **Per-Connection FIFO Ordering & Work Stealing (`server.c`)**:",
        "   - `conn->worker_busy` (CAS `0 -> 1`) guarantees at most one worker executes requests for a given connection at a time, preserving strict pipelined response order.",
        "   - `srv->queue_shards` is clamped to `<= srv->worker_count`, and shard selection uses `% srv->queue_shards`.",
        "   - `work_queue_pop()` uses a single `edb_cond_timedwait_ms(..., 5)` (`if`, NOT `while`) so idle workers return promptly and steal from busy shards.",
        "5. **GROUP BY & ORDER BY Aliases (`sql_exec.c`)**:",
        "   - `group_t` stores `const edb_row_t *const **tuples` (full joined tuple pointers across all `FROM`/`JOIN` sources), and `eval_aggregate()` updates all `ctx->sources[s].row` entries per tuple.",
        "   - `ORDER BY` resolves bare `EXPR_IDENT` against `projs[p].name` (SELECT column aliases) before falling back to table column lookup.",
        "6. **Cache Invalidation (`sql_exec.c`)**:",
        "   - Result cache entries store `schema_version` + `(table_id, table->version)` snapshots + `created_ms`. Bypassed when `sess->txn != NULL`, `param_count > 0`, or `NOW()` is present.",
        "",
        "---",
        "",
        "## 4. Wire Protocols (Multiplexed on Port `7432`)",
        "",
        "First-byte detection in `edb_protocol_process()` (`src/protocol.c`):",
        "- `0x45` (`'E'`): **Binary `EDB1`** (if `network.enable_binary_protocol = true`)",
        "- `GET `, `POST`, `HEAD`, `PUT `, `DELE`, `OPTI`: **HTTP/1.1 JSON** (if `network.enable_http = true`)",
        "- Anything else: **Text Line Protocol** (if `network.enable_text_protocol = true`)",
        "",
        "### Binary Protocol (`EDB1`) — Little-Endian",
        "- **Request Frame (8-byte header + payload)**:",
        "  - `[0]` `0x45` (`'E'`), `[1]` `op`, `[2..3]` `flags` (`u16`), `[4..7]` `len` (`u32`), `[8..8+len)` `payload`.",
        "  - Opcodes: `0x01 PING`, `0x02 QUERY` (UTF-8 SQL), `0x03 BATCH` (`;`-delimited SQL), `0x04 AUTH` (`[u16 ulen][user][u16 plen][pass]`), `0x05 PREPARE`, `0x06 EXEC`.",
        "- **Response Frame (8-byte header + payload)**:",
        "  - `[0]` `0x52` (`'R'`), `[1]` `status` (`edb_status_t`), `[2..3]` `flags` (`bit 0 = is_select_like`, `bit 1 = from_cache`), `[4..7]` `len` (`u32`).",
        "  - `PING` body: `\"PONG\"` (4 bytes).",
        "  - Query body: `[u64 rows_affected][u64 last_insert_id][u64 elapsed_us]`, then:",
        "    - If `status != 0`: `[u16 err_len][err_msg]`.",
        "    - Else if `is_select_like`: `[u32 col_count][u32 row_count]`, per-col `[u16 name_len][name][u8 type]`, per-cell `[u8 val_type][value]`.",
        "  - Value types: `0=NULL`, `1=INT(i64)`, `2=REAL(f64)`, `3=TEXT(u32 len + bytes)`, `4=BOOL(u8)`, `5=BLOB(u32 len + bytes)`, `6=TIMESTAMP(i64)`.",
        "  - Status codes: `0=OK`, `1=SYNTAX`, `2=NOT_FOUND`, `3=EXISTS`, `4=CONSTRAINT`, `5=IO`, `6=OOM`, `7=AUTH`, `8=OVERLOAD`, `9=INVALID`, `10=TXN`, `11=READONLY`, `12=INTERNAL`.",
        "",
        "### HTTP/1.1 JSON Endpoints",
        "- `GET /health` -> `{\"status\":\"ok\",\"version\":\"1.0.0\",\"node\":\"...\",\"uptime_sec\":N}` (exempt from auth)",
        "- `GET /metrics` -> JSON counters + `latency_p50_us`, `latency_p95_us`, `latency_p99_us`",
        "- `GET /config` -> JSON map of all 92 `section.key` entries (`admin_password` masked)",
        "- `POST /query` -> Raw SQL body or `{\"sql\":\"...\"}` -> `{\"ok\":true,\"rows_affected\":...,\"columns\":[...],\"rows\":[[...]]}`",
        "- `POST /batch` -> `;`-delimited SQL -> JSON array `[...]`",
        "- Auth: `Authorization: Basic base64(user:password)` when `security.auth_enabled = true`.",
        "",
        "---",
        "",
        "## 5. SQL Dialect Reference",
        "",
        "- **Types**: `INT`, `REAL`, `TEXT`, `BOOL`, `BLOB`, `TIMESTAMP`.",
        "- **DDL**: `CREATE TABLE [IF NOT EXISTS] t (col TYPE [PRIMARY KEY] [AUTO_INCREMENT] [NOT NULL] [UNIQUE] [DEFAULT val], ...)`, `DROP TABLE [IF EXISTS] t`, `ALTER TABLE t ADD [COLUMN] coldef`, `CREATE [UNIQUE] INDEX [IF NOT EXISTS] idx ON t (col) [USING BTREE|HASH]`, `DROP INDEX [IF EXISTS] idx`.",
        "- **DML**: `INSERT INTO t [(cols)] VALUES (...), (...)`, `UPDATE t SET c=expr [WHERE ...]`, `DELETE FROM t [WHERE ...]`.",
        "- **Queries**: `SELECT [DISTINCT] items FROM t [AS a] [INNER|LEFT|RIGHT|CROSS JOIN t2 [AS b] ON expr] [WHERE expr] [GROUP BY exprs] [HAVING expr] [ORDER BY expr [ASC|DESC]] [LIMIT n [OFFSET m]]`.",
        "- **Expressions & Functions**: arithmetic (`+ - * / %`), concat (`||`), comparisons (`= != <> < <= > >=`), `IS [NOT] NULL`, `[NOT] IN (...)`, `[NOT] BETWEEN a AND b`, `[NOT] LIKE pat`, `AND`/`OR`/`NOT`, `CASE WHEN ... THEN ... ELSE ... END`, `COUNT(*|col|DISTINCT col)`, `SUM`, `AVG`, `MIN`, `MAX`, `LOWER`, `UPPER`, `LENGTH`, `COALESCE`, `ABS`, `NOW`, `ROUND`, `SUBSTR`.",
        "- **Transactions & Prepared**: `BEGIN [TRANSACTION]`, `COMMIT`, `ROLLBACK`, `PREPARE name AS sql`, `EXECUTE name(args...)`, `DEALLOCATE name`.",
        "- **Introspection & Admin**: `EXPLAIN <stmt>`, `SHOW TABLES`, `DESCRIBE <table>`, `SHOW CONFIG [LIKE 'pat']`, `SHOW STATS`, `SET section.key = value`, `CHECKPOINT`, `VACUUM`, `PING`.",
        "",
        "---",
        "",
        f"## 6. Configuration System ({len(specs)} Parameters, {len(sections)} Sections)",
        "",
        "Precedence (lowest to highest):",
        "1. Built-in auto-tuned defaults (`edb_config_defaults`)",
        "2. INI file (`-c path` or `./elliottdb.conf` in CWD)",
        "3. Env vars `ELLIOTTDB_<SECTION>_<KEY>` (uppercased, e.g. `ELLIOTTDB_SERVER_PORT=8000`)",
        "4. CLI flags (`--<section>.<key>=<value>`, or shortcuts `-H`, `-p`, `-d`, `-t`, `-c`, `--memory`, `--no-banner`)",
        "5. Runtime SQL `SET section.key = value` (for mutable keys)",
        "",
        "Machine-readable schemas: [`docs/api/config-schema.json`](docs/api/config-schema.json) · [`docs/api/openapi.json`](docs/api/openapi.json).",
    ]

    for sec, items in sections.items():
        lines.append("")
        lines.append(f"### [{sec}] ({len(items)} options)")
        for it in items:
            rng = f" [range {it['min']}..{it['max']}]" if it["type"] == "int" else ""
            mut = "runtime-mutable" if it["runtime_mutable"] else "restart-required"
            lines.append(
                f"- `{it['full_key']}` (`{it['type']}`, default=`{it['default_str']}`{rng}, {mut}, env=`{it['env_var']}`): {it['description']}"
            )

    lines.extend([
        "",
        "---",
        "",
        "## 7. Build, Test & Maintenance Commands",
        "",
        "```bat",
        ":: Windows static single binary (MSVC /O2 /MT /W4)",
        "build.bat",
        ":: Verify zero non-OS DLL dependencies",
        "dumpbin /dependents build\\elliottdb.exe",
        ":: Run full end-to-end test & benchmark suite",
        "python tests\\test_suite.py",
        "```",
        "",
        "```bash",
        "# Linux single binary",
        "make            # or: make static / ./build.sh --static",
        "# CMake (Windows or Linux)",
        "cmake -B build_cmake -G Ninja && cmake --build build_cmake",
        "# Regenerate llms.txt, config-schema.json, and config-data.js from src/config.c",
        "python scripts/gen_docs_data.py",
        "```",
        "",
    ])
    return "\n".join(lines)


def main():
    os.makedirs(DOCS_DIR, exist_ok=True)
    os.makedirs(API_DIR, exist_ok=True)

    specs = parse_config_specs()
    if len(specs) < 90:
        raise RuntimeError(f"Expected >= 90 config specs, parsed only {len(specs)}")

    schema_obj = {
        "version": "1.0.0",
        "count": len(specs),
        "sections": list(dict.fromkeys(s["section"] for s in specs)),
        "options": specs,
    }

    schema_path = os.path.join(API_DIR, "config-schema.json")
    with open(schema_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(schema_obj, f, indent=2)
        f.write("\n")

    js_path = os.path.join(DOCS_DIR, "config-data.js")
    with open(js_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Auto-generated from src/config.c by scripts/gen_docs_data.py\n")
        f.write("window.EDB_CONFIG_SCHEMA = ")
        json.dump(schema_obj, f, indent=2)
        f.write(";\n")

    full_md = build_llms_full_markdown(specs)
    for out_path in (os.path.join(ROOT, "llms.txt"), os.path.join(DOCS_DIR, "llms.txt")):
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(full_md)

    shutil.copyfile(os.path.join(ROOT, "llms-lite.txt"), os.path.join(DOCS_DIR, "llms-lite.txt"))

    print(f"Generated llms.txt, llms-lite.txt, and schemas successfully: {len(specs)} config options across {len(schema_obj['sections'])} sections.")


if __name__ == "__main__":
    main()
