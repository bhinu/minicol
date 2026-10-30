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

`Reader(path, ReadLimits{max_footer_bytes, max_chunk_bytes, max_group_rows,
max_decoded_string_bytes, max_payload_bytes_per_operation,
max_decoded_rows_per_operation, max_metadata_entries,
max_predicates_per_operation, max_predicate_evaluations_per_operation,
max_string_comparison_bytes_per_operation,
max_decoded_string_bytes_per_operation})`
accepts optional, inclusive limits. It rejects an oversized footer before loading
its bytes, an oversized encoded chunk while parsing metadata (even if a later
scan would prune it), and oversized actual row groups before constructing schema
or group objects. The row limit applies to the largest actual group, not the
header's target size; an empty table satisfies a zero row/chunk limit. A zero
footer limit rejects every file. Limit violations throw `std::runtime_error`.
Limits do not alter persisted bytes or require a format-v1 version bump.

`max_decoded_string_bytes` is the inclusive sum of string byte lengths over all
rows of each loaded string chunk. NUL bytes count; length prefixes and string
object/allocation overhead do not. Dictionary entries count once per referencing
row, and RLE values count once per row in the run. Empty strings consume zero
bytes. The budget resets for each chunk and operation; it is not cumulative over
a column, Reader, or query. Existing three-field limit initializers keep an
unlimited decoded-string budget.

For a finite cap, a preflight parses string lengths, run lengths, and dictionary
IDs after loading encoded bytes but before constructing dictionary strings,
decoder batches, or materialized row strings. It checks repeated lengths against
the remaining budget without overflowing. Exceeding the cap throws
`std::runtime_error("decoded string byte limit exceeded")`. Construction still
validates metadata only; a string cap can therefore reject a later operation.
`read_column`, count/AND scans, and sum predicate scans apply the same cap to all
rows of a loaded chunk, even if few rows match or the codec avoids expansion.
Zone-pruned chunks, columns avoided by empty selections, and metadata-only scans
are not checked. Integer chunks are unaffected. Finite caps add a parsing pass;
dictionary preflight uses one uint32 length per dictionary entry. Defaults skip
this pass. This bounds logical string content, not process memory: encoded
buffers, metadata strings, unused dictionary entries, vector capacity, temporary
copies, and object overhead still need separate accounting. Total query work and
memory retained by callers across reads remain unbounded.

`max_decoded_string_bytes_per_operation` is an inclusive cumulative budget for
logical string content across loaded chunks in one `read_column`, `scan`,
`scan_all`, or `sum_int64` call. It uses the same row-expanded accounting as
`max_decoded_string_bytes`: sum each row's string byte length, including NULs
and repeated dictionary/RLE values, even when encoded scans avoid expansion.
Each distinct loaded predicate column in each visited group contributes once;
duplicate predicates on that column share the charge. Integer chunks, including
sum projections, contribute nothing. Metadata-only counts, unfiltered sums,
pruned chunks, and columns avoided by empty selections consume zero. Empty
strings consume zero. A zero budget permits those operations.

If either string-content cap is finite, preflight checks both after loading the
encoded payload and before constructing decoder strings or materialized rows.
For each string length/reference/run charge, check the per-chunk cap first,
then the operation cap, with division/subtraction to avoid overflow. An excess
of the operation cap throws
`std::runtime_error("operation decoded string byte limit exceeded")`; earlier
chunks may already have been decoded, but no partial result is returned. Row
and payload-byte charges, predicate/projection validation, and zone-map checks
retain their existing precedence. Malformed input may fail before exhaustion;
exhaustion may prevent later payload validation. The budget resets per call,
including after failure. Construction still validates metadata only.

The default `UINT64_MAX` disables cumulative accounting; existing ten-field
initializers remain unlimited. Format v1 is unchanged. This bounds logical
row-expanded string content over loaded chunks, not actual allocations, string
copies/comparisons, CPU time, or physical I/O. Unused dictionary entries, metadata
strings, buffers, object overhead, and caller-retained results still need separate
accounting. Predicate order and pruning can affect whether a call fits. Combine
this cap with the existing per-chunk/input/work limits for untrusted queries.

`max_payload_bytes_per_operation` is an inclusive cumulative budget for encoded
chunk bytes loaded during one `read_column`, `scan`, `scan_all`, or `sum_int64`
call. The budget resets on every call, including after a failure. Each chunk is
charged immediately before allocating/loading its payload, using subtraction
from the remaining budget to avoid overflow. Exceeding the budget throws
`std::runtime_error("operation payload byte limit exceeded")`; no partial result
is returned. Earlier chunks may already have been read and decoded. Reader
construction still checks metadata only and does not reject a table whose total
payload exceeds this operation budget.

