# Changelog

All notable changes follow Keep a Changelog. Versions follow Semantic Versioning.

## [Unreleased]

### Added

- Repository governance, authority map, data-contract registry, C++ foundation and validation gates.
- Machine-readable artifact membership, record-lineage, semantic-digest and immutable
  receipt contracts.
- 繁體中文資料紀錄、格式選擇、schema變更與validated-only查詢準則。
- Pinned clang-format 14 CI contract.
- Typed I/O, MM/ML, exact latest-tag join, deterministic bounded runtime, BGZF writer
  and small-q topology oracle synthetic tests.
- AI task records with parent/dependency DAG validation, typed non-overlapping
  write sets, expiring leases, heartbeat rules and immutable audit envelopes.
- Source/target implementation and verification lifecycles, transform/query
  registries, nested JSON contracts and explicit coordinate semantic groups.
- Positive and negative governance tests for stale/future leases, path aliases,
  symlink ancestors, dependency cycles, forged audit references and supersession
  cycles.

### Safety

- Production truth-isolation and validated-only presentation boundaries are release blockers.
- Release tests use always-on checks, and synthetic scratch artifacts are
  process-isolated so parallel build configurations cannot overwrite one another.
- Release attestation remains `NOT_READY`; unimplemented validator fault injection,
  P3/P4/P5 parity and full seven-dataset evidence cannot be bypassed.
- Foundation verification passes 25/25 tests in Debug, Release and ASan/UBSan;
  strict release coverage still fails closed on 12 declared blockers.
