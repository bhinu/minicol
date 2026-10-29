# minicol

A dependency-free C++17 columnar storage engine for learning storage layout, adaptive compression, pruning, and batch execution. Writes actual binary files, reopens them, and scans only predicate columns.

## Build and run

Requires a C++17 compiler and Make; Python 3 is optional for reporting.

```sh
make test
make sanitize
# Optional: Clang with libFuzzer, ASan, and UBSan runtimes
make fuzz
make build/benchmark
./build/benchmark 1000000 7 build/data > docs/benchmark-raw.csv 2> docs/benchmark-environment.txt
python3 bench/summarize.py docs/benchmark-raw.csv
```

## Implemented

- Versioned little-endian binary format: fixed-size row groups (65,536 rows by default, final group may be shorter), contiguous column chunks, footer with schema, offsets, encodings, counts, and per-chunk min/max.
- Plain fallback plus dictionary strings, integer/string RLE, and signed integer frame-of-reference bit packing. Sampling gates dictionary/RLE candidates; actual encoded sizes prevent expansion. Integer min/max determines packed width. Decisions are independent per column per row group.
- Zone maps skip groups whose ranges cannot satisfy equality or inclusive range predicates. AND scans intersect row selections across predicate columns and stop reading a group when no rows remain.
- Projected int64 sums over AND selections, with defined two's-complement wraparound and per-scan byte/group counters.
- Batches of up to 1,024 values, dictionary predicates evaluated through integer IDs, and RLE predicates evaluated once per run. Scalar C++ batch execution; no explicit SIMD intrinsics.
- Reader validation, full-column roundtrip decoding, scan counters, independent scalar-oracle correctness tests, ASan/UBSan, and a Linux/macOS CI workflow template.
- Deterministic benchmark against a fixed-width binary row store with identical rows and query results.

## API example

```cpp
#include "minicol/engine.hpp"
using namespace minicol;
write_file("example.mcol", {{"id", Type::Int64}},
           {std::vector<int64_t>{1, 2, 3, 4}});
Reader reader("example.mcol");
auto result = reader.scan({0, Op::Between, int64_t(2), int64_t(3)});
// result.matched == 2
auto both = reader.scan_all({
    {0, Op::Between, int64_t(2), int64_t(4)},
    {0, Op::Equal, int64_t(3), int64_t(0)}});
// both.matched == 1; predicates may also reference different columns
auto total = reader.sum_int64(0, {
    {0, Op::Between, int64_t(2), int64_t(4)}});
// total.sum == 9; total.scan.matched == 3
// An empty predicate list sums the entire projected column.
```

Compile with `c++ -std=c++17 -Iinclude src/engine.cpp your_program.cpp -o demo`.

For untrusted files, opt into input limits, for example
`Reader reader("example.mcol", ReadLimits{1048576, 16777216, 65536, 16777216});`.
These cap footer bytes, encoded bytes per chunk, actual rows per group, and
decoded string bytes per loaded chunk, respectively. An optional fifth field,
`max_payload_bytes_per_operation`, caps cumulative encoded chunk bytes loaded by
each read, count/AND scan, or sum call (excluding metadata). For example,
`ReadLimits{1048576, 16777216, 65536, 16777216, 33554432}` adds a 32 MiB
per-operation payload budget.
An optional sixth field, `max_decoded_rows_per_operation`, caps cumulative chunk
rows per decoding pass in a read/scan/sum call. Shared predicate/sum chunks count
twice because they are decoded twice; pruning avoids charges. For example,
`ReadLimits{1048576, 16777216, 65536, 16777216, 33554432, 1000000}` also
limits each call to one million logical row visits. Defaults retain all format-v1
dimensions. See
[resource limits and fuzzing](docs/FORMAT.md#resource-limits-and-fuzzing) for
validation boundaries and memory limitations.

## Evidence and boundaries

An hourly GitHub-hosted development worker is active and its first generation, validation, and commit-push run passed. Linux/macOS correctness CI also passed. The previous local hourly automation is paused. See [cloud runner setup, controls, and evidence](docs/CLOUD_RUNNER.md).

See [measured benchmarks](docs/BENCHMARKS.md), [raw trials](docs/benchmark-raw.csv), [format](docs/FORMAT.md), [Claude resume handoff](docs/CLAUDE_HANDOFF.md), and [roadmap](docs/ROADMAP.md).

This is an educational prototype. Supports non-null int64/string columns, single-column count predicates, AND count scans, and projected int64 sums. No SQL parser, joins, transactional updates, concurrent writers, checksums, or production durability guarantees. Writer accepts the full table in memory. Batch processing is not a claim of SIMD. Adaptive encoding is the implemented automatic physical optimization; there is no workload-driven reclustering. Benchmark bytes are logical file bytes requested, not physical disk I/O. Timings depend on this machine and synthetic workloads.
