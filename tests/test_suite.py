import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request

EXE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "elliottdb.exe"))


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http_req(port, method, path, body=None, headers=None):
    url = f"http://127.0.0.1:{port}{path}"
    data = body.encode("utf-8") if isinstance(body, str) else body
    req = urllib.request.Request(url, data=data, method=method)
    if headers:
        for k, v in headers.items():
            req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))


def bin_encode(op, payload=b""):
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    return struct.pack("<BBHI", ord("E"), op, 0, len(payload)) + payload


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("EOF")
        buf.extend(chunk)
    return bytes(buf)


def bin_recv(sock):
    hdr = recv_exact(sock, 8)
    magic, status, flags, plen = struct.unpack("<BBHI", hdr)
    assert magic == ord("R"), f"bad magic {magic}"
    body = recv_exact(sock, plen) if plen > 0 else b""
    return status, flags, body


def bin_decode_result(status, flags, body):
    rows_aff, last_id, elapsed_us = struct.unpack_from("<QQQ", body, 0)
    pos = 24
    if status != 0:
        (elen,) = struct.unpack_from("<H", body, pos)
        pos += 2
        err = body[pos:pos + elen].decode("utf-8")
        return {"ok": False, "status": status, "error": err}
    is_sel = (flags & 1) != 0
    if not is_sel:
        return {"ok": True, "rows_affected": rows_aff, "last_insert_id": last_id}
    col_cnt, row_cnt = struct.unpack_from("<II", body, pos)
    pos += 8
    cols = []
    for _ in range(col_cnt):
        (nl,) = struct.unpack_from("<H", body, pos)
        pos += 2
        cols.append(body[pos:pos + nl].decode("utf-8"))
        pos += nl + 1
    rows = []
    for _ in range(row_cnt):
        row = []
        for _ in range(col_cnt):
            vt = body[pos]
            pos += 1
            if vt == 0:
                row.append(None)
            elif vt in (1, 6):
                (v,) = struct.unpack_from("<q", body, pos)
                pos += 8
                row.append(v)
            elif vt == 2:
                (v,) = struct.unpack_from("<d", body, pos)
                pos += 8
                row.append(v)
            elif vt == 4:
                v = body[pos] != 0
                pos += 1
                row.append(v)
            elif vt == 3:
                (sl,) = struct.unpack_from("<I", body, pos)
                pos += 4
                row.append(body[pos:pos + sl].decode("utf-8"))
                pos += sl
            else:
                (bl,) = struct.unpack_from("<I", body, pos)
                pos += 4 + bl
                row.append(None)
        rows.append(row)
    return {"ok": True, "columns": cols, "rows": rows, "cached": (flags & 2) != 0}


def wait_server(port):
    for _ in range(50):
        try:
            code, data = http_req(port, "GET", "/health")
            if code == 200 and data.get("status") == "ok":
                return
        except Exception:
            time.sleep(0.05)
    raise RuntimeError(f"Server did not start on port {port}")


def start_server(port, data_dir, extra_args=None, env=None):
    args = [EXE, "-p", str(port), "-d", data_dir, "--no-banner"]
    if extra_args:
        args.extend(extra_args)
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=merged_env)
    try:
        wait_server(port)
    except Exception:
        proc.kill()
        out, err = proc.communicate()
        raise RuntimeError(f"Server failed: {out.decode()} {err.decode()}")
    return proc


