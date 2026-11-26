# AGENTS.md — ElliottDB Engineering & Integration Handbook

> Authoritative technical reference for AI coding agents working on or integrating with **ElliottDB** (`v1.0.0`).
> For a token-minimal version suitable for system prompts, see [`AGENTS-LITE.md`](AGENTS-LITE.md).

---

## 1. Project Identity & Core Constraints

- **Project**: ElliottDB — single-binary, zero-dependency, high-concurrency relational database in portable **C11**.
- **Release Targets**:
  - Windows x64: `build\elliottdb.exe` (statically linked `/MT`, OS dependencies `WS2_32.dll` + `KERNEL32.dll` only).
  - Linux x64: `build/elliottdb` (GCC/Clang `-std=c11 -O3`, optional `-static`, `pthread` + `lm` only).
- **Zero-Config UX**: Running `./elliottdb` or `elliottdb.exe` with no arguments binds `0.0.0.0:7432`, creates `./elliottdb_data`, auto-tunes worker/I/O threads and buffer pool (25% RAM, clamped 32 MB–2048 MB), and multiplexes **Binary (`EDB1`)**, **HTTP/1.1 JSON**, and **Text Line** protocols on the same TCP port.
- **Compiler Discipline**: Zero warnings under MSVC `/W4` and GCC/Clang `-Wall -Wextra`.

---

## 2. Repository & Codebase Map