Metadata bytes are excluded, although `ScanResult::bytes_read` still includes
metadata once. Pruned chunks and chunks avoided by an empty selection consume
no budget. Repeated predicates and a shared predicate/projection chunk consume
encoded bytes once per group, including when retained bytes are decoded again
for summation. An empty conjunction count consumes zero; an unfiltered sum
charges its projected chunks. `read_column` charges its single chunk. A zero
budget therefore permits metadata-only operations and fully pruned scans.
Predicate and projection validation still precedes payload loading. Predicate
order may determine whether a call fits the budget through early termination.

This cap bounds logical encoded payload bytes requested per operation, not
physical disk reads, decoded row visits, elapsed time, or process memory. Tiny
RLE/packed payloads can represent many rows; combine this budget with group-row
and decoded-string limits for untrusted files. Footer parsing, predicate-list
processing, repeated decoding, metadata traversal, allocation overhead, and
caller-retained results need separate budgets. Existing four-field limit
initializers retain an unlimited per-operation payload budget.

`max_decoded_rows_per_operation` is an inclusive cumulative budget for logical
chunk rows per decoding pass in each `read_column`, `scan`, `scan_all`, or
`sum_int64` call. Before loading a chunk for decoding, its full row count is
subtracted from the remaining budget, with an overflow-safe check. An excess
throws `std::runtime_error("operation decoded row limit exceeded")` before that
payload allocation/read. For a retained shared predicate/projection chunk, a
second row charge occurs before the summation decoding pass, without another
payload-byte charge or file read. Earlier chunks may already have been decoded;
no partial result is returned. Budgets reset on each call, including after a
failure. Reader construction still validates metadata only.

Each pass charges every row in the chunk, including unselected values and
logical rows represented by dictionary IDs, RLE runs, or zero-width packing.
Even an encoded RLE count charges the full represented row count. Repeated
predicates on one column share one pass. Unfiltered sums charge the projected
chunks once; shared predicate/sum projections charge twice when rows survive.
Pruned chunks and chunks avoided by an empty selection consume no budget.
Metadata-only counts and empty tables consume zero, so a zero budget permits
them. All predicate/projection validation precedes charging. Row charges precede
payload-byte charges and string-budget preflight; an exhausted row budget can
therefore reject a malformed chunk without validating its payload. Predicate
order and zone maps may affect whether an operation fits.

This is a conservative logical row-work limit, not an instruction, elapsed-time,
or process-memory limit. String preflight parsing is not a separate row charge;
dictionary construction, string comparisons, predicate-list processing,
selection intersection, metadata traversal, and per-row costs remain outside
this accounting. Combine it with the byte, string, and group limits for
untrusted inputs. Existing five-field initializers retain an unlimited row
budget. Persisted format v1 is unchanged.

`max_metadata_entries` is an inclusive construction-time cap on schema fields
plus chunk metadata entries: `column_count * (group_count + 1)`. A schema field
counts once, even for an empty table; each column in each group counts once.
Group containers are not charged separately, but their count cannot exceed the
chunk count because schemas are nonempty. Counts come from uint32 format fields;
the calculation uses uint64 arithmetic and cannot overflow at their maxima.
Zero rejects every file, including empty tables, because a schema is required.

After loading the footer and validating its schema/group dimensions and group-row
limit, construction checks this cap before parsing schema names or constructing
schema/group/chunk objects. Exceeding it throws
`std::runtime_error("metadata entry limit exceeded")`. This may precede errors
in schema names or later metadata; satisfying the cap does not bypass any
structural validation. All entries count even if later queries would prune their
groups or never reference their columns. The cap is per Reader construction,
does not charge queries, and does not change persisted format v1. Existing
six-field initializers keep an unlimited metadata-entry cap.

This bounds metadata object counts and the number of chunks available to query
traversal, not exact allocations, string bytes, predicate-list work, or CPU time.
The footer buffer has already been loaded; combine this cap with
`max_footer_bytes` to constrain encoded metadata and copied names/zone strings.
Vector capacity, object overhead, temporary copies, and caller-retained Readers
still prevent an exact process-memory guarantee.

