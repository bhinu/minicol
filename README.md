# minicol

A small columnar storage engine in C++17. It writes tables to a binary file, chooses an encoding for each column chunk, and answers filtered counts and sums without reading every column.

## Why this project

I wanted to work through a database question that is easy to overlook when using one: **how much of a query can be answered by choosing what not to read?**

If a table contains a date, a region, and a large text payload, counting rows in a date range should not require loading all that text. Storing values by column makes that possible. Keeping a little metadata about each block lets the reader skip some blocks entirely. Repeated values create another opportunity: a filter can sometimes work on dictionary IDs or runs instead of reconstructing every original value.

Minicol puts those ideas in one C++ project, from the bytes on disk to the scan loop. The scope stays small enough to follow the whole path. It supports integer and string columns, equality and range filters, AND conditions, and integer sums.

## Try it

You need a C++17 compiler and Make. There are no database or compression-library dependencies.

```sh
git clone https://github.com/bhinu/minicol.git
cd minicol
make test
make demo
```

The [demo](examples/query.cpp) writes six rows, reopens the file, filters for days 3–4 in the `west` region, and sums the matching amounts:

```text
Matched rows: 2
Amount sum: 70
Groups skipped: 2 / 3
```

It uses two-row groups to make skipping visible. The default group size is 65,536 rows. `make sanitize` runs the correctness suite with AddressSanitizer and UndefinedBehaviorSanitizer. `make fuzz` is an optional bounded fuzz run and requires Clang with libFuzzer support.

## Following a query through the file

The file is divided into row groups. Within each group, each column has its own contiguous chunk:

```text
header
  group 0: [day chunk] [region chunk] [amount chunk]
  group 1: [day chunk] [region chunk] [amount chunk]
  ...
footer: schema, chunk offsets, encodings, row counts, min/max values
trailer: location information for the footer
```

For the demo's count query, the reader:

1. Reads the footer to locate columns and inspect each group's min/max values.
2. Skips groups whose day range cannot overlap 3–4.
3. Reads the needed filter columns in surviving groups and intersects their matching row offsets. These offsets are the selection vector used for AND filters.
4. Counts the surviving rows. For the sum query, it also reads the amount column for groups with matches.

The sum currently decodes the whole amount chunk in each surviving group, then uses the selection to pick values. The reader can reuse loaded bytes when a column is both filtered and summed, but it still decodes those bytes again for the sum.

## Why these design choices

| Choice | Reason | Tradeoff |
|---|---|---|
| Column chunks inside fixed-size row groups | Read only the columns a query needs, while bounding the number of rows handled together. | Group size affects metadata overhead and how precisely data can be skipped; the default is a starting point, not a tuned optimum. |
| Metadata in a footer | One place describes the schema and every chunk; the writer can record offsets after writing data. | Opening a file reads its metadata up front. Files are immutable after writing. |
| Encoding chosen per column, per group | Different regions of the same column can have different repetition and value ranges. | Sampling can miss patterns. Candidate sizes are checked on the full chunk, with plain storage as a fallback. |
| Min/max zone maps | A small index can rule out a whole group before decoding it. | Works best when values are clustered. Randomly distributed values may leave every group eligible. |
| Batches of up to 1,024 values | Give scans a bounded unit of work instead of a separate operator call for each row. | This is scalar batch execution; there are no explicit SIMD intrinsics. |
| Selection vectors for AND filters | Keep matching row positions and stop reading later columns when no rows remain. | Extra temporary memory and intersection work; predicate order is not cost-optimized. |

The encodings target three different patterns:

- **Dictionary:** Store distinct strings once and use integer IDs per row. Equality filters compare IDs after evaluating dictionary entries.
- **Run length:** Store a value and its repetition count. Filters can evaluate a run once rather than compare every repeated value.
- **Bit packing:** Store an integer minimum plus small unsigned differences. A narrow value range needs fewer bits per value; the full signed integer domain is handled without signed-overflow arithmetic.

There is no single encoding that wins everywhere. Adaptation here means choosing a representation locally; the engine does not automatically sort or reorganize a workload.

## What the measurements show

The initial benchmark used **1 million synthetic rows**, seven trials per method, and a fixed-width binary row-store baseline containing the same values. These are median warm-cache timings from the October 27 engine revision:

| Query | Row store | Column scan, no zone maps | Column scan, zone maps |
|---|---:|---:|---:|
| Sorted-key range | 21.958 ms | 2.207 ms | 0.172 ms |
| Small-integer equality | 21.976 ms | 1.988 ms | 1.972 ms |
| Dictionary string equality | 29.980 ms | 1.637 ms | 1.522 ms |

The clearest result is the sorted-range scan: zone maps skipped **15 of 16 groups**, reducing requested column bytes from **2,004,223 to 137,280 — 93.15% fewer**. The other two queries skipped no groups, which shows why data distribution matters.

The row-store comparison combines column projection, encoding, and execution differences. It does not isolate batching or compare against a production database. Bytes are logical file bytes requested, not measured physical disk reads. Later changes have correctness smoke benchmarks; these original timings are historical evidence, not a fresh performance claim for the current revision.

[Methodology and all five workloads](docs/BENCHMARKS.md) · [Raw trials](docs/benchmark-raw.csv)

To run your own benchmark without replacing the original evidence:

```sh
make build/benchmark
./build/benchmark 1000000 7 build/data > build/benchmark.csv 2> build/environment.txt
python3 bench/summarize.py build/benchmark.csv
```

## Checking the hard parts

A codec roundtrip alone is not enough: a writer and reader could share the same mistake. The tests also compare scans and sums with independent row-wise oracles and use independently serialized bit-packed fixtures covering widths 0 through 64.

The current suite passes **251,007 checks** across encodings, AND filters, sums, integer extremes, embedded NUL strings, partial groups, pruning, and malformed files. That is a count of checks, not 251,007 separate test cases. Linux/macOS correctness CI and sanitizer runs provide additional coverage. The fuzz harness compares accepted inputs against scalar oracles; the latest recorded campaign exercised 10,000 inputs, which is bounded coverage rather than a safety guarantee.

Malformed metadata and compact encodings can advertise much more work than their file size suggests. Optional `ReadLimits` cap input dimensions, decoded content, and parts of query work. For example:

```cpp
minicol::ReadLimits limits;
limits.max_footer_bytes = 1024 * 1024;
limits.max_group_rows = 65536;
limits.max_payload_bytes_per_operation = 32 * 1024 * 1024;
minicol::Reader reader("table.mcol", limits);
```

Defaults are unlimited; these caps are not an exact process-memory or elapsed-time guarantee. [Format and resource-limit details](docs/FORMAT.md#resource-limits-and-fuzzing) explain their scope.

## Where it stops

This is a storage-engine learning project. There is no SQL parser, null support, joins, transactions, concurrent writes, checksums, or crash-safe replacement protocol. The writer takes the full input table in memory, and the reader loads whole encoded chunks. Integer sums deliberately wrap modulo 2^64 rather than report overflow.

The next useful steps are streaming writes and reads, checksums, and stronger benchmarks with an uncompressed column baseline to separate projection benefits from encoding benefits. The existing format and query path already provide plenty to inspect without adding those features.

Development used AI assistance, including an automated cloud worker. That worker is now disabled; building, testing, and running minicol locally do not require an API key. The [roadmap](docs/ROADMAP.md) records changes and validation, and the [cloud-runner notes](docs/CLOUD_RUNNER.md) retain the automation history.
