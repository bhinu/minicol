# Roadmap and ongoing agent work

## Delivered — 2026-10-27

- Binary row groups/footer, adaptive encodings, per-chunk zone maps, batch count scans, encoded dictionary/RLE predicates.
- 1,659 correctness checks, independent scalar oracles, randomized and malformed-file cases, ASan and UBSan.
- Five deterministic one-million-row benchmark workloads, seven trials per method, raw CSV and methodology.
- Make targets, Linux/macOS GitHub Actions template (inactive: login needs workflow scope), format docs, and resume evidence handoff.
- Public GitHub origin and hourly chat automation; commits use repository configured author identity.

## Next useful improvements, in order

1. Strengthen bounded malformed-input tests and add a fuzz target; document exact resource limits.
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
