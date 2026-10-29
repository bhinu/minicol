# Roadmap and ongoing agent work

## Delivered — 2026-10-27

- Binary row groups/footer, adaptive encodings, per-chunk zone maps, batch count scans, encoded dictionary/RLE predicates.
- 1,659 correctness checks, independent scalar oracles, randomized and malformed-file cases, ASan and UBSan.
- Five deterministic one-million-row benchmark workloads, seven trials per method, raw CSV and methodology.
- Make targets, Linux/macOS GitHub Actions template (inactive: login needs workflow scope), format docs, and resume evidence handoff.
- Public GitHub origin and hourly chat automation; commits use repository configured author identity.

## Next useful improvements, in order

1. Extend the bounded malformed-input/fuzz coverage with longer campaigns and total-work/memory budgets (input, decoded-string, per-operation payload/decoded-row caps, and fuzz target delivered below).
2. Add streaming row-group writes and bounded-memory readers for large inputs.
3. Add file/chunk checksums with a format version bump and compatibility tests.
4. Benchmark uniform versus clustered distributions, multiple group sizes, and narrow/wide rows. Add a strong uncompressed column baseline to isolate encoding from projection benefits.
5. Improve bit unpacking hot loops only after profiling; report before/after measurements with compiler and machine details.

Every scheduled run should choose one bounded improvement, validate it, update this log, and commit/push useful changes. Do not manufacture commit activity or benchmark claims. Resume handoff numbers describe the checked-in initial raw results; refresh claims explicitly when rerunning.

## Cloud runner preparation — 2026-10-27

Prepared hourly generation → isolated validation → publication workflow, pinned official actions, explicit author attribution to bhinu, bounded patch transport, and eight transport tests. Activation awaits workflow-upload permission and the OPENAI_API_KEY GitHub secret; do not call this live until the first cloud run succeeds. Existing local hourly automation remains available until cloud handoff is verified.

## AND count scans — 2026-10-27

- Added `Reader::scan_all` for typed AND predicates with internal, sorted,
  group-local row selections. The existing `scan` delegates to the same engine;
  format v1 is unchanged. Empty conjunctions count all rows from metadata.
- Validate all predicates before pruning, combine repeated-column predicates in
  one chunk read, check every predicate's zone map before payload reads, and stop
  reading later columns when an intersection is empty. Dictionary entries and
  RLE runs retain encoded predicate evaluation. Documented counters, validation
  boundaries, predicate order, and row-group selection memory in FORMAT.md.
- Release and ASan/UBSan suites pass 3,311 checks. Independent row-wise scalar
  oracles cover every codec pair, deterministic randomized conjunctions (seed
  87291), predicate reversal, duplicates, contradictory and reversed ranges,
  empty tables/conjunctions, embedded NUL strings, integer extremes, batch/group
  tails, exact column byte accounting, and malformed payloads in selection paths.
- Checks: `make test`; `make sanitize` initially failed because LeakSanitizer
  cannot run under this container's tracing environment;
  `ASAN_OPTIONS=detect_leaks=0 make sanitize` passed with address and undefined
  behavior sanitizers active. Leak detection remains unverified here.
- Benchmark regression smoke: `make build/benchmark`, then
  `./build/benchmark 10000 1 build/data-smoke`. All five existing workloads and
  three methods agreed with the row baseline. Retained all 15 raw trials in
  [benchmark-smoke-2026-10-27.csv](benchmark-smoke-2026-10-27.csv), with compiler,
  machine, flags, seed 42, row/trial counts, cache assumptions, logical byte
  accounting, and limitations in
  [the environment record](benchmark-smoke-2026-10-27-environment.txt).
  This single-group smoke does not measure AND performance or establish a
  performance claim; the earlier benchmark evidence remains unchanged.
- Remaining: projected aggregates and their overflow contract; selection vectors
  currently stay internal and each visited predicate chunk is decoded in full.
  Workload-driven predicate ordering, AND benchmark workloads, fuzzing/resource
  limits, and streaming readers/writers remain future work.

## Cloud handoff verified — 2026-10-27

First cloud run 40596007041 passed all three jobs and published af440d8 with bhinu author attribution. API credential and hourly enable switch are configured. Local hourly automation paused after verification. Generation, validation, and publication now run on GitHub-hosted runners independently of the Mac. See docs/CLOUD_RUNNER.md for controls and run evidence.

## Projected int64 sums — 2026-10-28

- Added `Reader::sum_int64(column, predicates, zone_maps)` over internal AND
  selections, returning the sum and existing scan counters. Empty predicates sum
  the whole projected column; empty selections return zero. Addition wraps modulo
  2^64 and interprets the result as signed two's complement, including across
  row groups. No dependencies or format v1 changes.