`max_predicates_per_operation` is an inclusive cap on the supplied predicate
count in each `scan`, `scan_all`, or `sum_int64` call. Every predicate counts,
including duplicates, repeated columns, contradictions, and conditions that
could prune every group. A single `scan` counts one. Before validating predicate
columns/operations/types, constructing grouping vectors, opening the payload
file, or traversing groups, an excess throws
`std::runtime_error("operation predicate limit exceeded")`. Sum projection
validation still happens first. Within the cap, all predicates retain their
normal validation before pruning. The cap applies to empty tables too.

Zero permits metadata-only empty conjunction counts and unfiltered sums;
`read_column` is unaffected. The cap is per call, with no cumulative Reader
state, including after failures. It does not bound caller allocations for the
predicate vector/strings or the temporary single-predicate copy made by `scan`.
It bounds list length and internal grouping work, not string comparison bytes,
total comparisons across rows/groups, elapsed time, or process memory. Combine
it with the other limits for untrusted queries. Existing seven-field initializers
keep an unlimited predicate cap; format v1 is unchanged.

`max_predicate_evaluations_per_operation` is an inclusive cumulative budget for
predicate evaluations in one `scan`, `scan_all`, or `sum_int64` call. Each
zone-map predicate check costs one, including checks that prune a group. Payload
filtering costs one per predicate actually tested against a plain/packed row,
dictionary entry (including unused entries), or RLE run. A range test costs one,
even though it can compare both bounds. Duplicates count separately. Same-column
AND evaluation stops at the first false predicate, and zone-map checks stop at
the first excluded predicate; predicates bypassed this way cost nothing.
Evaluations of later columns avoided by an empty selection also cost nothing.
Predicate order, encoding, and enabling zone maps can therefore change the budget
needed, including when the query's result is unchanged.

Immediately before each evaluation, an exhausted budget throws
`std::runtime_error("operation predicate evaluation limit exceeded")`; no partial
result is returned. Earlier payloads may have been read/decoded. Budgets reset
on every call, including after failures. All predicate/projection validation and
the predicate-count check still precede evaluation. Zone checks occur before
payload work; payload evaluations occur after the applicable row/payload/string
budget checks and decoder setup. This is not a payload preflight: malformed
payloads may fail before budget exhaustion, and exhaustion may prevent later
structural validation. Default unlimited calls retain their validation behavior.

An empty table, empty conjunction count, unfiltered sum, and `read_column` use
no evaluations. Summation's projection pass uses none, including a retained
predicate/projection chunk decoded again. A zero budget permits these operations,
but rejects even a fully pruned nonempty scan at its first zone-map check.
Existing eight-field initializers retain an unlimited evaluation budget; persisted
format v1 is unchanged.

This bounds predicate checks, not individual comparisons, string bytes compared,
CPU instructions, elapsed time, or memory. Decoding, dictionary construction,
metadata traversal, selection intersection, and aggregation need separate
accounting. Combine this cap with the existing input and operation limits;
a small evaluation budget alone does not bound payload allocation or expansion.

`max_string_comparison_bytes_per_operation` is an inclusive cumulative budget
for conservative string byte positions compared by one `scan`, `scan_all`, or
`sum_int64` call. Each string ordering comparison charges `min(a.size(), b.size())`;
equality charges that length when sizes match and zero otherwise. One byte
position means a corresponding byte in each operand, not two physical bytes
read. The entire charge occurs before invoking the standard string comparison,
even if the first byte differs. This deliberately bounds potential comparison
work without counting the library's actual byte reads. NUL and high-bit bytes
count like other bytes, and existing `std::string` ordering is preserved.

The budget includes both payload predicates and zone maps. Payload equality
compares the value to the low bound; ranges compare `value < low`, then
`high < value` only if needed. Zone equality compares `low < min`, then
`max < low`; zone ranges compare `high < min`, `max < low`, then `high < low`.
These zone comparisons stop at the first true result. Payload predicates stop
at the first false result. Each comparison reached is charged separately,
including repeated predicates and the two or three comparisons within a zone
check. Dictionary entries (including unused entries) and RLE runs are checked
once per predicate reached, while plain strings are checked per row. Skipped
predicates, pruned payloads, and later columns avoided by empty selections cost
nothing. Encoding, predicate order, and zone-map use can change the required
budget without changing query results.

