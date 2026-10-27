# Measured benchmarks

Measured 2026-10-27 on Darwin 27.0.0 arm64, Apple clang version 21.0.0 (clang-2100.3.34.2). CPU model and RAM were unavailable in this sandbox; no model-specific hardware claim is made. Optimized flags: `-O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic`. No explicit SIMD intrinsics.

## Reproduce

```sh
make build/benchmark
./build/benchmark 1000000 7 build/data > docs/benchmark-raw.csv 2> docs/benchmark-environment.txt
python3 bench/report.py
```

One million deterministic rows, `std::mt19937_64` seed 42; five columns: sorted unique key, uniform small integer in [0,1023], repetitive status changing every 4,096 rows, uniform 16-category strings, and 96 random uppercase payload characters. Row groups: 65,536 rows, 16 total. Data generation and writes are excluded from query timing. Both files contain identical values. An independent row-store count is checked against every measured result.

One untimed warmup per method/query; seven timed repetitions, rotating method order. Table uses medians. Timed column scans include Reader construction and footer parsing; all methods reopen files each trial. This is a warm-cache, single-machine synthetic microbenchmark; OS caches are not cleared and physical storage throughput is not measured.

| Query | Row store ms | Column ms, no zones | Column ms, zones | Speedup vs row | Bytes, zones | Groups skipped |
|---|---:|---:|---:|---:|---:|---:|
| sorted_key_range | 21.958 | 2.207 | 0.172 | 128.0x | 137,280 | 15 |
| small_integer_equality | 21.976 | 1.988 | 1.972 | 11.1x | 1,256,343 | 0 |
| repetitive_integer_equality | 21.961 | 0.077 | 0.056 | 393.0x | 9,139 | 0 |
| dictionary_equality | 29.980 | 1.637 | 1.522 | 19.7x | 4,009,943 | 0 |
| absent_dictionary_value | 28.606 | 1.616 | 0.032 | 905.7x | 6,199 | 16 |


## Accounting and interpretation

`bytes_read` counts logical file bytes requested: header/footer/trailer plus selected column chunks for minicol, full binary records for the row store. It is not a physical disk counter. The row store buffers 1,024 fixed-width 152-byte records per read, then evaluates each row. It includes padding in the fixed-width category field and materializes category strings; the column engine benefits from projection, compression, encoded comparisons, and pruning. These speedups do not isolate vectorization, do not compare against a production database, and are not general performance guarantees.

Sorted-key range selects exactly 10,000 rows. Zone maps skip 15/16 groups, reducing column bytes from 2,004,223 to 137,280 (93.15% fewer). RLE counts matching runs directly; its throughput represents logical rows examined, not individually decoded values. The absent-value query is metadata-only with zones, so its speedup should not headline a resume. Category and small-integer equality have no zone-map skipping on this data. Variation between zones on/off for those queries is timing noise and identical core work.

Generated file metadata:

```text
rows=1000000 row_store_bytes=152000000 column_store_bytes=107261051 metadata_bytes=6199 seed=42
plain_chunks=16
dictionary_chunks=16
rle_chunks=16
bitpacked_chunks=32
```

Full column file is 107,261,051 bytes versus the fixed-width row file's 152,000,000 bytes (29.43% smaller). This is a layout comparison that includes row padding and string length prefixes, not an isolated codec compression ratio. The high-entropy 96-byte payload dominates column-file size. [Raw CSV](benchmark-raw.csv) retains all 105 measured trials.

## Source fingerprints

- `src/engine.cpp` SHA-256: `579a6222b9a85c4782b32ba7ed17e35f65acb0f77d484b4061905c747f23f884`
- `bench/benchmark.cpp` SHA-256: `b2eecf858333f34727c364044b47ad1a9a907b2161ef19bec5bad97c4d89ea60`