- Reused predicate validation, encoded predicate evaluation, zone pruning, and
  early termination. Projection chunks are loaded only for surviving selections;
  shared predicate/projection chunks are retained and read from the file once.
  Documented semantics, logical byte accounting, validation boundaries, memory
  use, and current full-chunk decoding in FORMAT.md; added a README example.
- Release suite passes 5,637 checks. Independent row-wise scalar sum oracles cover
  all existing predicate codecs and plain/RLE/packed integer projections, random
  data (existing seeds 7331, 91527, 87291), predicate reversal, repeated columns,
  contradictions, reversed ranges, integer extremes, empty tables/selections,
  batch/group tails, and overflow. Added exact projection byte/group/batch checks,
  invalid projection/predicate checks, and malformed aggregate payload tests.
- Checks: `make test` passed. `make sanitize` reached the container's existing
  LeakSanitizer fatal error because leak detection cannot run under ptrace;
  `ASAN_OPTIONS=detect_leaks=0 make sanitize` passed with ASan/UBSan active.
  Leak detection remains unverified in this environment.
- Benchmark smoke: `make build/benchmark`, then
  `./build/benchmark 10000 1 build/data-smoke-2026-10-28`. All five existing count
  workloads and three methods agreed with the row-store baseline. Retained all
  15 trials in [raw CSV](benchmark-smoke-2026-10-28.csv), with machine/compiler,
  flags, seed, cache assumptions, byte accounting, and limitations in the
  [environment record](benchmark-smoke-2026-10-28-environment.txt). This smoke
  makes no aggregate performance claim; earlier benchmark evidence is preserved.
- Remaining: bounded malformed-input tests/fuzzing and exact resource limits are
  the next independent increment. Other aggregate operations, checked-overflow
  alternatives, aggregate/AND benchmark workloads, and streaming readers/writers
  remain future work. Summation currently decodes the full projected chunk and
  redecodes it when shared with predicates; RLE sums expand values in the scalar
  consumer. Optimizing those paths requires profiling and retained measurements.

## Bounded input limits and fuzzing — 2026-10-28

- Added opt-in `ReadLimits` to `Reader`: inclusive footer-byte, encoded
  chunk-byte, and actual group-row caps. Reject oversized footers before loading,
  oversized groups before schema/group allocation, and oversized chunks during
  metadata validation, including chunks that scans might prune. Default limits
  preserve all format-v1 dimensions and existing API calls; persisted format and
  dependencies are unchanged. Documented exact boundaries, default behavior,
  string-expansion costs, and the absence of total-memory/time budgets.
- Release suite passes 5,842 checks. Added exact-boundary and one-below-limit
  cases, zero limits, empty/short groups with large header target sizes, all
  codecs with independent scalar count/AND/sum oracles, every truncated prefix
  of an independently constructed packed file, oversized footer dimensions, and
  tiny RLE/zero-width packed files advertising `UINT32_MAX` rows. The latter are
  accepted as metadata under defaults and rejected before decoding with caps.
- Added optional `make fuzz` using Clang/libFuzzer with ASan/UBSan. Nine generated
  seeds exercise every codec/type combination, signed extremes, zero-width
  packing, NUL strings, multi-column groups, tails, and empty tables. The harness
  caps file/footer/chunk/group dimensions and post-parse row/column counts;
  accepted payloads are compared with independent materialized row-wise scalar
  count/AND/wrapping-sum oracles. Pruning checks require bounds containing actual
  decoded values because v1 has no integrity checks. Corpus/artifacts stay in
  `build/`; no binary assets are added. See FORMAT.md for limits and longer runs.
- Checks: `make test` passed. `make sanitize` and the initial `make fuzz` reached
  this container's known LeakSanitizer fatal error under ptrace. Reruns with
  `ASAN_OPTIONS=detect_leaks=0 make sanitize` and
  `ASAN_OPTIONS=detect_leaks=0 make fuzz` passed with ASan/UBSan enabled;
  leak detection remains unverified here. Fuzz smoke used Clang 18.1.3, seed 42,
  1,000 runs, max input 65,536 bytes, and five-second per-input timeout. This is
  bounded smoke coverage, not an exhaustive safety claim.
- Required benchmark smoke: `make build/benchmark`, then
  `./build/benchmark 10000 1 build/data-smoke-input-limits-2026-10-28`.
  All five workloads and three methods agreed with the row baseline. Preserved
  earlier evidence and retained all 15 new trials in
  [raw CSV](benchmark-smoke-input-limits-2026-10-28.csv), with command, compiler,
  machine, seed, row/trial counts, cache assumptions, logical-byte accounting,
  and limitations in the [environment record](benchmark-smoke-input-limits-2026-10-28-environment.txt).
  No performance claim is made.
