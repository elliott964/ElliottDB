# ElliottDB

**ElliottDB** is a zero-dependency, single-binary, ultra-high-concurrency relational database written in pure C11 for **Windows** (`elliottdb.exe`) and **Linux** (`elliottdb`).

No Docker. No runtime dependencies. No external libraries. Just run the binary and it starts immediately with auto-tuned sensible defaults.

- **Documentation Portal**: https://elliott964.github.io/ElliottDB/ ([`docs/index.html`](docs/index.html))
- **For AI Agents**: [`AGENTS-LITE.md`](AGENTS-LITE.md) (compact spec) · [`AGENTS.md`](AGENTS.md) (full handbook) · [`docs/llms.txt`](docs/llms.txt) · [`docs/llms-full.txt`](docs/llms-full.txt) · [`docs/api/config-schema.json`](docs/api/config-schema.json)

### Verified Windows Benchmarks (MSVC `/O2 /MT`, 8 Threads)
- **Embedded `point` (`200,000` reqs)**: `493,907 req/s` (`p50=6us`, `p95=10us`, `p99=76us`)
- **TCP IOCP `ping` (`200,000` reqs, 64 clients, pipeline 32)**: `466,944 req/s` (`p50=11us`, `p95=26us`, `p99=59us`)
- **TCP IOCP `read` (`100,000` reqs, 64 clients, pipeline 16)**: `171,326 req/s` (`p50=33us`, `p95=91us`, `p99=193us`)
- **TCP IOCP `mixed` (`50,000` reqs, 64 clients, pipeline 16)**: `112,176 req/s` (`p50=54us`, `p95=140us`, `p99=225us`)

---

## Quick Start

### Start the Server (Zero-Config)
```bash
# Linux
./elliottdb

# Windows
elliottdb.exe
```
ElliottDB immediately binds `0.0.0.0:7432`, creates `./elliottdb_data`, auto-sizes its sharded buffer pool, I/O reactor (`IOCP` on Windows, `epoll` on Linux), and worker threads, and multiplexes **Binary (`EDB1`)**, **HTTP/1.1 JSON**, and **Text Line** protocols on the same port.

### Interactive SQL Client & One-Shot CLI
```bash
elliottdb cli
elliottdb cli -c "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, score REAL)"
elliottdb cli -c "INSERT INTO users (name, score) VALUES ('Alice', 99.5), ('Bob', 88.0)"
elliottdb cli -c "SELECT * FROM users WHERE score >= 90 ORDER BY score DESC" -f json
```

### HTTP/JSON API (Same Port `7432`)
```bash
curl http://127.0.0.1:7432/health
curl http://127.0.0.1:7432/metrics
curl http://127.0.0.1:7432/config
curl -X POST http://127.0.0.1:7432/query -d "SELECT * FROM users"
```

### Generate Full Configuration File
```bash
elliottdb init-config elliottdb.conf
elliottdb -c elliottdb.conf
```

### Built-In High-Concurrency Benchmark
```bash
elliottdb bench --workload ping -c 128 -t 8 -n 500000 --pipeline 32
elliottdb bench --workload mixed -c 64 -t 8 -n 200000 --pipeline 16
elliottdb bench --embedded --workload point -n 500000
```

---

## Architecture

- **Platform Abstraction Layer (`pal_win.c` / `pal_linux.c`)**: Native atomics, SRWLOCK/pthread_rwlock, IOCP/epoll, pread/pwrite, and console/POSIX signals.
- **Event-Driven Network Reactor (`reactor_win.c` / `reactor_linux.c`)**:
  - **Windows**: Multi-threaded **I/O Completion Ports (IOCP)** with overlapped `WSARecv`/`WSASend`.
  - **Linux**: Multi-reactor **edge-triggered `epoll` (`EPOLLET`)** with `SO_REUSEPORT` and `eventfd`.
  - Dynamically-growing **connection slab pool** (`max_connections = 0` = unlimited) with atomic reference counting and lock-free RX/TX handoff.
- **Sharded Worker Pool (`server.c`)**: Work-stealing sharded request queues guaranteeing per-connection FIFO response ordering for pipelined clients while scaling across all CPU cores.
- **Storage Engine (`pager.c`, `wal.c`, `index.c`, `storage.c`)**:
  - 64-shard clock-sweep **Buffer Pool** with configurable page sizes (`4096..65536` bytes) and pure `--memory` mode.
  - **Write-Ahead Log (WAL)** with CRC32C checksums, Group Commit coalescing, and crash-safe redo recovery (filtering uncommitted transactions).
  - **B+Tree** (linked leaf range scans) and **Hash** (O(1) point lookups) indexes, auto-created on `PRIMARY KEY` / `UNIQUE`.
- **SQL Engine (`sql_parser.c`, `sql_exec.c`)**:
  - Full SQL DDL/DML: `SELECT` (`DISTINCT`, `INNER/LEFT/RIGHT/CROSS JOIN` with Hash Join, `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT/OFFSET`, aggregates, `CASE`, `LIKE`, `IN`, `BETWEEN`), `INSERT` (multi-row), `UPDATE`, `DELETE`, `CREATE/DROP/ALTER TABLE`, `CREATE/DROP INDEX [USING BTREE|HASH]`, `BEGIN/COMMIT/ROLLBACK`, `PREPARE/EXECUTE/DEALLOCATE`, `EXPLAIN`, `SHOW TABLES/CONFIG/STATS`, `DESCRIBE`, `SET`, `CHECKPOINT`.
  - **64-shard Plan Cache** and **64-shard Version-Invalidated Result Cache**.

---

## Configuration

ElliottDB exposes 80+ configuration parameters across 11 sections (`[server]`, `[network]`, `[concurrency]`, `[storage]`, `[buffer_pool]`, `[wal]`, `[query]`, `[index]`, `[security]`, `[logging]`, `[metrics]`).

Precedence (lowest to highest):
1. Built-in auto-tuned defaults
2. `elliottdb.conf` (auto-loaded from CWD if present, or `-c path`)
3. Environment variables (`ELLIOTTDB_<SECTION>_<KEY>`, e.g. `ELLIOTTDB_SERVER_PORT=8000`)
4. CLI flags (`--<section>.<key>=<value>` or shortcuts like `-p`, `-H`, `-d`, `-t`, `--memory`)
5. Live runtime `SET section.key = value` over SQL for mutable settings.

---

## Building from Source

### Windows (Single Static `build\elliottdb.exe`)
Requires Visual Studio 2022 Build Tools (MSVC):
```bat
build.bat
```
Or via CMake:
```bat
cmake -B build -G Ninja
cmake --build build
```

### Linux (Single Binary `build/elliottdb`)
```bash
make
# or static binary:
make static
# or:
./build.sh --static
```
