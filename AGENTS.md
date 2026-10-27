# Working on minicol

Read README.md, docs/FORMAT.md, and docs/ROADMAP.md before changes. Preserve the documented on-disk contract or explicitly bump the version. Keep the project dependency-free C++17. Run `make test` and `make sanitize` for engine changes, and a benchmark smoke test for benchmark changes. Tests must check results against independent scalar oracles and run in release builds too.

The user authorizes ongoing useful improvements, local commits under the configured Git identity, and pushing to origin. Do not force push or rewrite history. Do not add artificial activity commits. Never fabricate performance numbers. Record benchmark commands, seeds, row counts, trial counts, machine/compiler, cache assumptions, byte accounting and limitations. Update docs/ROADMAP.md after meaningful changes. Keep raw benchmark results when publishing claims; distinguish logical file bytes requested from physical disk reads. Stay within this repository and preserve unrelated work.