- Remaining: longer fuzz campaigns and regression cases for any findings;
  stricter decoded-string/total-memory and total-work budgets; streaming writes
  and bounded-memory reads. Limits are opt-in and are input dimensions rather
  than a process-memory guarantee. Checksums with a version bump, expanded
  benchmarks, and profiled hot-loop improvements remain later roadmap items.

## Decoded-string chunk budgets — 2026-10-28

- Added opt-in `ReadLimits::max_decoded_string_bytes`, an inclusive sum of
  string lengths over every row in each loaded chunk. Plain lengths, repeated
  dictionary references, and RLE expansion count equally, including NUL bytes.
  Default and existing three-field initializers remain unlimited; format v1 and
  dependencies are unchanged. README.md and FORMAT.md describe the API,
  validation timing, scope, and memory limitations.
- Finite budgets preflight loaded string payloads before string construction or
  expansion, using overflow-safe accounting and dictionary length metadata.
  Materialization, count/AND scans, and projected-sum predicate scans enforce the
  same limit. Pruning and early termination still avoid unused chunks. Budgets
  reset per chunk/operation and do not affect integer chunks. Finite caps add a
  parsing pass; this is a logical content bound, not a process-memory guarantee.
- Release suite passes 6,027 checks. Independent source/scalar oracles cover all
  string codecs, exact and one-below bounds, variable-length dictionary strings,
  embedded NULs, zero-byte empty strings, repeated calls, batch/group tails,
  pruning, AND/sum predicates, and unchanged integer results. An independently
  constructed tiny RLE file represents `UINT32_MAX` three-byte strings: an exact
  64-bit budget permits an encoded count, while one below rejects before
  materialization. Malformed payload tests now run with unlimited and finite
  caps, including invalid IDs, truncated strings, and invalid string runs.
- Checks: `make test` passed in the release build (`-O3 -DNDEBUG`).
  `make sanitize` hit the documented LeakSanitizer fatal error under ptrace;
  `ASAN_OPTIONS=detect_leaks=0 make sanitize` passed all 6,027 checks with
  address and undefined-behavior sanitizers enabled. Leak detection remains
  unverified in this environment. `git diff --check` passed.
- The fuzz harness now caps decoded string content at 8,192 bytes per chunk.
  `ASAN_OPTIONS=detect_leaks=0 make fuzz` passed 1,000 runs with seed 42,
  Clang 18.1.3, max input 65,536 bytes, and five-second per-input timeout.
  Initial `make fuzz` completed its inputs but hit the documented LeakSanitizer
  fatal error under ptrace at exit; leak detection remains unverified.
- Required benchmark smoke: `make build/benchmark`, then
  `./build/benchmark 10000 1 build/data-smoke-string-budget-2026-10-28`.
  All five workloads and three methods agreed with the row-store baseline.
  Preserved earlier evidence and retained all 15 trials in
  [raw CSV](benchmark-smoke-string-budget-2026-10-28.csv), with command, seed,
  row/trial counts, machine/compiler, flags, cache assumptions, logical byte
  accounting, and limitations in the
  [environment record](benchmark-smoke-string-budget-2026-10-28-environment.txt).
  The benchmark uses unlimited defaults; no configured-cap performance claim
  is made.
- Remaining: total-query work limits, full allocation accounting (including
  metadata, unused dictionary entries, string objects/copies, and caller-retained
  columns), longer fuzz campaigns and regression cases for findings. Streaming
  writes and bounded-memory readers remain the next larger feature; checksums,
  expanded benchmarks, and profiling remain later roadmap items.

## Per-operation payload-byte budgets — 2026-10-28

- Extended the first roadmap item with opt-in
  `ReadLimits::max_payload_bytes_per_operation`: an inclusive cumulative cap on
  encoded chunk bytes loaded by each materialization, count/AND scan, or sum
  call. Charge before allocating/reading each payload, using subtraction to
  avoid overflow; return an exception rather than a partial result if exceeded.
  Defaults and existing aggregate initializers retain unlimited budgets. Format
  v1, dependencies, and persisted bytes are unchanged.
- Budgets reset per operation, including after failures, and exclude metadata.
  Pruning and empty selections avoid charges; repeated predicates and retained
  predicate/projection chunks charge bytes once per group. Documented exact
  semantics, error, predicate-order effects, and limitations in FORMAT.md and
  added a README example. This bounds logical payload bytes requested, not
  physical I/O, decoded row visits, elapsed time, or process memory.
