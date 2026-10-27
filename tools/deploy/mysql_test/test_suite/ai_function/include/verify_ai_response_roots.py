#!/usr/bin/env python3
"""Check invalid HTTP JSON roots after verify_ai_function_regression.py setup.

Use the same local fixture and the reg_c/reg_e/reg_r models from that suite.
Each failure must come after a real HTTP request and leave SELECT 42 working.
"""

import argparse
import json
import os
from pathlib import Path
import time
import traceback
import urllib.request

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--mock", default="http://127.0.0.1:8000")
    parser.add_argument("--output", required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    db = pymysql.connect(
        host=args.host,
        port=args.port,
        user="root",
        password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
        database="ai_regression",
        charset="utf8mb4",
        autocommit=True,
        read_timeout=120,
    )
    cursor = db.cursor()
    methods = [
        ("complete", "c", "/openai/completions", "select ai_complete('reg_c','root')"),
        ("embed", "e", "/openai/embeddings", "select ai_embed('reg_e','root')"),
        (
            "embed_batch",
            "e",
            "/openai/embeddings",
            "select id,ai_embed('reg_e',content) from embedding_rows order by id",
        ),
        (
            "rerank",
            "r",
            "/v1/rerank",
            "select ai_rerank('reg_r','root','[\"a\",\"b\"]')",
        ),
    ]
    tests = []
    for name, model, path, query in methods:
        for case in ["root_array", "root_null", "root_string"]:
            record = {"name": name + "/" + case}
            start = time.monotonic()
            try:
                endpoint = "reg_" + model + "_ep"
                url = args.mock + path + "?case=" + case
                cursor.execute(
                    "call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                    (endpoint, json.dumps({"url": url})),
                )
                request = urllib.request.Request(
                    args.mock + "/requests", method="DELETE"
                )
                with urllib.request.urlopen(request, timeout=10) as response:
                    json.load(response)
                try:
                    cursor.execute(query)
                except pymysql.MySQLError as error:
                    assert error.args[0] not in (0, 2002, 2003, 2006, 2013), error
                    record["error_code"] = error.args[0]
                else:
                    raise AssertionError("invalid HTTP JSON root was accepted")
                cursor.execute("select 42")
                assert cursor.fetchone() == (42,)
                with urllib.request.urlopen(
                    args.mock + "/requests", timeout=10
                ) as response:
                    requests = json.load(response)
                assert any(row["path"] == path + "?case=" + case for row in requests)
                cursor.execute(
                    "call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                    (endpoint, json.dumps({"url": args.mock + path})),
                )
                record["passed"] = True
            except Exception:
                record.update(passed=False, error=traceback.format_exc())
            record["seconds"] = round(time.monotonic() - start, 3)
            tests.append(record)
            print(json.dumps(record), flush=True)
    db.close()
    report = {
        "revision": args.revision,
        "tests": tests,
        "passed": sum(row["passed"] for row in tests),
        "total": len(tests),
    }
    Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["passed"] == report["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
