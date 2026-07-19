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
- Deterministic sample/contig/cost block planning with exact 4,096-site,
  250,000-estimated-alignment and clipped ±5 kb boundary contracts.
- Move-only, explicitly indexed BAM block readers for worker-owned HTSlib
  handles; fixed flag/MAPQ/query-length/MM/ML filters expose typed exclusion
  counters.
- A centrally sized logical retained-byte reorder sink with reserved frontier
  credit, completion-order-independent oversize rejection, duplicate/late/gap
  rejection and pool/sink cancellation wake-up.
- Dedicated P2 gates for planner, indexed reader, adversarial reorder,
  forced-out-of-order 1/2/4/24/40 worker/chunk semantic determinism and worker
  failure cancellation/staging behavior.
- AI readiness staleness checks scoped to the intentionally independent
  governance target, with a CTest regression preventing unrelated producer
  headers from invalidating the governance executable.

### Safety

- Production truth-isolation and validated-only presentation boundaries are release blockers.
- Release tests use always-on checks, and synthetic scratch artifacts are
  process-isolated so parallel build configurations cannot overwrite one another.
- Release attestation remains `NOT_READY`; unimplemented validator fault injection,
  P3/P4/P5 parity and full seven-dataset evidence cannot be bypassed.
- Foundation plus P2 synthetic verification passes 31/31 tests in Debug,
  Release and ASan/UBSan; P2 concurrency cases also pass 5/5 under TSan.
  P2 remains `IN_PROGRESS`/partial until the complete production input bundle,
  physical global memory bound, multi-marker projection, validator/freeze
  integration and P7 full-data replay exist.
