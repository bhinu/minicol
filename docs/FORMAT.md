# MiniCol binary format v1

All integer fields are little endian; signed int64 values use two's complement bits.
Strings are a uint32 byte length followed by bytes, without a terminator. Strings
may contain NUL and are compared lexicographically as `std::string` values.
There are no nulls, floating point values, checksums, compression libraries,
updates, transactions, or concurrent writer support. Files are immutable after
writing. A failed write may leave a partial destination file.

## Layout

```
16-byte header | row group 0 chunks | row group 1 chunks | ... | footer | 16-byte trailer
```

The header is ASCII `MINICOL1`, uint32 version (`1`), uint32 target row group
size. All groups have that many rows except the last. Each group's columns are
stored contiguously in schema order, with no padding. Chunk lengths describe
encoded bytes only.

The trailer is uint64 footer byte length and ASCII `MCOLEND1`.

The footer contains:

1. uint64 total rows, uint32 column count, uint32 row group count.
2. For every schema field: string name, uint8 type (`0` int64, `1` string).
3. For every group, then every column: uint32 rows, uint8 encoding, uint64 chunk
   offset, uint64 chunk byte length, typed minimum, typed maximum. Int64 zone
   bounds are eight bytes each; string zone bounds use length-prefixed strings.

An empty table retains a nonempty schema and has zero groups. Columns must have
identical row counts. The final chunk ends exactly where the footer begins.
Reader construction validates the header, version, trailer, footer dimensions,
types, offsets, lengths, group row counts, and ordered zone bounds. Payload
structure is checked when a chunk is read. There is no checksum: arbitrary
payload modification or falsified but structurally valid zone bounds cannot be
detected reliably. Files must not be modified while a Reader is in use.

## Encodings

| Tag | Encoding | Payload |
|---|---|---|
| 0 | Plain | Consecutive typed values |
| 1 | Dictionary (strings) | uint32 dictionary size; dictionary strings in first-occurrence order; one uint32 dictionary ID per row |
| 2 | Run length | Repeated `(uint32 run length, typed value)` pairs, consuming exactly the chunk row count |
| 3 | Bit packed (int64) | int64 base (minimum), uint8 bit width (0–64), packed unsigned deltas from base |

Packed deltas are stored least-significant-bit first, with consecutive values
sharing byte boundaries. Width zero represents a constant; bytes are rounded
up and padding bits are zero on write. Unsigned modular subtraction/addition
handles the full signed int64 range. A full-domain span falls back to plain
storage because a 64-bit delta plus its header would be larger.

## Physical selection

The writer samples up to 2,048 evenly spaced values by default, separately per
column and group. Adjacent pairs at sampled positions estimate run density;
fewer than half changing pairs enables RLE as a candidate. At most half as many
sampled distinct strings as sample rows enables a dictionary candidate. A
sampled integer span requiring fewer than 64 bits enables a bitpacking
candidate. The full group determines actual dictionaries, runs, and exact bit
width. Each candidate is accepted only if its actual payload is smaller than
the current best; plain is the fallback. This guards against misleading samples,
but the heuristic is not an exhaustive optimal encoder search. The chosen
encoding and exact zone bounds are persisted in the footer.

## Scan semantics and accounting

`scan` evaluates one typed equality or inclusive range predicate and returns a
count. `scan_all` evaluates an AND of such predicates, using sorted group-local
uint32 row offsets as internal selection vectors. A reversed range is empty.
An empty predicate list matches all rows using metadata alone, with zero groups
read/skipped and zero batches. All predicates are type/operation/column checked
before any pruning, including predicates after an impossible condition. Equality
uses only the low bound; its high bound is ignored.

AND scans read only predicate columns, in order of first appearance. Predicates
on the same column are evaluated together, so each chunk is read at most once.
With zone maps enabled, any excluded predicate skips the entire group before
payload reads. Otherwise, selections are intersected across columns; an empty
selection avoids reading later columns. Each loaded chunk is structurally
validated; chunks skipped by zone maps or an empty selection are not payload-validated.
Dictionary entries and RLE run values are tested against all predicates for that
column once per entry/run. Multi-column selections use memory proportional to
one row group's rows, in addition to the existing chunk decoder allocations;
this is not a bounded-memory streaming reader.

`groups_read` counts groups with any payload read; `groups_skipped` counts groups
excluded by zone maps. Logical batches count each visited group's rows once,
regardless of the number of predicate columns actually read. Predicate order can
affect bytes read through early termination, but does not affect the count.

String predicates evaluate each dictionary entry once and then filter integer IDs;
matching rows do not materialize strings. RLE scans evaluate each run directly.
Plain and packed chunks fill batches of up to 1,024 values; dictionary chunks
fill batches of IDs. This is batched scalar C++, not a claim of explicit SIMD.
RLE scans exploit runs without expanding batches; the batch counter records
logical 1,024-row batches in every visited group.

