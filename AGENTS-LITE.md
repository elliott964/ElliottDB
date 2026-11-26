# AGENTS-LITE.md — ElliottDB Compact Agent Spec (v1.0.0)

**Identity**: Single-binary, zero-dep C11 RDBMS. Targets: `build\elliottdb.exe` (Win x64 `/MT`, `WS2_32`+`KERNEL32` only) & `build/elliottdb` (Linux x64 `pthread`+`lm`). Default port `7432` multiplexes Binary `EDB1`, HTTP/1.1 JSON, and Text SQL. Zero warnings under MSVC `/W4` & GCC `-Wall -Wextra`.

## Code Map (`src/`)
- `edb_common.h`, `edb_util.c`: `edb_status_t` (0..12), `edb_buf_t`, `edb_arena_t` (64KB blocks), `edb_crc32c`, `edb_hash64`, `edb_like_match`, text/JSON logger.
- `pal.h`, `pal_win.c`, `pal_linux.c`: atomics (`edb_atomic_*`), mutex/rwlock/cond/thread, `pread`/`pwrite`, signals, TCP helpers.
- `config.h`, `config.c`, `metrics.h`, `metrics.c`: 92 config keys (11 sections), INI/env/CLI/runtime `SET`, atomic counters + 16-bucket us latency histogram.
- `pager.h`, `pager.c`, `wal.h`, `wal.c`: 64-shard clock-sweep buffer pool, slotted pages (`rid=(pid<<16)|slot`), CRC32C WAL (`off|normal|full`), group commit, crash replay.
- `index.h`, `index.c`, `storage.h`, `storage.c`: B+Tree (linked leaves, `(key,rid)` tie-break) + Hash index, catalog persistence, per-table `rwlock` + atomic `version`, undo-log txns.
- `sql_ast.h`, `sql_parser.c`, `sql_exec.h`, `sql_exec.c`: lexer/RD-parser, 64-shard plan & versioned result caches, index eq/range scans, Hash Join, `GROUP BY`/`HAVING`, `ORDER BY`, `EXPLAIN`, `PREPARE`/`EXECUTE`.
- `protocol.h`, `protocol.c`: auto-detect `EDB1` / HTTP / Text; `edb_bin_encode_frame`, `edb_bin_decode_response`, Basic Auth.
- `reactor.h`, `reactor_win.c`, `reactor_linux.c`: slab-pooled `edb_conn_t`, Windows IOCP (`WSARecv`/`WSASend`), Linux `EPOLLET`+`SO_REUSEPORT`+`eventfd`.
- `server.h`, `server.c`: sharded work-stealing queues, worker pool, bg flush/checkpoint/reaper thread.
- `cli.c`, `bench.c`, `main.c`: `EDB1` REPL/CLI, TCP+`--embedded` bench, subcommand dispatch (`server|cli|bench|init-config|version|help`).

## Hard Invariants (Do Not Violate)
1. **Win Headers**: Include `<winsock2.h>`/`<ws2tcpip.h>` (via `pal.h`) BEFORE `<windows.h>`. Never use `<stdatomic.h>` or GCC extensions (VLAs/`typeof`). Keep `_GNU_SOURCE` on Linux.
2. **Conn Lifetime**: `refcount=1` on accept; retain for every pending I/O, kick, or worker task; `free_conn_final()` runs at `refcount==0` calling `on_close` before slab recycle.
3. **IOCP Double-Buffer (`reactor_win.c`)**: Workers append to `ctx->tx_buf` under `io_mu`; `try_start_send_locked()` swaps `tx_buf`->`tx_active` only when `!send_pending`. Never `realloc` an in-flight `WSASend` buffer.
4. **Lock Hygiene**: In `alloc_conn()`, initialize `last_active_ms` & `conn` BEFORE linking into `active_head` under `pool_mu`. Never call `close_conn_socket()` while holding `io_mu`—return `bool need_close` and close after unlock.
5. **Worker Queues (`server.c`)**: `conn->worker_busy` enforces 1 worker/conn (FIFO pipeline). `queue_shards <= worker_count`; index with `% queue_shards`. `work_queue_pop()` uses single 5ms `timedwait` (`if`, not `while`) to allow work stealing.
6. **SQL Exec (`sql_exec.c`)**: `group_t` stores full joined `tuples` (`const edb_row_t *const **`); `ORDER BY` checks `projs[p].name` aliases first; `qsort_s`(Win) vs `qsort_r`(Linux) arg order differs.

## Wire Protocols (Port 7432)
- **Detect**: byte[0]==`0x45` -> Binary; `GET |POST|HEAD|PUT |DELE|OPTI` -> HTTP; else -> Text (`\n`-delimited, `AUTH u p`, `QUIT`).
- **Binary `EDB1` (LE)**:
  - Req `[0]='E', [1]=op(1=PING,2=QUERY,3=BATCH,4=AUTH,5=PREPARE,6=EXEC), [2..3]=flags, [4..7]=len` + payload.
  - Resp `[0]='R', [1]=status(0..12), [2..3]=flags(b0=select,b1=cached), [4..7]=len`. Body: `PING`->`"PONG"`; Query->`[u64 aff][u64 last_id][u64 us]`, err? `[u16 elen][err]` : select? `[u32 cols][u32 rows]` + cols `[u16 nl][name][u8 typ]` + cells `[u8 typ][val]`. Types: `0=NULL,1=INT(i64),2=REAL(f64),3=TEXT(u32+b),4=BOOL(u8),5=BLOB(u32+b),6=TS(i64)`.
- **HTTP JSON**: `GET /health`, `GET /metrics`, `GET /config`, `POST /query` (raw SQL or `{"sql":...}`), `POST /batch` (`;`-sep SQL). Basic Auth (`401` except `/health`).

## Config & Commands
- **Precedence**: Defaults < `elliottdb.conf` (`-c`) < `ELLIOTTDB_<SEC>_<KEY>` < `--sec.key=val` (`-H,-p,-d,-t,--memory,--no-banner`) < SQL `SET sec.key=val`.
- **Sections**: `server, network, concurrency, storage, buffer_pool, wal, query, index, security, logging, metrics` (schema: `docs/api/config-schema.json`).
- **Build & Test**:
  - Win: `build.bat` | CMake: `cmake -B build_cmake -G Ninja && cmake --build build_cmake` | Linux: `make` / `./build.sh --static`
  - Test: `python tests\test_suite.py` | Regenerate docs data: `python scripts/gen_docs_data.py`
