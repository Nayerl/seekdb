# GPU construction of native HNSW indexes

`LIB=HIPVS` uses AMD hipVS to construct a CAGRA base graph on the GPU, converts
it into a full mutable HNSW graph with a CPU hierarchy, and imports that graph
into VSAG. The resulting index uses seekdb's normal query, filter, delta-update,
and snapshot persistence paths. `LIB=VSAG` retains CPU construction.

## Build and run

The optional feature defaults to **OFF**. An enabled build requires Linux
x86-64, glibc 2.34 or later, the seekdb VSAG dependency, ROCm, and an installed
hipVS C library exposing CAGRA construction and HNSW conversion/serialization.
The tested Radeon image provides hipVS under `/opt/hipvs` and ROCm under
`/opt/rocm`. See [the measured report](REPORT.md) for exact versions.

```bash
# Initialize normal seekdb build dependencies first, if needed.
./build.sh release --init -DOB_BUILD_HIPVS=ON \
  -DHIPVS_ROOT=/opt/hipvs -DROCM_ROOT=/opt/rocm
make -C build_release -j8 seekdb

# The child worker needs the installed hipVS/ROCm libraries.
export LD_LIBRARY_PATH="/opt/hipvs/lib:/opt/rocm/lib:${LD_LIBRARY_PATH:-}"
python3 .github/script/seekdb/sdb.py start \
  --binary "$PWD/build_release/src/observer/seekdb" \
  --base-dir /workspace/hipvs-test --port 2881 \
  --parameter cpu_count=16
```

Deploy `seekdb_hipvs_hnsw_worker` beside the seekdb executable and
`libseekdb_hipvs_hnsw.so` in its library directory. CMake builds the worker
when building `seekdb` and installs both supporting artifacts. Keep the normal
CPU deployment by configuring without `-DOB_BUILD_HIPVS=ON`.

```sql
CREATE TABLE documents(id BIGINT PRIMARY KEY, embedding VECTOR(128));
-- Import the original float32 vectors before creating the index.
CREATE VECTOR INDEX ann ON documents(embedding)
  WITH (distance=l2, type=hnsw, lib=hipvs,
        m=32, ef_construction=200, ef_search=1000);
SET ob_hnsw_ef_search=1000;
SELECT id FROM documents
  ORDER BY l2_distance(embedding, '[...]') APPROXIMATE LIMIT 10;
```

Supported GPU construction is dense float32 L2 HNSW, no extra-info payload,
and `M` equal to 16, 32, or 64. Unsupported combinations and use of `LIB=HIPVS`
in a disabled build return an error. Small snapshots with at most
`max(128, 4*M)` rows explicitly use CPU construction and log
`gpu=false fallback=true`. Larger GPU failures propagate without CPU fallback.
Existing native snapshots and CPU delta indexes do not need a GPU to query.

## Construction and isolation

Snapshot insertion batches collect vectors and arbitrary 64-bit seekdb VIDs.
Finalization before serialization or a read creates private anonymous files,
writes the complete float32 dataset, and uses `posix_spawn` to launch the
worker. It closes inherited database descriptors other than the two data files
and standard streams. The parent waits for completion, with a 30-minute limit.

The worker uses NN-descent CAGRA construction, graph degree `2*M`, intermediate
degree 256 for M=16/32 or 512 for M=64 (capped for small inputs), and uncompressed vectors. It synchronizes
GPU work, builds the HNSW hierarchy with eight CPU threads and the requested
`ef_construction`, then serializes it. The parent checks the 64-bit HNSW layout,
every ordinal, every stored float32 vector, and edge bounds before mapping
ordinals to the original VIDs and importing the graph as VSAG `hnsw_data`.
This format is checked explicitly; another VSAG/hipVS serialization version
may require an updated importer.

hipVS and VSAG both export hnswlib symbols. They must live in separate
processes: direct linking causes symbol collisions, and loader namespaces
alone do not isolate seekdb's allocation hooks. Only the worker loads hipVS;
no GPU library is linked into seekdb. Worker startup, transfers, synchronization,
conversion, validation, import, persistence, and first-query loading all remain
inside the measured SQL construction interval.

Snapshot memory renewal also preserves the reload flag and honors it even
when another adapter still holds the same snapshot prefix. This prevents a
query from observing a freshly renewed empty index after successful DDL.
Targets created solely for deserialization use native VSAG directly.

## Reproduce the comparison

Install NumPy in the chosen Python environment and provide the `mysql` CLI.
Use a dedicated database: the benchmark recreates its `corpus` table.

```bash
OPENBLAS_NUM_THREADS=8 OMP_NUM_THREADS=8 python3 \
  docs/gpu-hnsw-build/benchmark.py \
  --rows 50000 --dim 128 --queries 100 --rounds 3 \
  --m 32 --ef-construction 200 --ef-search 1000 --min-recall 0.99 \
  --database hipvs_build_benchmark \
  --server-log /workspace/hipvs-test/log/seekdb.log \
  --output /workspace/hipvs-results.json
```

Both backends construct a fresh index in alternating order on the same imported
dataset. The timer runs from `CREATE VECTOR INDEX` through completion of the
same first ANN availability query; it includes snapshot reload and returns
only when ten distinct IDs with valid distances are available. Generation,
import, exact-ground-truth computation, and server startup are reported
separately. Do not run profiling or another benchmark concurrently.

The fixed seed is 20261030. Vectors are float32, document IDs are shuffled and
noncontiguous, and exact nearest neighbors are computed independently with
float64 NumPy distances. Three exact SQL queries cross-check that ground truth
on every run. The tool records all raw times, per-query Recall@10, medians,
speedup, server settings, query plans, dataset SHA256, and actual GPU-path logs.
The local 99% quality floor is a reproducibility choice; the competition's
official quality and speedup thresholds have not been supplied here.

The baseline caps effective `ef_search` at `max(1000, 10*topk)`; increasing the
session value beyond 1000 does not improve top-10 search. M=16 measured 97.5%
CPU recall on this dataset, so the reported comparison uses M=32 for both
backends. GPU performance results containing fallback or GPU errors fail.
Failures are preserved in the JSON, and do not produce a completed report.

After all rounds, the tool tests update, delete, reinsert, and filtering and
leaves a newly built GPU-produced native index. Restart the same database,
then repeat the exact command with `--verify-existing` and the same output
path to verify persistence and retrieval quality. Existing snapshots can also
be checked with `HIP_VISIBLE_DEVICES=-1` in the server environment.

The mysqltest case
`vector_index/t/vector_index_hipvs_validation.test` covers unsupported GPU
declarations, the CPU route, and twelve fresh build/query/drop cycles. It runs
without GPU resources and in builds where the optional feature is disabled.

## Attribution

The official [AMD hipVS project](https://github.com/AMD-Ecosystem/hipVS) provides
the CAGRA and HNSW algorithms. The seekdb hipVS integration discussion and
[PR #1314](https://github.com/oceanbase/seekdb/pull/1314) informed the environment
and C API investigation. Their batch-query performance is not used as this
implementation's construction result. This implementation constructs a native
HNSW snapshot and measures its complete SQL construction path.