`bytes_read` is logical bytes requested: header, entire footer and trailer once,
plus encoded predicate chunk bytes actually read by that scan. It does not
measure device I/O or cache misses. Reader construction loads metadata before `scan`; the
counter includes those bytes even when a benchmark reuses a Reader. Payloads
are loaded from the file anew for each scan. `read_column` reconstructs a full
column chunk and is intended for round-trip checks and consumers requiring
materialized data.

## Projected integer sum

`sum_int64(column, predicates, zone_maps)` returns `SumResult`, containing
`int64_t sum` and `ScanResult scan`. It sums the projected int64 column over
the rows satisfying the same AND predicates as `scan_all`; `scan.matched` is
the number of selected rows. An empty predicate list sums every row. An empty
table or selection returns zero. String projections and out-of-range columns
are rejected, and all predicates are validated before pruning.

Addition is defined modulo 2^64, with the resulting bits interpreted as a signed
two's-complement int64. For example, `INT64_MAX + 1` yields `INT64_MIN`, and
`INT64_MIN - 1` yields `INT64_MAX`. Intermediate overflow does not throw or
saturate; the implementation accumulates unsigned values without signed overflow.
This contract applies across row-group boundaries and is independent of
predicate order. The on-disk format remains v1.

The sum uses group-local AND selections. Predicate chunks are read in the same
order as count scans, and the projected chunk is read only if rows survive
(or if there are no predicates). When projection is also a predicate column,
its encoded bytes are retained and decoded again for summation, without another
file read. Every loaded chunk is structurally validated, including unselected
values within a loaded projection. Skipped chunks are not payload-validated.
Summation currently decodes the entire projected chunk, including RLE expansion
through the scalar consumer; this is not a performance claim or a streaming
aggregate implementation.

`scan.bytes_read` includes metadata once and each loaded predicate/projected
chunk once. It measures logical requested bytes, not physical reads. Group and
logical batch counters follow count-scan semantics, counting each visited group
once even when its selection is empty. An unfiltered sum visits every group and
reads only the projected column; an empty table visits no groups. Memory includes
row-group selections, decoder allocations, and one retained encoded projected
chunk in addition to the currently loaded predicate chunk.

## Resource limits and fuzzing

`Reader(path, ReadLimits{max_footer_bytes, max_chunk_bytes, max_group_rows})`
accepts optional, inclusive limits. It rejects an oversized footer before loading
its bytes, an oversized encoded chunk while parsing metadata (even if a later
scan would prune it), and oversized actual row groups before constructing schema
or group objects. The row limit applies to the largest actual group, not the
header's target size; an empty table satisfies a zero row/chunk limit. A zero
footer limit rejects every file. Limit violations throw `std::runtime_error`.
Limits do not alter persisted bytes or require a format-v1 version bump.

Defaults are `UINT64_MAX`, `UINT64_MAX`, and `UINT32_MAX`: existing callers retain
all format-v1 dimensions, subject to file-bound checks, address space, stream
limits, and available resources. No small input or process-memory budget is
imposed by default. Callers handling untrusted files should choose explicit
limits; for example `{1048576, 16777216, 65536}` caps the footer at 1 MiB, each
encoded chunk at 16 MiB, and each actual group at 65,536 rows. These are input
dimension caps, not an exact allocation or runtime budget. Metadata allocations
include all schema/group objects and copied strings in addition to the footer
buffer. Dictionary and RLE strings can expand on materialization; decoded string
storage can scale with group rows times encoded chunk bytes. Count/sum scans can
visit all groups, and there is no total-row, total-file, or elapsed-time cap.
The reader still validates payloads only when loaded and assumes immutable files.

`make fuzz` optionally requires Clang's libFuzzer, ASan, and UBSan runtimes; the
engine and regular tests still need only dependency-free C++17. `FUZZ_CXX` can
select a compatible compiler. The target generates nine seed files in
`build/fuzz-corpus` covering every codec/type combination, zero-width packing,
empty tables, multiple columns, group tails, NUL strings, and signed extremes.
It runs 1,000 inputs with seed 42 and a five-second per-input timeout. Corpus
and failure artifacts stay under `build/`; no binary corpus is checked in.
For a longer run after building:

```sh
./build/fuzz_reader build/fuzz-corpus -seed=42 -runs=100000 -max_len=65536 -timeout=5 -artifact_prefix=build/fuzz-artifacts/
```

The harness caps input at 65,536 bytes, footer/chunk bytes at 4,096 each, and
actual group rows at 64. After metadata validation it exercises payloads only
for tables with at most 256 total rows and four columns. Expected parse/decode
errors are rejected inputs; allocation failures and sanitizer errors remain
failures. Accepted payloads are materialized and checked against independent
row-wise scalar count/AND/sum oracles, including modulo-2^64 sums. Unpruned scans
are always checked. Pruned scans are checked only when decoded values fit every
persisted zone bound: format v1 cannot detect arbitrary false bounds. The smoke
is bounded coverage, not proof of malformed-input safety or a memory budget.
If LeakSanitizer cannot run under a tracing environment, use
`ASAN_OPTIONS=detect_leaks=0 make fuzz` (and the same prefix for `make sanitize`);
address and undefined-behavior checks remain enabled, but leaks are unverified.
