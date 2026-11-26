#!/usr/bin/env python3
"""
Parses g_specs[] from src/config.c and builds machine- & agent-readable assets:
  - docs/api/config-schema.json
  - docs/config-data.js
  - docs/llms-full.txt
  - docs/agents.md (mirror of AGENTS.md)
  - docs/agents-lite.md (mirror of AGENTS-LITE.md)
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


def build_llms_full(specs):
    sections = {}
    for s in specs:
        sections.setdefault(s["section"], []).append(s)

    agents_path = os.path.join(ROOT, "AGENTS.md")
    agents_lite_path = os.path.join(ROOT, "AGENTS-LITE.md")
    with open(agents_path, "r", encoding="utf-8") as f:
        agents_text = f.read().strip()
    with open(agents_lite_path, "r", encoding="utf-8") as f:
        agents_lite_text = f.read().strip()

    lines = [
        "# ElliottDB v1.0.0 — Complete Knowledge Base (llms-full.txt)",
        "",
        "> Single-binary, zero-dependency, high-concurrency relational database written in C11 for Linux and Windows.",
        "> Repository: https://github.com/elliott964/ElliottDB",
        "> Live Docs: https://elliott964.github.io/ElliottDB/",
        "",
        "## Quick Start",
        "1. Run `elliottdb` (Linux) or `elliottdb.exe` (Windows) — zero configuration required.",
        "2. Connect via CLI (`elliottdb cli`), HTTP/1.1 JSON (`http://127.0.0.1:7432/query`), Binary (`EDB1`), or Text SQL on port `7432`.",
        "3. Generate config template: `elliottdb init-config elliottdb.conf`.",
        "4. Run built-in benchmarks: `elliottdb bench --workload ping -c 64 -t 8 -n 200000 --pipeline 32`.",
        "",
        "## Verified Windows Benchmarks (MSVC /O2 /MT, 8 threads)",
        "- Embedded `point` (200,000 reqs): 493,907 req/s | p50=6us p95=10us p99=76us",
        "- TCP IOCP `ping` (200,000 reqs, 64 clients, pipeline 32): 466,944 req/s | p50=11us p95=26us p99=59us",
        "- TCP IOCP `read` (100,000 reqs, 64 clients, pipeline 16): 171,326 req/s | p50=33us p95=91us p99=193us",
        "- TCP IOCP `mixed` (50,000 reqs, 64 clients, pipeline 16): 112,176 req/s | p50=54us p95=140us p99=225us",
        "",
        "## Complete SQL Reference",
        "- DDL: `CREATE TABLE [IF NOT EXISTS] t (col INT|REAL|TEXT|BOOL|BLOB|TIMESTAMP [PRIMARY KEY] [AUTO_INCREMENT] [NOT NULL] [UNIQUE] [DEFAULT v])`, `DROP TABLE [IF EXISTS] t`, `ALTER TABLE t ADD [COLUMN] coldef`, `CREATE [UNIQUE] INDEX [IF NOT EXISTS] idx ON t (col) [USING BTREE|HASH]`, `DROP INDEX [IF EXISTS] idx`.",
        "- DML: `INSERT INTO t [(cols)] VALUES (...), (...)`, `UPDATE t SET c=expr [WHERE ...]`, `DELETE FROM t [WHERE ...]`.",
        "- Queries: `SELECT [DISTINCT] items FROM t [AS a] [INNER|LEFT|RIGHT|CROSS JOIN t2 [AS b] ON expr] [WHERE expr] [GROUP BY exprs] [HAVING expr] [ORDER BY expr [ASC|DESC]] [LIMIT n [OFFSET m]]`.",
        "- Expressions & Functions: arithmetic (`+ - * / %`), concat (`||`), comparisons (`= != <> < <= > >=`), `IS [NOT] NULL`, `[NOT] IN (...)`, `[NOT] BETWEEN a AND b`, `[NOT] LIKE pat`, `AND`/`OR`/`NOT`, `CASE WHEN ... THEN ... ELSE ... END`, `COUNT(*|col|DISTINCT col)`, `SUM`, `AVG`, `MIN`, `MAX`, `LOWER`, `UPPER`, `LENGTH`, `COALESCE`, `ABS`, `NOW`, `ROUND`, `SUBSTR`.",
        "- Transactions & Prepared: `BEGIN [TRANSACTION]`, `COMMIT`, `ROLLBACK`, `PREPARE name AS sql`, `EXECUTE name(args...)`, `DEALLOCATE name`.",
        "- Introspection & Admin: `EXPLAIN <stmt>`, `SHOW TABLES`, `DESCRIBE <table>`, `SHOW CONFIG [LIKE 'pat']`, `SHOW STATS`, `SET section.key = value`, `CHECKPOINT`, `VACUUM`, `PING`.",
        "",
        f"## All {len(specs)} Configuration Parameters",
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
        "## Compact Agent Reference (AGENTS-LITE.md)",
        "",
        agents_lite_text,
        "",
        "---",
        "",
        "## Full Agent Reference (AGENTS.md)",
        "",
        agents_text,
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

    shutil.copyfile(os.path.join(ROOT, "AGENTS.md"), os.path.join(DOCS_DIR, "agents.md"))
    shutil.copyfile(os.path.join(ROOT, "AGENTS-LITE.md"), os.path.join(DOCS_DIR, "agents-lite.md"))

    llms_full_path = os.path.join(DOCS_DIR, "llms-full.txt")
    with open(llms_full_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(build_llms_full(specs))

    print(f"Generated docs data successfully: {len(specs)} config options across {len(schema_obj['sections'])} sections.")


if __name__ == "__main__":
    main()
