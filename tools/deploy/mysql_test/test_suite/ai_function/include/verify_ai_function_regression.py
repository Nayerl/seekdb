#!/usr/bin/env python3
"""Run SQL contract checks against a real seekdb and the adjacent local mock.

Requires PyMySQL. Use exactly the same file against baseline and candidate builds.
This supplements, and does not change, the official contest tests.
"""

import argparse
import json
import os
import time
import traceback
import urllib.request
from pathlib import Path

import pymysql

from ai_function_regression_mock import embedding


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--mock", default="http://127.0.0.1:8000")
    parser.add_argument("--database-mock", default="http://127.0.0.1:8000")
    parser.add_argument("--output", required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    db = pymysql.connect(
        host=args.host,
        port=args.port,
        user=args.user,
        password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
        charset="utf8mb4",
        autocommit=True,
        read_timeout=120,
    )
    cursor = db.cursor()

    def sql(query, params=None):
        cursor.execute(query, params)
        return cursor.fetchall()

    def one(query, params=None):
        return sql(query, params)[0][0]

    def equal(actual, expected):
        assert actual == expected, (
            f"actual={str(actual)[:500]!r}, expected={str(expected)[:500]!r}"
        )

    def fails(query, params=None):
        try:
            sql(query, params)
        except pymysql.MySQLError as error:
            assert error.args[0] not in (2002, 2003, 2006, 2013), (
                f"connection failure: {error}"
            )
        else:
            raise AssertionError("SQL succeeded, but must reject this input/response")
        equal(one("select 17 + 25"), 42)

    def requests(reset=False):
        request = urllib.request.Request(
            args.mock + "/requests", method="DELETE" if reset else "GET"
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.load(response)

    sql("create database if not exists ai_regression")
    sql("use ai_regression")
    sql("set ob_query_timeout=120000000")
    version = one("select version()")
    setup_errors = []
    models = {
        "c": ("completion", "openai", "/openai/completions"),
        "e": ("dense_embedding", "openai", "/openai/embeddings"),
        "r": ("rerank", "siliconflow", "/v1/rerank"),
        "oc": ("completion", "oLlAmA", "/api/generate"),
        "oe": ("dense_embedding", "OLLAMA", "/api/embed"),
    }
    for key, (kind, provider, path) in models.items():
        for proc, name in [
            ("DROP_AI_MODEL_ENDPOINT", "reg_" + key + "_ep"),
            ("DROP_AI_MODEL", "reg_" + key),
        ]:
            try:
                sql("call DBMS_AI_SERVICE." + proc + "(%s)", (name,))
            except pymysql.MySQLError:
                pass
        try:
            sql(
                "call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                (
                    "reg_" + key,
                    json.dumps({"type": kind, "model_name": "fixture-" + key}),
                ),
            )
            config = {
                "ai_model_name": "reg_" + key,
                "scope": "all",
                "url": args.database_mock + path,
                "provider": provider,
                "access_key": "" if key.startswith("o") else "synthetic-test-key",
            }
            sql(
                "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
                ("reg_" + key + "_ep", json.dumps(config)),
            )
        except pymysql.MySQLError as error:
            setup_errors.append({"model": key, "error": str(error)})

    tests = []

    def check(name, fn):
        start = time.monotonic()
        record = {"name": name}
        try:
            fn()
            record["passed"] = True
        except Exception:
            record.update(passed=False, error=traceback.format_exc())
        record["seconds"] = round(time.monotonic() - start, 3)
        tests.append(record)
        print(json.dumps(record, ensure_ascii=False), flush=True)

    def endpoint_case(key, case):
        path = models[key][2] + ("?case=" + case if case else "")
        sql(
            "call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
            ("reg_" + key + "_ep", json.dumps({"url": args.database_mock + path})),
        )

    def json_args():
        result = json.loads(
            one("select ai_prompt('{0}/{0}/{1}',json_array(1,true),json_object('k',2))")
        )
        equal(result, {"template": "{0}/{0}/{1}", "args": [[1, True], {"k": 2}]})

    check("prompt_json_structure", json_args)

    def long_prompt():
        sql("drop table if exists prompt_lob")
        sql("create table prompt_lob(t longtext,a longtext)")
        value = '长文本\\"\n' * 18000
        sql("insert into prompt_lob values(%s,%s)", ("{0}/{0}", value))
        result = json.loads(one("select ai_prompt(t,a) from prompt_lob"))
        equal(result, {"template": "{0}/{0}", "args": [value]})
        equal(
            one("select ai_complete('reg_c',ai_prompt(t,a)) from prompt_lob"),
            value + "/" + value,
        )

    check("prompt_longtext_and_repeated_expansion", long_prompt)

    def long_template():
        template = "长模板/" * 20000 + "{0}"
        sql("delete from prompt_lob")
        sql("insert into prompt_lob values(%s,%s)", (template, "tail"))
        equal(
            one("select ai_complete('reg_c',ai_prompt(t,a)) from prompt_lob"),
            template[:-3] + "tail",
        )

    check("prompt_template_over_64k", long_template)

    def rendered_json_args():
        raw = one(
            "select ai_complete('reg_c',ai_prompt('{0}|{1}',json_array(1,true),json_object('k',2)))"
        )
        array, obj = raw.split("|", 1)
        equal(json.loads(array), [1, True])
        equal(json.loads(obj), {"k": 2})

    check("prompt_json_rendered_into_completion", rendered_json_args)
    check(
        "prompt_repetition",
        lambda: equal(
            one(
                "select ai_complete('reg_c',ai_prompt('{0}/{0}/{0}',%s))", ("x" * 1000,)
            ),
            "/".join(["x" * 1000] * 3),
        ),
    )
    check(
        "prompt_nested",
        lambda: equal(
            one(
                "select ai_complete('reg_c',ai_prompt('outer:{0}',ai_prompt('inner:{0}',%s)))",
                ("你好",),
            ),
            "outer:inner:你好",
        ),
    )
    check(
        "prompt_no_recursive_insertion",
        lambda: equal(
            one(
                "select ai_complete('reg_c',ai_prompt('{0}:{1}',%s,%s))",
                ("{1}", "tail"),
            ),
            "{1}:tail",
        ),
    )
    check(
        "prompt_unicode_escapes_nul",
        lambda: equal(
            one(
                "select ai_complete('reg_c',ai_prompt(%s,%s))",
                ('引号\\"\n{0}\x00终', '🚀\\"\n'),
            ),
            '引号\\"\n🚀\\"\n\x00终',
        ),
    )
    check(
        "prompt_deferred_index_validation",
        lambda: equal(
            json.loads(one("select ai_prompt('{1}','a')"))["template"], "{1}"
        ),
    )
    for placeholder in ["{1}", "{9223372036854775808}", "{184467440737095516160}"]:
        check(
            "prompt_invalid_index_" + placeholder,
            lambda p=placeholder: fails(
                "select ai_complete('reg_c',ai_prompt(%s,'a'))", (p,)
            ),
        )
    check(
        "prompt_literal_braces",
        lambda: equal(
            one("select ai_complete('reg_c',ai_prompt('json: {abc} {} {0}','ok'))"),
            "json: {abc} {} ok",
        ),
    )
    check("prompt_invalid_type", lambda: fails("select ai_prompt('{0}',42)"))

    check(
        "complete_null_config",
        lambda: equal(one("select ai_complete('reg_c','hello',null)"), "hello"),
    )

    def config_object():
        requests(reset=True)
        equal(
            one(
                "select ai_complete('reg_c','config',json_object('temperature',0.2,'max_tokens',16))"
            ),
            "config",
        )
        body = requests()[-1]["body"]
        equal(body["temperature"], 0.2)
        equal(body["max_tokens"], 16)

    check("complete_json_config", config_object)
    check(
        "complete_bad_config",
        lambda: fails("select ai_complete('reg_c','hello',json_array(1))"),
    )
    for case in [
        "empty",
        "null",
        "object",
        "missing",
        "http_error",
        "root_array",
        "root_null",
        "root_string",
    ]:

        def invalid_completion(case=case):
            endpoint_case("c", case)
            try:
                fails("select ai_complete('reg_c','hello')")
            finally:
                endpoint_case("c", "")

        check("complete_response_" + case, invalid_completion)

    check(
        "embedding_default_dimension",
        lambda: equal(
            json.loads(one("select ai_embed('reg_e','single')")), embedding("single")
        ),
    )
    check(
        "embedding_explicit_dimension",
        lambda: equal(
            json.loads(one("select ai_embed('reg_e','explicit',13)")),
            embedding("explicit", 13),
        ),
    )
    check(
        "embedding_null_propagation",
        lambda: equal(one("select ai_embed('reg_e',null)"), None),
    )
    for dimension in [0, -1, 1.5]:
        check(
            "embedding_invalid_dimension_" + str(dimension),
            lambda d=dimension: fails("select ai_embed('reg_e','invalid',%s)", (d,)),
        )
    for case in [
        "bad_element",
        "empty_vector",
        "vector_object",
        "bad_index",
        "missing_index",
        "missing_row",
    ]:

        def invalid_embedding(case=case):
            endpoint_case("e", case)
            try:
                fails("select ai_embed('reg_e','invalid')")
            finally:
                endpoint_case("e", "")

        check("embedding_response_" + case, invalid_embedding)

    def bad_dimension():
        endpoint_case("e", "bad_dimension")
        try:
            fails("select ai_embed('reg_e','invalid',6)")
        finally:
            endpoint_case("e", "")

    check("embedding_response_dimension_mismatch", bad_dimension)

    def batch_rows():
        sql("drop table if exists embedding_rows")
        sql("create table embedding_rows(id int primary key,content text,dim int)")
        cursor.executemany(
            "insert into embedding_rows values(%s,%s,%s)",
            [
                (i, None if i % 11 == 0 else f"row-{i}", 6 if i < 70 else 9)
                for i in range(150)
            ],
        )
        requests(reset=True)
        result = sql(
            "select id,ai_embed('reg_e',content) from embedding_rows order by id"
        )
        for i, raw in result:
            equal(
                None if raw is None else json.loads(raw),
                None if i % 11 == 0 else embedding(f"row-{i}"),
            )
        bodies = [
            r["body"] for r in requests() if r["path"].startswith("/openai/embeddings")
        ]
        assert any(len(body["input"]) > 1 for body in bodies), (
            "no cross-row request was batched"
        )
        equal(
            sum(len(body["input"]) for body in bodies),
            sum(i % 11 != 0 for i in range(150)),
        )
        assert len(bodies) > 1, (
            "bounded requests should split this data into multiple batches"
        )
        for i, raw in sql(
            "select id,ai_embed('reg_e',content,dim) from embedding_rows where id>25 order by id limit 80"
        ):
            equal(
                None if raw is None else json.loads(raw),
                None if i % 11 == 0 else embedding(f"row-{i}", 6 if i < 70 else 9),
            )

    check("embedding_batch_reversed_response_null_filter_limit_dimensions", batch_rows)

    def duplicate_batch():
        endpoint_case("e", "duplicate_index")
        try:
            fails("select id,ai_embed('reg_e',content) from embedding_rows order by id")
        finally:
            endpoint_case("e", "")

    check("embedding_batch_duplicate_index", duplicate_batch)

    check(
        "ollama_native_completion",
        lambda: equal(one("select ai_complete('reg_oc','native')"), "native"),
    )
    check(
        "ollama_native_embedding",
        lambda: equal(
            json.loads(one("select ai_embed('reg_oe','native')")), embedding("native")
        ),
    )

    def provider_alter():
        sql(
            "call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('reg_oc_ep','{\"provider\":\"Ollama\"}')"
        )
        equal(one("select ai_complete('reg_oc','altered')"), "altered")

    check("ollama_provider_alter", provider_alter)

    def rerank(ties=False, objects=False):
        docs = (
            [{"id": f"id-{i}", "text": f"doc-{i}", "extra": i} for i in range(45)]
            if objects
            else [f"doc-{i}" for i in range(45)]
        )
        result = json.loads(
            one(
                "select ai_rerank('reg_r',%s,%s,%s)",
                (
                    "ties" if ties else "score",
                    json.dumps(docs),
                    "text" if objects else None,
                ),
            )
        )
        order = list(range(45)) if ties else list(reversed(range(45)))
        equal(len(result), len(docs))
        equal(
            [item["id"] if objects else item["index"] for item in result],
            [f"id-{i}" if objects else i for i in order],
        )
        if objects:
            equal([item["extra"] for item in result], order)

    check("rerank_strings_null_key_global_sort", rerank)
    check("rerank_strings_stable_ties", lambda: rerank(ties=True))
    check("rerank_objects_batched_identity", lambda: rerank(objects=True))
    check("rerank_objects_stable_ties", lambda: rerank(ties=True, objects=True))
    for case in ["duplicate_index", "bad_index", "bad_score", "missing_row"]:

        def invalid_rerank(case=case):
            endpoint_case("r", case)
            try:
                fails("select ai_rerank('reg_r','score','[\"doc-1\",\"doc-2\"]',null)")
            finally:
                endpoint_case("r", "")

        check("rerank_response_" + case, invalid_rerank)
    check(
        "ordinary_sql_regression",
        lambda: equal(one("select sum(id) from embedding_rows"), sum(range(150))),
    )

    result = {
        "revision": args.revision,
        "version": version,
        "setup_errors": setup_errors,
        "passed": sum(test["passed"] for test in tests),
        "total": len(tests),
        "tests": tests,
    }
    Path(args.output).write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    )
    cursor.close()
    db.close()
    print(
        json.dumps({k: result[k] for k in ("revision", "passed", "total")}), flush=True
    )
    return 0 if result["passed"] == result["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
