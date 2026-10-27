# Roadmap and ongoing agent work

## Delivered — 2026-10-27

- Binary row groups/footer, adaptive encodings, per-chunk zone maps, batch count scans, encoded dictionary/RLE predicates.
- 1,659 correctness checks, independent scalar oracles, randomized and malformed-file cases, ASan and UBSan.
- Five deterministic one-million-row benchmark workloads, seven trials per method, raw CSV and methodology.
- Make targets, Linux/macOS GitHub Actions template (inactive: login needs workflow scope), format docs, and resume evidence handoff.
- Public GitHub origin and hourly chat automation; commits use repository configured author identity.

## Next useful improvements, in order

1. Add AND predicates and selection vectors, then projected aggregate scans; verify against a scalar oracle.
2. Strengthen bounded malformed-input tests and add a fuzz target; document exact resource limits.
3. Add streaming row-group writes and bounded-memory readers for large inputs.
4. Add file/chunk checksums with a format version bump and compatibility tests.
5. Benchmark uniform versus clustered distributions, multiple group sizes, and narrow/wide rows. Add a strong uncompressed column baseline to isolate encoding from projection benefits.
6. Improve bit unpacking hot loops only after profiling; report before/after measurements with compiler and machine details.

Every scheduled run should choose one bounded improvement, validate it, update this log, and commit/push useful changes. Do not manufacture commit activity or benchmark claims. Resume handoff numbers describe the checked-in initial raw results; refresh claims explicitly when rerunning.