- Release and ASan/UBSan suites pass 6,279 checks. Added scalar-oracle count,
  AND, and wrapping-sum comparisons across every existing codec/type, multiple
  groups and a short tail, exact and one-below budgets, repeated calls, separate
  and shared projections, partial/full pruning, early termination, zero-budget
  empty tables/metadata scans, and malformed-payload error precedence. Existing
  source-checked chunk roundtrips and metadata byte accounting remain covered.
- Checks: `make test` passed with `-O3 -DNDEBUG`.
  The initial `make sanitize` exposed a test-fixture mistake (an absent value
  inside a broad zone range still requires a payload read), which was fixed,
  and reached the known LeakSanitizer fatal error under ptrace.
  `ASAN_OPTIONS=detect_leaks=0 make sanitize` passed the final 6,279 checks with
  address and undefined-behavior sanitizers enabled. Leak detection remains
  unverified in this environment. `git diff --check` passed.
- Required benchmark smoke: `make build/benchmark`, then
  `./build/benchmark 10000 1 build/data-smoke-operation-budget-2026-10-28`.
  All five count workloads and three methods agreed with the row-store baseline.
  Retained all 15 trials in the [raw CSV](benchmark-smoke-operation-budget-2026-10-28.csv),
  with command, seed, row/trial counts, machine/compiler, flags, cache assumptions,
  logical byte accounting, and limitations in the
  [environment record](benchmark-smoke-operation-budget-2026-10-28-environment.txt).
  The benchmark uses unlimited defaults; no performance claim is made and
  earlier benchmark evidence is preserved.
- Remaining: decoded-row/CPU work and metadata-traversal budgets, full allocation
  accounting (including caller-retained results), longer fuzz campaigns and
  regression cases for findings. Streaming writes and bounded-memory reads
  remain the next larger feature; checksums, expanded benchmarks, and profiling
  remain later roadmap items.

## Per-operation decoded-row budgets — 2026-10-29

- Extended the first roadmap item with opt-in
  `ReadLimits::max_decoded_rows_per_operation`, an inclusive cumulative cap on
  logical chunk rows per decoding pass in each read/count/AND/sum call. Charge
  whole chunks before payload allocation/read using overflow-safe subtraction;
  shared predicate/sum projections charge a second pass before summation while
  retaining the existing one-read payload accounting. Defaults and existing
  initializers remain unlimited; format v1 and dependencies are unchanged.
- Pruning and empty selections avoid charges, repeated-column predicates share
  one pass, and each call resets its budget even after failure. Encoded RLE
  counts charge represented rows despite avoiding expansion. README.md and
  FORMAT.md document semantics, error precedence, and accounting limitations.
- Release tests pass 6,693 checks (`make test`, `-O3 -DNDEBUG`). New independent
  source/scalar-oracle tests cover all six codec/type combinations, exact and
  one-below limits, multiple groups and batch/group tails, repeated calls,
  multi-column ANDs, separate/shared projections, wrapping sums, full/partial
  pruning, early termination, empty tables, and malformed payloads. Independently
  constructed tiny RLE/zero-width chunks advertising `UINT32_MAX` rows reject
  before decoding with a small budget; encoded RLE counts pass at the exact
  represented-row boundary.
- Sanitizers: `make sanitize` reached this environment's documented LeakSanitizer
  fatal error under ptrace. `ASAN_OPTIONS=detect_leaks=0 make sanitize` passed
  all 6,693 checks with address and undefined-behavior sanitizers active; leak
  detection remains unverified here. `git diff --check` passed.
- Required benchmark smoke: `make build/benchmark`, then
  `./build/benchmark 10000 1 build/data-smoke-row-budget-2026-10-29`.
  All five existing count workloads and three methods agreed with the row-store
  baseline. All 15 trials are retained in the
  [raw CSV](benchmark-smoke-row-budget-2026-10-29.csv), with command, seed,
  row/trial counts, machine/compiler, flags, cache assumptions, logical byte
  accounting, and limitations in the
  [environment record](benchmark-smoke-row-budget-2026-10-29-environment.txt).
  This smoke uses unlimited defaults and makes no performance claim; earlier
  evidence remains unchanged.
- Remaining: metadata/predicate traversal and full allocation budgets, longer
  fuzz campaigns and regression cases for findings. The row budget bounds
  logical decoding passes, not CPU instructions, elapsed time, or process memory;
  string preflight, dictionary construction, comparisons, selection intersection,
  and caller-retained data still need separate accounting. Streaming writes and
  bounded-memory reads remain the next larger feature; checksums, expanded
  benchmarks, and profiling remain later items.
