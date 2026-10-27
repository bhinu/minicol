#!/usr/bin/env python3
"""Build checked-in evidence documents from benchmark raw trials."""
import csv
import hashlib
import pathlib
import statistics
import subprocess
from collections import defaultdict
root = pathlib.Path(__file__).resolve().parents[1]
raw = root / 'docs/benchmark-raw.csv'
groups = defaultdict(list)
with raw.open(newline='') as f:
    for r in csv.DictReader(f):
        groups[r['query'], r['mode']].append(r)
def ms(query, mode):
    return statistics.median(float(r['elapsed_ms']) for r in groups[query, mode])
def speed(query):
    return ms(query, 'row_store') / ms(query, 'column_zones')
table = subprocess.check_output(['python3', str(root / 'bench/summarize.py'), str(raw)], text=True)
compiler = subprocess.check_output(['c++', '--version'], text=True).splitlines()[0]
platform = subprocess.check_output(['uname', '-srm'], text=True).strip()
meta = (root / 'docs/benchmark-environment.txt').read_text()
fingerprint = '\n'.join(f'- `{p}` SHA-256: `{hashlib.sha256((root/p).read_bytes()).hexdigest()}`' for p in ['src/engine.cpp', 'bench/benchmark.cpp'])
report = f'''# Measured benchmarks

Measured 2026-10-27 on {platform}, {compiler}. CPU model and RAM were unavailable in this sandbox; no model-specific hardware claim is made. Optimized flags: `-O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic`. No explicit SIMD intrinsics.

## Reproduce

```sh
make build/benchmark
./build/benchmark 1000000 7 build/data > docs/benchmark-raw.csv 2> docs/benchmark-environment.txt
python3 bench/report.py
```

One million deterministic rows, `std::mt19937_64` seed 42; five columns: sorted unique key, uniform small integer in [0,1023], repetitive status changing every 4,096 rows, uniform 16-category strings, and 96 random uppercase payload characters. Row groups: 65,536 rows, 16 total. Data generation and writes are excluded from query timing. Both files contain identical values. An independent row-store count is checked against every measured result.

One untimed warmup per method/query; seven timed repetitions, rotating method order. Table uses medians. Timed column scans include Reader construction and footer parsing; all methods reopen files each trial. This is a warm-cache, single-machine synthetic microbenchmark; OS caches are not cleared and physical storage throughput is not measured.

{table}

## Accounting and interpretation

`bytes_read` counts logical file bytes requested: header/footer/trailer plus selected column chunks for minicol, full binary records for the row store. It is not a physical disk counter. The row store buffers 1,024 fixed-width 152-byte records per read, then evaluates each row. It includes padding in the fixed-width category field and materializes category strings; the column engine benefits from projection, compression, encoded comparisons, and pruning. These speedups do not isolate vectorization, do not compare against a production database, and are not general performance guarantees.

Sorted-key range selects exactly 10,000 rows. Zone maps skip 15/16 groups, reducing column bytes from 2,004,223 to 137,280 (93.15% fewer). RLE counts matching runs directly; its throughput represents logical rows examined, not individually decoded values. The absent-value query is metadata-only with zones, so its speedup should not headline a resume. Category and small-integer equality have no zone-map skipping on this data. Variation between zones on/off for those queries is timing noise and identical core work.

Generated file metadata:

```text
{meta.strip()}
```

Full column file is 107,261,051 bytes versus the fixed-width row file's 152,000,000 bytes (29.43% smaller). This is a layout comparison that includes row padding and string length prefixes, not an isolated codec compression ratio. The high-entropy 96-byte payload dominates column-file size. [Raw CSV](benchmark-raw.csv) retains all 105 measured trials.

## Source fingerprints

{fingerprint}
'''
(root/'docs/BENCHMARKS.md').write_text(report)
handoff = f'''# Claude resume handoff

Repository: https://github.com/bhinu/minicol (private; share access or paste this packet).

Use the facts below to update my projects section for storage/database/systems roles. Keep the writing direct and specific. Produce two or three concise bullets. Do not invent usage, employers, production impact, concurrency, SQL, SIMD, or distributed systems features. This project was implemented with AI assistance; do not describe it as independently hand-coded. I should review the implementation and be able to explain it before using the bullets.

## Implemented facts

- C++17 columnar engine from scratch, with versioned binary files, 65,536-row groups, contiguous column chunks, schema/offset/encoding footer, and per-group min/max zone maps.
- Adaptive dictionary encoding for strings, integer/string RLE, signed integer frame-of-reference bit packing, and plain fallback; sampled candidate selection plus full-size fallback.
- Up to 1,024-value scan batches, single-column equality/range count predicates, dictionary-ID filtering without per-row string materialization, direct run filtering.
- 1,659 correctness checks covering scalar oracle agreement, all codecs, randomized roundtrips, int64 extremes, malformed files, and pruning; passing AddressSanitizer and UndefinedBehaviorSanitizer locally. Linux/macOS CI workflow supplied as a template; not active because the GitHub login lacks workflow permission.

## Measured evidence

One million synthetic rows; seven repetitions per method; optimized Apple clang 21, Darwin arm64, warm cache. Median elapsed times and comparisons to a naive fixed-width binary row store:

- Sorted-key range: {ms('sorted_key_range','row_store'):.3f} ms row store, {ms('sorted_key_range','column_no_zones'):.3f} ms column scan without pruning, {ms('sorted_key_range','column_zones'):.3f} ms with pruning. Skipped 15/16 row groups; 93.15% fewer requested column bytes (2,004,223 to 137,280).
- Low-cardinality string equality: {ms('dictionary_equality','row_store'):.3f} ms row store versus {ms('dictionary_equality','column_zones'):.3f} ms column engine ({speed('dictionary_equality'):.1f}x). No groups skipped; benefits include projection and dictionary-ID evaluation.
- Small-integer equality: {ms('small_integer_equality','row_store'):.3f} ms row store versus {ms('small_integer_equality','column_zones'):.3f} ms column engine ({speed('small_integer_equality'):.1f}x). No groups skipped.

These are narrow synthetic comparisons. Bytes are logical file bytes requested, not physical disk reads. Speedups combine layout, projection, encoding and execution choices. Favor the verifiable 93.15% byte reduction and 15/16 group pruning over an unqualified giant speedup.

## Suggested bullet material to adapt

- Built a C++17 columnar storage engine with adaptive dictionary/RLE/bit-packed encodings, binary row groups, and metadata-driven zone-map pruning.
- Implemented 1,024-value scan batches and encoded string predicates; skipped 15 of 16 row groups and reduced requested column bytes by 93% on a one-million-row sorted-range benchmark.
- Validated all codecs and scans with 1,659 correctness checks, scalar-oracle comparisons, and address/undefined-behavior sanitizers; benchmarked five workloads against a binary row-store baseline.

Full methodology: docs/BENCHMARKS.md. Raw data: docs/benchmark-raw.csv. Format and tradeoffs: docs/FORMAT.md.
'''
(root/'docs/CLAUDE_HANDOFF.md').write_text(handoff)
print(table)
