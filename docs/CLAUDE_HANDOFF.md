# Claude resume handoff

Repository: https://github.com/bhinu/minicol (public).

Use the facts below to update my projects section for storage/database/systems roles. Keep the writing direct and specific. Produce two or three concise bullets. Do not invent usage, employers, production impact, concurrency, SQL, SIMD, or distributed systems features. This project was implemented with AI assistance; do not describe it as independently hand-coded. I should review the implementation and be able to explain it before using the bullets.

## Implemented facts

- C++17 columnar engine from scratch, with versioned binary files, 65,536-row groups, contiguous column chunks, schema/offset/encoding footer, and per-group min/max zone maps.
- Adaptive dictionary encoding for strings, integer/string RLE, signed integer frame-of-reference bit packing, and plain fallback; sampled candidate selection plus full-size fallback.
- Up to 1,024-value scan batches, single-column equality/range count predicates, dictionary-ID filtering without per-row string materialization, direct run filtering.
- 1,659 correctness checks covering scalar oracle agreement, all codecs, randomized roundtrips, int64 extremes, malformed files, and pruning; passing AddressSanitizer and UndefinedBehaviorSanitizer locally. Linux/macOS CI workflow supplied as a template; not active because the GitHub login lacks workflow permission.

## Measured evidence

One million synthetic rows; seven repetitions per method; optimized Apple clang 21, Darwin arm64, warm cache. Median elapsed times and comparisons to a naive fixed-width binary row store:

- Sorted-key range: 21.958 ms row store, 2.207 ms column scan without pruning, 0.172 ms with pruning. Skipped 15/16 row groups; 93.15% fewer requested column bytes (2,004,223 to 137,280).
- Low-cardinality string equality: 29.980 ms row store versus 1.522 ms column engine (19.7x). No groups skipped; benefits include projection and dictionary-ID evaluation.
- Small-integer equality: 21.976 ms row store versus 1.972 ms column engine (11.1x). No groups skipped.

These are narrow synthetic comparisons. Bytes are logical file bytes requested, not physical disk reads. Speedups combine layout, projection, encoding and execution choices. Favor the verifiable 93.15% byte reduction and 15/16 group pruning over an unqualified giant speedup.

## Suggested bullet material to adapt

- Built a C++17 columnar storage engine with adaptive dictionary/RLE/bit-packed encodings, binary row groups, and metadata-driven zone-map pruning.
- Implemented 1,024-value scan batches and encoded string predicates; skipped 15 of 16 row groups and reduced requested column bytes by 93% on a one-million-row sorted-range benchmark.
- Validated all codecs and scans with 1,659 correctness checks, scalar-oracle comparisons, and address/undefined-behavior sanitizers; benchmarked five workloads against a binary row-store baseline.

Full methodology: docs/BENCHMARKS.md. Raw data: docs/benchmark-raw.csv. Format and tradeoffs: docs/FORMAT.md.