| File | Responsibility & Key Symbols |
| :--- | :--- |
| `src/edb_common.h`, `src/edb_util.c` | `edb_status_t` (0..12), `edb_buf_t` dynamic byte buffer, `edb_arena_t` (64 KB chunk bump allocator), `edb_crc32c()`, `edb_hash64()`, `edb_hash_str_ci()`, `edb_like_match()`, `edb_json_append_escaped()`, thread-safe text/JSON logger (`edb_log_*`). |
| `src/pal.h` | OS-agnostic atomics (`edb_atomic_i64/u64/u32`), `edb_mutex_t`, `edb_rwlock_t`, `edb_cond_t`, `edb_thread_t`, `edb_file_t`, `edb_socket_t`, `EDB_INVALID_SOCK`, time/CPU/RAM helpers. |
| `src/pal_win.c` | Windows PAL: `Interlocked*`, `CRITICAL_SECTION`, `SRWLOCK`, `CONDITION_VARIABLE`, `_beginthreadex`, `QueryPerformanceCounter`, `CreateFileA` with `io_mu`-protected `SetFilePointerEx`+`ReadFile`/`WriteFile`, `SetConsoleCtrlHandler`, Winsock2. |
| `src/pal_linux.c` | Linux PAL: `__atomic_*`, `pthread_*`, `clock_gettime`, `pread`/`pwrite`/`fdatasync`, `sigaction` (`SIGINT`/`SIGTERM`, `SIG_IGN` on `SIGPIPE`), POSIX sockets (`TCP_KEEPIDLE`, `SO_REUSEPORT`). |
| `src/config.h`, `src/config.c` | `edb_config_t` & `g_specs[]` (**92** parameters across 11 sections): defaults, INI parser (`elliottdb.conf`), env vars (`ELLIOTTDB_<SECTION>_<KEY>`), CLI args (`--section.key=val`), `edb_config_finalize()`, `edb_config_set_kv()`, `edb_config_write_template()`, `edb_config_dump_json()`. |
| `src/metrics.h`, `src/metrics.c` | Lock-free atomic counters (`active_connections`, `total_requests`, `select_count`, cache hits, WAL/buffer stats) + 16-bucket logarithmic latency histogram (`edb_metrics_percentile_us()`). |
| `src/pager.h`, `src/pager.c` | 64-shard clock-sweep Buffer Pool (`edb_pager_t`), Page 0 `edb_file_header_t`, slotted page header `edb_page_hdr_t`, `edb_rid_t = (page_id << 16) | slot_idx`, pure `in_memory` mode. |
| `src/wal.h`, `src/wal.c` | Append-only WAL (`elliottdb.wal`), `EDB_WAL_MAGIC = 0x45444257`, CRC32C record framing, `off \| normal \| full` sync modes, Group Commit coalescing (`group_commit_ms`, `group_commit_max_ops`), `edb_wal_replay()`. |
| `src/index.h`, `src/index.c` | `edb_val_t` & `edb_val_cmp()`/`edb_val_hash()`, B+Tree (`EDB_IDX_BTREE` with doubly-linked leaves and `(key, rid)` tie-breaker for non-unique keys), open-addressed Hash index (`EDB_IDX_HASH`). |
| `src/storage.h`, `src/storage.c` | `edb_db_t`, `edb_table_t` (per-table `edb_rwlock_t`, atomic `version`), `edb_txn_t` (undo log `UNDO_INSERT`/`DELETE`/`UPDATE`), catalog page serialization, WAL replay filtering uncommitted txns, `edb_db_checkpoint()`. |
| `src/sql_ast.h`, `src/sql_parser.c` | Hand-written lexer & recursive-descent parser allocating AST nodes from `edb_arena_t`. |
| `src/sql_exec.h`, `src/sql_exec.c` | `edb_engine_t` (64-shard plan cache + 64-shard atomic table-version-invalidated result cache), `edb_session_t`, index scan extractor, Hash Join + Nested Loop Join, `GROUP BY`/aggregates/`HAVING`, `ORDER BY`, `EXPLAIN`, `PREPARE`/`EXECUTE`, JSON/ASCII table formatters. |
| `src/protocol.h`, `src/protocol.c` | Multiplexed protocol auto-detection (`PROTO_BINARY`, `PROTO_HTTP`, `PROTO_TEXT`), `EDB1` encoder/decoder (`edb_bin_encode_frame`, `edb_bin_decode_response`), HTTP/1.1 Keep-Alive router, Basic Auth. |
| `src/reactor.h`, `src/reactor_win.c`, `src/reactor_linux.c` | Slab-pooled `edb_conn_t` lifecycle, atomic `refcount`, Windows IOCP (`WSARecv`/`WSASend` + `PostQueuedCompletionStatus`), Linux `epoll` (`EPOLLET` + `SO_REUSEPORT` + `eventfd`), RX/TX handoff. |
| `src/server.h`, `src/server.c` | `edb_server_t`, sharded work-stealing queues (`edb_work_queue_t`), worker threads, maintenance thread (bg flush, WAL checkpoint, idle reaper, metrics log), graceful shutdown. |
| `src/cli.c`, `src/bench.c`, `src/main.c` | Binary `EDB1` interactive/one-shot CLI, multi-threaded TCP & `--embedded` benchmark runner, CLI subcommand dispatcher & startup banner. |
| `tests/test_suite.py` | End-to-end Python test suite: SQL/protocols, crash recovery (`proc.kill()`), config/auth, 200-client stress test, embedded & TCP IOCP benchmarks. |
| `scripts/gen_docs_data.py` | Parses `g_specs[]` from `src/config.c` to regenerate `docs/api/config-schema.json`, `docs/config-data.js`, `docs/llms-full.txt`, and mirrors `AGENTS*.md` into `docs/`. |

---

## 3. Critical Portability & Concurrency Invariants

### Portability Rules (MUST follow on every edit)
1. **Windows header order**: NEVER include `<windows.h>` before `<winsock2.h>` and `<ws2tcpip.h>`. Always include `pal.h` or define `WIN32_LEAN_AND_MEAN` and include `<winsock2.h>` first.
2. **No `<stdatomic.h>`**: MSVC C11 support for `<stdatomic.h>` varies by SDK; always use `edb_atomic_i64`, `edb_atomic_u64`, `edb_atomic_u32` from `src/pal.h`.
3. **No GCC extensions**: Avoid VLAs, `typeof`, statement expressions, and zero-length `[0]` arrays (use `[1]` or pointer allocations).
4. **Linux feature macro**: Keep `#define _GNU_SOURCE` in `pal_linux.c`, `reactor_linux.c`, `CMakeLists.txt`, and `Makefile` (needed for `accept4`, `qsort_r`, `sigaction`, `pthread_rwlock_t`).
5. **Portable `qsort`**: `sql_exec.c` uses `qsort_s(arr, n, sz, cmp, ctx)` on Windows (`cmp(ctx, a, b)`) and `qsort_r(arr, n, sz, cmp, ctx)` on Linux (`cmp(a, b, ctx)`).