Overflow-safe subtraction checks each charge against the remaining budget.
An excess throws
`std::runtime_error("operation string comparison byte limit exceeded")` without
a partial result. The budget resets per call, including after failures. Predicate
count and all predicate/projection validation precede comparisons; each
predicate's evaluation budget is charged before its string comparisons. Payload
comparisons follow loading, applicable decoding budgets, and decoder setup.
Earlier payloads may already have been read/decoded; budget exhaustion may
prevent later structural validation. Construction-time metadata validation,
writer comparisons, `read_column`, empty conjunction counts, unfiltered sums,
and integer comparisons consume none of this budget. Empty strings and
unequal-length payload equalities can therefore succeed with zero. Zone maps
still perform ordering comparisons even for equality predicates.

The default `UINT64_MAX` disables this accounting, and existing nine-field
initializers remain unlimited. Format v1 is unchanged. This is a conservative
logical work cap, not a measurement of actual comparison bytes, physical memory
reads, CPU instructions, elapsed time, or allocations. It does not cover string
copying/decoding, dictionary construction, metadata validation, selection
intersection, or aggregation; combine it with the existing limits.

Defaults are `UINT64_MAX`, `UINT64_MAX`, `UINT32_MAX`, `UINT64_MAX`, `UINT64_MAX`,
`UINT64_MAX`, `UINT64_MAX`, `UINT64_MAX`, `UINT64_MAX`, and `UINT64_MAX`:
existing callers retain all format-v1 dimensions, subject to file-bound checks, address space, stream
limits, and available resources. No small input or process-memory budget is
imposed by default. Callers handling untrusted files should choose explicit
limits; for example `{1048576, 16777216, 65536, 16777216}` caps the footer at
1 MiB, each encoded chunk at 16 MiB, each actual group at 65,536 rows, and
decoded string content per loaded chunk at 16 MiB. These are input
dimension caps, not an exact allocation or runtime budget. Metadata allocations
include all schema/group objects and copied strings in addition to the footer
buffer. Without a decoded-string cap, dictionary and RLE strings can expand on
materialization; decoded string storage can scale with group rows times encoded
chunk bytes. Count/sum scans can visit all groups, and there is no construction-time
total-row, total-file, or elapsed-time cap. The optional per-operation decoded-row
budget does not limit metadata traversal.
The reader still validates payloads only when loaded and assumes immutable files.

`make fuzz` optionally requires Clang's libFuzzer, ASan, and UBSan runtimes; the
engine and regular tests still need only dependency-free C++17. `FUZZ_CXX` can
select a compatible compiler. The target generates 74 seed files in
`build/fuzz-corpus` covering every codec/type combination, zero-width packing,
empty tables, multiple columns, group tails, NUL strings, and signed extremes.
These include 65 independently serialized packed chunks, one for each width
from 0 through 64, with nine rows to exercise byte-crossing tails. Width 64 is
legal to read even though the adaptive writer chooses plain storage instead.
The regular release/sanitizer suite uses the same test-only bit-by-bit serializer
with independent scalar query oracles, signed-edge bases, and row counts around
the 1,024-value batch boundary. Every truncated payload prefix of the nine-row
fixtures (with intact metadata), trailing payload bytes, and all invalid width
tags are checked too.
`make fuzz` runs 1,000 inputs with seed 42 and a five-second per-input timeout. Corpus
and failure artifacts stay under `build/`; no binary corpus is checked in.
For a longer run after building:

```sh
./build/fuzz_reader build/fuzz-corpus -seed=42 -runs=100000 -max_len=65536 -timeout=5 -artifact_prefix=build/fuzz-artifacts/
```

The harness caps input at 65,536 bytes, footer/chunk bytes at 4,096 each,
actual group rows at 64, decoded string bytes per chunk at 8,192, and combined
schema/chunk metadata entries at 128 before constructing those objects. It also
sets a 1 MiB per-operation decoded-string budget; accepted inputs fit that cap
because there are at most 128 chunks of at most 8,192 logical string bytes each.
After metadata validation it exercises payloads only for tables with at most 256 total rows and four columns. Expected parse/decode
errors are rejected inputs; allocation failures and sanitizer errors remain
failures. Accepted payloads are materialized and checked against independent
row-wise scalar count/AND/sum oracles, including modulo-2^64 sums. Unpruned scans
are always checked. Pruned scans are checked only when decoded values fit every
persisted zone bound: format v1 cannot detect arbitrary false bounds. The smoke
is bounded coverage, not proof of malformed-input safety or a memory budget.
If LeakSanitizer cannot run under a tracing environment, use
`ASAN_OPTIONS=detect_leaks=0 make fuzz` (and the same prefix for `make sanitize`);
address and undefined-behavior checks remain enabled, but leaks are unverified.