def test_sql_and_protocols():
    print("[1/5] Testing SQL DDL/DML, Joins, Indexes, Transactions, and 3 Protocols...")
    tmp = tempfile.mkdtemp(prefix="edb_test1_")
    port = find_free_port()
    proc = start_server(port, tmp)
    try:
        # HTTP /health, /metrics, /config
        code, h = http_req(port, "GET", "/health")
        assert code == 200 and h["version"] == "1.0.0"
        code, cfg = http_req(port, "GET", "/config")
        assert code == 200 and len(cfg) >= 75

        def q(sql):
            c, r = http_req(port, "POST", "/query", sql)
            assert r.get("ok"), f"SQL failed ({sql}): {r}"
            return r

        # DDL + Auto-index + Hash index
        q("CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT NOT NULL, dept_id INT, salary REAL)")
        q("CREATE TABLE depts (id INT PRIMARY KEY, dname TEXT)")
        q("CREATE INDEX idx_users_dept ON users (dept_id) USING HASH")
        q("CREATE INDEX idx_users_salary ON users (salary) USING BTREE")

        q("INSERT INTO depts (id, dname) VALUES (10, 'Engineering'), (20, 'Sales')")
        r = q("INSERT INTO users (name, dept_id, salary) VALUES ('Alice', 10, 150.0), ('Bob', 10, 120.0), ('Carol', 20, 110.0), ('Dave', 20, 95.0)")
        assert r["rows_affected"] == 4

        # Point lookup + range scan + aggregates + joins + order + limit
        r = q("SELECT name, salary FROM users WHERE id = 1")
        assert r["rows"] == [["Alice", 150.0]]
        # Second call hits result cache
        r2 = q("SELECT name, salary FROM users WHERE id = 1")
        assert r2["cached"] is True and r2["rows"] == [["Alice", 150.0]]

        r = q("SELECT name FROM users WHERE salary BETWEEN 100.0 AND 130.0 ORDER BY salary DESC")
        assert r["rows"] == [["Bob"], ["Carol"]]

        r = q("SELECT d.dname, COUNT(*) AS cnt, SUM(u.salary) AS total FROM users u INNER JOIN depts d ON u.dept_id = d.id GROUP BY d.dname ORDER BY total DESC")
        assert r["rows"] == [["Engineering", 2, 270.0], ["Sales", 2, 205.0]]

        # EXPLAIN
        r = q("EXPLAIN SELECT * FROM users WHERE id = 2")
        assert r["rows"][0][0] == "INDEX_EQ_SCAN"

        # UPDATE & DELETE
        q("UPDATE users SET salary = salary + 10.0 WHERE dept_id = 10")
        r = q("SELECT salary FROM users WHERE id = 1")
        assert r["rows"] == [[160.0]]
        q("DELETE FROM users WHERE id = 4")
        r = q("SELECT COUNT(*) FROM users")
        assert r["rows"] == [[3]]

        # Transactions: ROLLBACK and COMMIT
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            for sql in ["BEGIN", "INSERT INTO users (name, dept_id, salary) VALUES ('Eve', 10, 99.0)", "ROLLBACK"]:
                s.sendall(bin_encode(2, sql))
                st, fl, b = bin_recv(s)
                res = bin_decode_result(st, fl, b)
                assert res["ok"], res
            s.sendall(bin_encode(2, "SELECT COUNT(*) FROM users"))
            res = bin_decode_result(*bin_recv(s))
            assert res["rows"] == [[3]]

            # Pipelined binary PING + QUERY
            pipe = bin_encode(1) * 5 + bin_encode(2, "SELECT name FROM users ORDER BY id ASC")
            s.sendall(pipe)
            for _ in range(5):
                st, fl, b = bin_recv(s)
                assert st == 0 and b == b"PONG"
            res = bin_decode_result(*bin_recv(s))
            assert res["rows"] == [["Alice"], ["Bob"], ["Carol"]]

            # PREPARE / EXECUTE
            s.sendall(bin_encode(2, "PREPARE get_u AS SELECT name FROM users WHERE id = $1"))
            assert bin_decode_result(*bin_recv(s))["ok"]
            s.sendall(bin_encode(2, "EXECUTE get_u(2)"))
            res = bin_decode_result(*bin_recv(s))
            assert res["rows"] == [["Bob"]]

        # Text protocol on same port
        with socket.create_connection(("127.0.0.1", port), timeout=5) as ts:
            ts.sendall(b"SELECT COUNT(*) FROM users\nQUIT\n")
            txt = b""
            while True:
                chunk = ts.recv(4096)
                if not chunk:
                    break
                txt += chunk
            assert b"3" in txt and b"BYE" in txt

        # CLI one-shot tool
        out = subprocess.check_output([EXE, "cli", "-H", "127.0.0.1", "-p", str(port), "-c", "SELECT name FROM users WHERE id = 1", "-f", "json"], text=True)
        assert json.loads(out)["rows"] == [["Alice"]]
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)
    print("  PASS")


