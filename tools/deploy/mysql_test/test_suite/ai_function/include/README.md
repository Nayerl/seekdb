# AI function regression checks

These checks supplement the unchanged contest mysqltest cases. Run them only
against a dedicated seekdb instance: they recreate `ai_regression` and synthetic
`reg_*` AI models/endpoints. PyMySQL is required. Authentication headers are not
recorded by the team-authored HTTP fixture.

```sh
python3 ai_function_regression_mock.py --port 8000 --default-dimension 6
```

In another terminal, from this directory:

```sh
python3 verify_ai_function_regression.py --revision "$REVISION" --output regression.json
python3 verify_ai_response_roots.py --revision "$REVISION" --output response-roots.json
python3 verify_ai_endpoint_lifecycle.py --revision "$REVISION" --output endpoint-lifecycle.json
```

The first script has 52 fixed checks covering LOB and nested prompts, repeated
placeholder expansion, NULL/config handling, malformed responses, embedding
request batching and row order, Ollama's native protocol, and stable reranking
across batches. Run exactly the same script on baseline and candidate. The root
checks depend on the first script's models/table and verify that rejected HTTP
JSON roots do not crash the server. Lifecycle checks verify persisted keyless
Ollama endpoints and that OpenAI still requires an access key.

`--mock` is the address used by the check process. If seekdb runs on another
host, the first script's `--database-mock` must be an address reachable from that
host. The fixture binds to localhost by default.

For the official `ai_function_enhancement.test`, restart this fixture with
`--default-dimension 768` because its frozen expected result uses that dimension.
Use the OceanBase dependency package's `obclient/bin/mysqltest`; stock MariaDB
mysqltest may reject the official result format. From the repository root:

```sh
mkdir -p /tmp/ai-mysqltest
for name in ai_prompt ai_function_enhancement; do
  "$MYSQLTEST" --host=127.0.0.1 --port=2881 --user=root --database=test \
    --test-file="tools/deploy/mysql_test/test_suite/ai_function/t/$name.test" \
    --result-file="tools/deploy/mysql_test/test_suite/ai_function/r/mysql/$name.result" \
    --logdir=/tmp/ai-mysqltest --tmpdir=/tmp
done
```

Create the `test` database first. These two official files and their expected
outputs are not modified. This fixture is not the organizer's mock, so passing
locally does not assert a platform score or hidden-test result.

Native Linux ARM64 validation on 2026-10-28 used the same RelWithDebInfo/O2 build
options and instance limits for frozen baseline `5bce9e06d1281347ad61344e4b01fab9158c58a2`
and the candidate: 4 GiB seekdb memory, 4 database CPUs, 1 GiB initial/4 GiB maximum
data file, and 2 GiB log disk. The frozen baseline passed 27/52 fixed checks; the
candidate passed 52/52 and 12/12 invalid-root checks, plus both official tests.
The two C++ unittest files named in the task are absent from this frozen source
tree and were not run. Linux x86_64/official CI validation remains necessary.