### Concurrency & Memory-Safety Invariants
1. **Connection Reference Counting**: Every `edb_conn_t` starts with `refcount = 1` (owned by the open socket). Each in-flight `WSARecv`, `WSASend`, `IOCP_OP_KICK_SEND`, epoll event dispatch, or worker queue item MUST retain via `edb_reactor_conn_retain()` before queuing and release via `edb_reactor_conn_release()`. `free_conn_final()` runs strictly when `refcount == 0` and invokes `on_close` (`edb_session_destroy`) before returning the slot to `free_list`.
2. **Double-Buffered IOCP Send (`reactor_win.c`)**: Workers append to `ctx->tx_buf` under `ctx->io_mu`. `try_start_send_locked()` swaps `ctx->tx_buf` into `ctx->tx_active` ONLY when `ctx->send_pending == false`. **Never** `realloc` `ctx->tx_active` while `WSASend` is in flight.
3. **Lock Ordering (`io_mu` vs `pool_mu` vs `close_conn_socket`)**:
   - In `alloc_conn()`: pop from `free_list` under `pool_mu`, unlock `pool_mu`, initialize ALL fields (including `last_active_ms = edb_now_ms()`), then re-acquire `pool_mu` to insert into `active_head`. Otherwise `edb_reactor_reap_idle()` can observe `last_active_ms == 0`.
   - In `reactor_win.c` (`try_start_send_locked`) and `reactor_linux.c` (`flush_tx_locked`): never call `close_conn_socket()` while holding `ctx->io_mu`; return `bool need_close` and call `close_conn_socket()` after unlocking `ctx->io_mu`.
4. **Per-Connection FIFO Ordering & Work Stealing (`server.c`)**:
   - `conn->worker_busy` (CAS `0 -> 1`) guarantees at most one worker executes requests for a given connection at a time, preserving strict pipelined response order.
   - `srv->queue_shards` is clamped to `<= srv->worker_count`, and shard selection uses `% srv->queue_shards`.
   - `work_queue_pop()` uses a single `edb_cond_timedwait_ms(..., 5)` (`if`, NOT `while`) so idle workers return promptly and steal from busy shards.
5. **GROUP BY & ORDER BY Aliases (`sql_exec.c`)**:
   - `group_t` stores `const edb_row_t *const **tuples` (full joined tuple pointers across all `FROM`/`JOIN` sources), and `eval_aggregate()` updates all `ctx->sources[s].row` entries per tuple.
   - `ORDER BY` resolves bare `EXPR_IDENT` against `projs[p].name` (SELECT column aliases) before falling back to table column lookup.
6. **Cache Invalidation (`sql_exec.c`)**:
   - Result cache entries store `schema_version` + `(table_id, table->version)` snapshots + `created_ms`. Bypassed when `sess->txn != NULL`, `param_count > 0`, or `NOW()` is present.

---

## 4. Wire Protocols (Multiplexed on Port `7432`)

First-byte detection in `edb_protocol_process()` (`src/protocol.c`):
- `0x45` (`'E'`): **Binary `EDB1`** (if `network.enable_binary_protocol = true`)
- `GET `, `POST`, `HEAD`, `PUT `, `DELE`, `OPTI`: **HTTP/1.1 JSON** (if `network.enable_http = true`)
- Anything else: **Text Line Protocol** (if `network.enable_text_protocol = true`)

