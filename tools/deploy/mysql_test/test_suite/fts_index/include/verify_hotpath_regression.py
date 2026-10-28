#!/usr/bin/env python3
"""Compare real TOKENIZE/MATCH results from frozen baseline and candidate builds.

Requires PyMySQL. This supplements the unchanged official FTS tests/benchmark.
Use --reference baseline.json to compare complete results, not just hit counts.
Only the dedicated fts_hotpath_regression database is recreated.
"""

import argparse
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import random
import time

import pymysql


def samples():
    texts = [
        None,
        "",
        " ",
        "\t\n\r \x00",
        "lowercase tokens remain unchanged",
        "UPPERCASE MixedCase I Istanbul i 123",
        "The quick brown fox jumps over the lazy dog.",
        "audit,performance; database-index / full_text: query!",
        "'quoted' ((parentheses)) [brackets] <html>",
        "OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化。",
        "三百六十五天，3.14米，2026年10月28日，20kg。",
        "Café CAFÉ Straße STRASSE naïve Ångström İstanbul",
        "中文与English 42混合，東京大阪 한글 Ελληνικά русский",
        "😀 emoji 🚀 launch 𠮷 野家 🧑‍💻 developer",
        "word\x00next\x01control\x7fend",
        "a" * 85 + " " + "B" * 84 + " " + "end",
        "全文检索 database performance audit 123 " * 150,
    ]
    rng = random.Random(20261028)
    words = [
        "database",
        "Audit",
        "PERFORMANCE",
        "OceanBase",
        "索引",
        "分词器",
        "查询",
        "Café",
        "東京",
        "😀",
        "123.45",
        "3kg",
    ]
    separators = [" ", "\t", "\n", ",", ".", "，", "-", "_", "/", ":", "!", "\x00"]
    for _ in range(72):
        texts.append(
            "".join(
                rng.choice(words) + rng.choice(separators)
                for _ in range(rng.randint(1, 30))
            )
        )
    return texts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=2881, type=int)
    parser.add_argument("--user", default="root")
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--reference")
    args = parser.parse_args()
    db = pymysql.connect(
        host=args.host,
        port=args.port,
        user=args.user,
        password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
        charset="utf8mb4",
        autocommit=True,
        read_timeout=180,
    )
    cursor = db.cursor()
    report = {"revision": args.revision, "started": time.time(), "cases": []}

    def sql(query, values=None):
        cursor.execute(query, values)
        return cursor.fetchall()

    def capture(name, query, values=None, allowed_errors=()):
        try:
            result = sql(query, values)
        except pymysql.MySQLError as error:
            if error.args[0] not in allowed_errors:
                raise
            report["cases"].append({"name": name, "error_code": error.args[0]})
            return
        # Keep full returned values and row order; JSON text is parsed to avoid
        # treating equivalent JSON whitespace/key order as a semantic change.
        rows = []
        for row in result:
            parsed = []
            for value in row:
                if isinstance(value, Decimal):
                    value = str(value)
                if isinstance(value, str):
                    try:
                        value = json.loads(value)
                    except (ValueError, TypeError):
                        pass
                parsed.append(value)
            rows.append(parsed)
        report["cases"].append({"name": name, "rows": rows})

    try:
        report["version"] = sql("select version()")[0][0]
        sql("set ob_query_timeout=180000000")
        for tokenizer in ["beng", "ik"]:
            for i, text in enumerate(samples()):
                capture(
                    f"{tokenizer}/default/{i}",
                    "select tokenize(%s,%s)",
                    (text, tokenizer),
                    allowed_errors=(1235,) if text is None else (),
                )
                capture(
                    f"{tokenizer}/all/{i}",
                    'select tokenize(%s,%s,\'[{"output":"all"}]\')',
                    (text, tokenizer),
                    allowed_errors=(1235,) if text is None else (),
                )
            # Current seekdb exposes only binary and utf8mb4. Also retain its
            # rejection of unsupported encodings instead of silently skipping.
            for charset in ["binary", "latin1", "gbk", "utf16"]:
                for i, text in enumerate(
                    ["UPPER lower 123", "Café 混合 OceanBase", "punctuation,/!? tail"]
                ):
                    capture(
                        f"{tokenizer}/{charset}/{i}",
                        f"select tokenize(convert(%s using {charset}),%s)",
                        (text, tokenizer),
                        allowed_errors=(1115, 1235),
                    )
            for collation in [
                "utf8mb4_bin",
                "utf8mb4_general_ci",
                "utf8mb4_unicode_ci",
            ]:
                capture(
                    f"{tokenizer}/{collation}",
                    f"select tokenize(%s collate {collation},%s)",
                    ("I i Café CAFÉ Straße 中文", tokenizer),
                    allowed_errors=(1273,),
                )

        sql("drop database if exists fts_hotpath_regression")
        sql("create database fts_hotpath_regression")
        sql("use fts_hotpath_regression")
        documents = [
            (
                i,
                f"Document {i} 全文索引",
                f"database audit performance {i} " + samples()[4 + i % 12],
            )
            for i in range(1, 49)
        ]
        queries = [
            "database audit",
            "全文索引 分词器",
            "OceanBase",
            "Café",
            "123",
            "nonexistent_unique_term",
            "query performance",
        ]
        for tokenizer in ["beng", "ik"]:
            table = "documents_" + tokenizer
            sql(
                f"create table {table}(id int primary key,title varchar(200),body text)"
            )
            cursor.executemany(f"insert into {table} values(%s,%s,%s)", documents)
            sql(
                f"alter table {table} add fulltext index ft(title,body) with parser {tokenizer}"
            )
            for phase in ["initial", "updated"]:
                if phase == "updated":
                    sql(
                        f"update {table} set body='fresh lighthouse audit 全文检索' where id=1"
                    )
                    sql(f"delete from {table} where id=2")
                    sql(
                        f"insert into {table} values(101,'Added document','fresh lighthouse database 全文索引')"
                    )
                for i, query in enumerate(queries + ["fresh lighthouse"]):
                    capture(
                        f"match/{tokenizer}/{phase}/{i}",
                        f"select id,title,body from {table} where match(title,body) against(%s) order by id",
                        (query,),
                    )
                    capture(
                        f"limit/{tokenizer}/{phase}/{i}",
                        f"select id from {table} where match(title,body) against(%s) order by id limit 5",
                        (query,),
                    )
        capture("ordinary-sql", "select count(*),sum(id) from documents_beng")
        serialized = json.dumps(
            report["cases"], ensure_ascii=False, sort_keys=True
        ).encode()
        report["result_sha256"] = hashlib.sha256(serialized).hexdigest()
        if args.reference:
            reference = json.loads(Path(args.reference).read_text())
            old = {case["name"]: case for case in reference["cases"]}
            report["differences"] = [
                case["name"]
                for case in report["cases"]
                if old.get(case["name"]) != case
            ]
            report["passed"] = not report["differences"] and len(old) == len(
                report["cases"]
            )
            assert report["passed"], f"Result differences: {report['differences']}"
        else:
            report["captured"] = True
        print(
            json.dumps(
                {k: v for k, v in report.items() if k != "cases"}, ensure_ascii=False
            ),
            flush=True,
        )
    except Exception as error:
        report["failure"] = str(error)
        report["passed"] = False
        raise
    finally:
        report["finished"] = time.time()
        Path(args.output).write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        )
        db.close()


if __name__ == "__main__":
    main()
