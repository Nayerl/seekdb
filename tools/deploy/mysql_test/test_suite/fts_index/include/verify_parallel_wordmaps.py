#!/usr/bin/env python3
"""Compare all built-in tokenizers with a frozen baseline, then interleave sessions.

This exercises the ownership invariant of the per-call word-frequency map.
Each worker has its own SQL connection and must receive the complete baseline
JSON value, including duplicate frequencies and document length.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import time

import pymysql

from verify_hotpath_regression import samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--reference")
    parser.add_argument("--workers", type=int, default=12)
    parser.add_argument("--rounds", type=int, default=200)
    args = parser.parse_args()
    assert args.workers > 0 and args.rounds > 0
    connection = dict(
        host=args.host, port=args.port, user=args.user,
        password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
        charset="utf8mb4", autocommit=True, read_timeout=180,
    )
    cases = [
        {"name": f"{tokenizer}/{mode}/{index}", "text": text,
         "tokenizer": tokenizer, "properties": json.dumps([{"output": mode}])}
        for tokenizer in ("ik", "beng", "space", "ngram", "ngram2")
        for mode in ("default", "all")
        for index, text in enumerate(samples()[:17]) if text is not None
    ]
    report = {"revision": args.revision, "started": time.time(), "cases": []}

    def execute(cursor, case):
        cursor.execute("SELECT tokenize(%s,%s,%s)",
                       (case["text"], case["tokenizer"], case["properties"]))
        rows = cursor.fetchall()
        return [[json.loads(value) if isinstance(value, str) else value for value in row]
                for row in rows]

    try:
        with pymysql.connect(**connection) as db:
            with db.cursor() as cursor:
                cursor.execute("SELECT version()")
                report["version"] = cursor.fetchone()[0]
                for case in cases:
                    report["cases"].append({"name": case["name"], "rows": execute(cursor, case)})
        expected = report["cases"]
        if args.reference:
            frozen = json.loads(Path(args.reference).read_text())["cases"]
            assert expected == frozen, "Serial tokenizer results differ from frozen baseline"
            expected = frozen

        def worker(worker_id):
            with pymysql.connect(**connection) as db:
                with db.cursor() as cursor:
                    for iteration in range(args.rounds):
                        index = (worker_id * 31 + iteration * 13) % len(cases)
                        actual = execute(cursor, cases[index])
                        assert actual == expected[index]["rows"], (
                            f"Session {worker_id}, iteration {iteration}: {cases[index]['name']}"
                        )
            return args.rounds

        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            counts = list(executor.map(worker, range(args.workers)))
        report["parallel"] = {"workers": args.workers, "queries": sum(counts), "all_equal": True}
        report["result_sha256"] = hashlib.sha256(json.dumps(
            report["cases"], ensure_ascii=False, sort_keys=True
        ).encode()).hexdigest()
        report["passed"] = True
        print(json.dumps({key: value for key, value in report.items() if key != "cases"}), flush=True)
    except Exception as error:
        report["passed"] = False
        report["failure"] = str(error)
        raise
    finally:
        report["finished"] = time.time()
        Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