### Binary Protocol (`EDB1`) — Little-Endian
- **Request Frame (8-byte header + payload)**:
  - `[0]` `0x45` (`'E'`), `[1]` `op`, `[2..3]` `flags` (`u16`), `[4..7]` `len` (`u32`), `[8..8+len)` `payload`.
  - Opcodes: `0x01 PING`, `0x02 QUERY` (UTF-8 SQL), `0x03 BATCH` (`;`-delimited SQL), `0x04 AUTH` (`[u16 ulen][user][u16 plen][pass]`), `0x05 PREPARE`, `0x06 EXEC`.
- **Response Frame (8-byte header + payload)**:
  - `[0]` `0x52` (`'R'`), `[1]` `status` (`edb_status_t`), `[2..3]` `flags` (`bit 0 = is_select_like`, `bit 1 = from_cache`), `[4..7]` `len` (`u32`).
  - `PING` body: `"PONG"` (4 bytes).
  - Query body: `[u64 rows_affected][u64 last_insert_id][u64 elapsed_us]`, then:
    - If `status != 0`: `[u16 err_len][err_msg]`.
    - Else if `is_select_like`: `[u32 col_count][u32 row_count]`, per-col `[u16 name_len][name][u8 type]`, per-cell `[u8 val_type][value]`.
  - Value types: `0=NULL`, `1=INT(i64)`, `2=REAL(f64)`, `3=TEXT(u32 len + bytes)`, `4=BOOL(u8)`, `5=BLOB(u32 len + bytes)`, `6=TIMESTAMP(i64)`.
  - Status codes: `0=OK`, `1=SYNTAX`, `2=NOT_FOUND`, `3=EXISTS`, `4=CONSTRAINT`, `5=IO`, `6=OOM`, `7=AUTH`, `8=OVERLOAD`, `9=INVALID`, `10=TXN`, `11=READONLY`, `12=INTERNAL`.

### HTTP/1.1 JSON Endpoints
- `GET /health` -> `{"status":"ok","version":"1.0.0","node":"...","uptime_sec":N}` (exempt from auth)
- `GET /metrics` -> JSON counters + `latency_p50_us`, `latency_p95_us`, `latency_p99_us`
- `GET /config` -> JSON map of all 92 `section.key` entries (`admin_password` masked)
- `POST /query` -> Raw SQL body or `{"sql":"..."}` -> `{"ok":true,"rows_affected":...,"columns":[...],"rows":[[...]]}`
- `POST /batch` -> `;`-delimited SQL -> JSON array `[...]`
- Auth: `Authorization: Basic base64(user:password)` when `security.auth_enabled = true`.

---

## 5. Configuration System (92 Parameters, 11 Sections)

Precedence (lowest to highest):
1. Built-in auto-tuned defaults (`edb_config_defaults`)
2. INI file (`-c path` or `./elliottdb.conf` in CWD)
3. Env vars `ELLIOTTDB_<SECTION>_<KEY>` (uppercased, e.g. `ELLIOTTDB_SERVER_PORT=8000`)
4. CLI flags (`--<section>.<key>=<value>`, or shortcuts `-H`, `-p`, `-d`, `-t`, `-c`, `--memory`, `--no-banner`)
5. Runtime SQL `SET section.key = value` (for mutable keys)

Sections: `[server]`, `[network]`, `[concurrency]`, `[storage]`, `[buffer_pool]`, `[wal]`, `[query]`, `[index]`, `[security]`, `[logging]`, `[metrics]`.
Full machine-readable schema: [`docs/api/config-schema.json`](docs/api/config-schema.json).

---

## 6. Build, Test & Maintenance Commands

```bat
:: Windows static single binary (MSVC /O2 /MT /W4)
build.bat
:: Verify zero non-OS DLL dependencies
dumpbin /dependents build\elliottdb.exe
:: Run full end-to-end test & benchmark suite
python tests\test_suite.py
```

```bash
# Linux single binary
make            # or: make static / ./build.sh --static
# CMake (Windows or Linux)
cmake -B build_cmake -G Ninja && cmake --build build_cmake
```

Whenever modifying `g_specs[]` in `src/config.c` or updating documentation, regenerate the site schema and agent bundles:
```bash
python scripts/gen_docs_data.py
```
