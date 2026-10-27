#!/usr/bin/env python3
"""Check keyless Ollama persistence and provider changes against the local fixture."""

import argparse
import json
import os
from pathlib import Path

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--mock", default="http://127.0.0.1:8000")
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    db = pymysql.connect(
        host=args.host,
        port=args.port,
        user="root",
        password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
        charset="utf8mb4",
        autocommit=True,
        read_timeout=120,
    )
    cursor = db.cursor()
    model, endpoint = "reg_lifecycle", "reg_lifecycle_ep"
    checks = []

    def reject(query, values):
        try:
            cursor.execute(query, values)
        except pymysql.MySQLError as error:
            assert error.args[0] == 11114, error  # required parameter is empty
        else:
            raise AssertionError("empty access key accepted for OpenAI")

    def alter(config):
        cursor.execute(
            "call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
            (endpoint, json.dumps(config)),
        )

    def complete(text):
        cursor.execute("select ai_complete(%s,%s)", (model, text))
        assert cursor.fetchone() == (text,)

    try:
        for proc, name in [
            ("DROP_AI_MODEL_ENDPOINT", endpoint),
            ("DROP_AI_MODEL", model),
        ]:
            try:
                cursor.execute("call DBMS_AI_SERVICE." + proc + "(%s)", (name,))
            except pymysql.MySQLError as error:
                assert error.args[0] in (11112, 11118), error
        cursor.execute(
            "call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
            (model, json.dumps({"type": "completion", "model_name": "fixture"})),
        )
        config = {
            "ai_model_name": model,
            "scope": "all",
            "url": args.mock + "/api/generate",
            "provider": "ollama",
        }
        cursor.execute(
            "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
            (endpoint, json.dumps(config)),
        )
        complete("omitted-key")
        checks.append("create_without_key")
        for i in range(3):
            alter({"provider": "Ollama"})
            complete("after-alter-" + str(i))
        checks.append("repeated_alter_and_reload_without_key")
        reject(
            "call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
            (endpoint, json.dumps({"provider": "openai"})),
        )
        complete("rejected-change-keeps-ollama")
        checks.append("keyless_provider_change_rejected_atomically")
        alter(
            {
                "provider": "openai",
                "access_key": "synthetic-test-key",
                "url": args.mock + "/openai/completions",
            }
        )
        complete("with-key")
        checks.append("provider_change_with_key")
        config.update(provider="openai", url=args.mock + "/openai/completions")
        reject(
            "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
            ("reg_empty_openai_ep", json.dumps(config)),
        )
        checks.append("openai_create_without_key_rejected")
    finally:
        db.close()
    report = {"revision": args.revision, "passed": len(checks), "checks": checks}
    Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