def test_persistence_and_crash_recovery():
    print("[2/5] Testing Graceful Restart & Hard-Kill WAL Crash Recovery...")
    tmp = tempfile.mkdtemp(prefix="edb_test2_")
    port = find_free_port()
    proc = start_server(port, tmp, ["--wal.sync_mode=normal"])
    try:
        http_req(port, "POST", "/query", "CREATE TABLE accounts (id INT PRIMARY KEY, bal INT)")
        http_req(port, "POST", "/query", "INSERT INTO accounts VALUES (1, 1000), (2, 2000)")
        http_req(port, "POST", "/query", "CHECKPOINT")
        http_req(port, "POST", "/query", "INSERT INTO accounts VALUES (3, 3000)")
        # Uncommitted transaction should NOT survive crash
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(bin_encode(2, "BEGIN") + bin_encode(2, "INSERT INTO accounts VALUES (99, 9999)"))
            bin_recv(s)
            bin_recv(s)
        # Hard kill!
        proc.kill()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
        raise

    port2 = find_free_port()
    proc2 = start_server(port2, tmp)
    try:
        _, r = http_req(port2, "POST", "/query", "SELECT id, bal FROM accounts ORDER BY id ASC")
        assert r["rows"] == [[1, 1000], [2, 2000], [3, 3000]], f"Recovered rows mismatch: {r}"
    finally:
        proc2.terminate()
        proc2.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)
    print("  PASS")


def test_config_and_auth():
    print("[3/5] Testing init-config, Config File, Env Vars, Live SET, and Auth...")
    tmp = tempfile.mkdtemp(prefix="edb_test3_")
    cfg_path = os.path.join(tmp, "elliottdb.conf")
    subprocess.check_call([EXE, "init-config", cfg_path], stdout=subprocess.DEVNULL)
    assert os.path.exists(cfg_path)

    port = find_free_port()
    proc = start_server(
        port,
        tmp,
        ["-c", cfg_path, "--security.auth_enabled=true", "--security.admin_password=secretpw"],
        env={"ELLIOTTDB_SERVER_NODE_NAME": "custom-node-99"},
    )
    try:
        code, _ = http_req(port, "POST", "/query", "SELECT 1")
        assert code == 401
        import base64
        tok = base64.b64encode(b"admin:secretpw").decode("ascii")
        code, r = http_req(port, "POST", "/query", "SHOW CONFIG LIKE 'server.node_name'",
                           headers={"Authorization": f"Basic {tok}"})
        assert code == 200 and r["rows"][0][1] == "custom-node-99"
        code, r = http_req(port, "POST", "/query", "SET query.slow_query_ms = 250",
                           headers={"Authorization": f"Basic {tok}"})
        assert code == 200 and r["ok"]
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)
    print("  PASS")


def test_high_concurrency_stress():
    print("[4/5] Testing 200 Concurrent Clients Stress Test...")
    tmp = tempfile.mkdtemp(prefix="edb_test4_")
    port = find_free_port()
    proc = start_server(port, tmp, ["--memory"])
    errors = []
    try:
        http_req(port, "POST", "/query", "CREATE TABLE kv (id INT PRIMARY KEY, v INT)")
        vals = ",".join(f"({i},{i*10})" for i in range(1, 201))
        http_req(port, "POST", "/query", f"INSERT INTO kv VALUES {vals}")

        def worker(cid):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
                    batch = b"".join(bin_encode(2, f"SELECT v FROM kv WHERE id = {(cid % 200) + 1}") for _ in range(20))
                    s.sendall(batch)
                    for _ in range(20):
                        st, fl, b = bin_recv(s)
                        res = bin_decode_result(st, fl, b)
                        if not res["ok"] or res["rows"] != [[((cid % 200) + 1) * 10]]:
                            errors.append(res)
            except Exception as e:
                errors.append(str(e))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(200)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert not errors, f"Concurrency errors: {errors[:5]}"
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)
    print("  PASS (200 concurrent clients x 20 pipelined queries = 4000 queries, 0 errors)")


def test_benchmarks():
    print("[5/5] Running Built-In Benchmarks (Embedded + TCP IOCP)...")
    out_emb = subprocess.check_output([EXE, "bench", "--embedded", "--workload", "point", "-t", "8", "-n", "200000"], text=True)
    print(" ", out_emb.strip().splitlines()[-1])

    tmp = tempfile.mkdtemp(prefix="edb_test5_")
    port = find_free_port()
    proc = start_server(port, tmp, ["--memory"])
    try:
        for wl, n, p in [("ping", "200000", "32"), ("read", "100000", "16"), ("mixed", "50000", "16")]:
            out = subprocess.check_output([EXE, "bench", "-H", "127.0.0.1", "-p", str(port),
                                           "--workload", wl, "-c", "64", "-t", "8", "-n", n, "--pipeline", p], text=True)
            print(" ", out.strip().splitlines()[-1])
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)
    print("  PASS")


if __name__ == "__main__":
    test_sql_and_protocols()
    test_persistence_and_crash_recovery()
    test_config_and_auth()
    test_high_concurrency_stress()
    test_benchmarks()
    print("\nALL ELLIOTTDB TESTS PASSED SUCCESSFULLY!")
