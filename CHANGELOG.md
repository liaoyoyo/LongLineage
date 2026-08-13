# Changelog

All notable changes follow Keep a Changelog. Versions follow Semantic Versioning.

## [Unreleased]

### Added

- A private-first public-preview safety foundation: row-level source replay and
  license-review states, a deterministic SPDX 2.3 inventory, third-party
  notices, branch-specific capability documentation and a fail-closed public
  gate. This records blockers and does not approve publication or a release.
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
- A versioned performance-benchmark schema, reproducible BGZF writer harness,
  source-digest replay, raw-trial/median checks and production-claim tamper test.
- A performance/output policy separating implemented, component/bounded/full
  measurements, derived values, estimates and plans, with exact comparability
  and semantic-invariance gates.
- ADR-0005 and release gates separating topology objective, candidate-family
  completion and BQ-aware vertex-set ranking, including numerical-certificate
  and large-family output requirements.
- CI dependency-lock negative regressions for retired apt pins, missing
  apt-owned CMake, missing hosted/container Boost headers and shallow
  benchmark-history checkout.
- Builder/runtime apt package manifests embedded in the production image for
  resolved-package provenance.
- A bounded HCC1395 whole-autosome dataset gate with same-binary 24/40-worker
  independent validation, eight-artifact semantic determinism and atomic
  `VALIDATED_FROZEN_DATASET_GATE` closeout.
- A versioned HCC1395 performance/edge observation contract covering additive
  versus nested time, task latency, thread/RSS/I/O telemetry, six fail-closed
  edge cases, source bindings and read-only `jq` query views.
- A sanitized standalone HCC1395 report plus machine JSON; presentation-only
  desktop/mobile/print/offline/keyboard QA passes without reading scientific
  data rows or recomputing science.
- A separate C++ `PYTHON_V2_DESCRIPTIVE_REGIONAL` compatibility CLI reproducing
  frozen 50 kb grouping, HP-family evidence, minimum-three-read patterns and the
  CPython 3.9 capped legacy solver, with ordered multi-worker publication.
- An independently linked regional bundle validator, closed validation-receipt
  schema, immutable `FROZEN` transaction and registered positive/negative parity
  gate.
- A full HCC1395 regional compatibility report binding 8,222 regions, 20,119
  units and 106,559 patterns to zero-mismatch Python crosswalks, deterministic
  1/24-worker probes and explicitly scoped performance evidence.
- Summary 2.0.0 now binds run-local dataset-gate phase scope and the exact M1
  representation without changing the repository phase ledger.
- Canonical validation now independently replays manifest/input content and
  identity, canonical input snapshot/lock and lineage, then rejects the tested
  validation-to-freeze mutation windows with a cooperative lock and publication
  snapshot. A non-cooperating same-UID writer race remains a release blocker.

### Changed

- Versioned co-occurrence pair contracts to 1.0.1 so the explicit
  endpoint-A-not-testable state may serialize `group_count=0` as `[]`, while
  retaining the 10-group ceiling, K×2 shape, aggregate and canonical-digest
  conservation; 1.0.0 schemas remain byte-identical and readable.
- Independent artifact replay now reports artifact/1-based row/field/schema
  bound context, validates embedded schema IDs, canonicalizes tuple schemas by
  item index and checks smaller artifacts first without changing artifact-map
  semantics.
- BGZF TSV rows now use one canonical payload for both physical write and
  semantic digest, removing a redundant per-row allocation/copy while retaining
  exact decompressed bytes and digests.
- Hosted CI explicitly installs and uses `/usr/bin/cmake`, checks out full Git
  history for benchmark replay, installs the declared Boost 1.74 header
  dependency, and separates repository-context tests from the sanitized
  production-image build context. CMake now rejects a missing Boost dependency
  during configure instead of failing later during compilation.
- The production container retains immutable Ubuntu/HTSlib authorities while
  resolving supported Jammy security packages at image build and recording the
  exact installed versions.
- The phase ledger and current-focus mirror now distinguish the bounded HCC1395
  dataset gate from project phase completion: M1 stable membership remains
  `COMPARABLE_DIFFERENT`, real nonzero topology is unexercised, and P7/P8 remain
  blocked.
- HCC1395 report bars now keep the minimum visible CSS width separate from the
  displayed percentage, preventing a tiny 14/79,687 category from being
  mislabeled as 0.20%; producer closeout phases are also explicitly labeled
  run-local rather than project-ledger status.
- Receipt-only post-rename recovery now fails closed until a validator-aware
  recovery path can repeat the complete input and publication replay.
- Canonical validator JSON replay now owns its unconstrained Jansson schema
  object with RAII, eliminating the sanitizer-visible reference leak without
  changing canonical bytes or validation decisions.
- HCC1395 compressed audit readers now close BGZF streams and free kstring
  buffers on callback exceptions as well as normal completion.
- Pinned container builds receive a clean-HEAD commit assertion verified by
  hosted CI while `.git` remains excluded from the build context. CMake rejects
  mismatched or dirty Git checkouts and requires explicit trusted-builder
  acknowledgement without Git metadata; builder-only Git is declared and
  recorded so repository self-tests remain runnable offline.
- Commit-specific Docker arguments are consumed only after stable apt and
  HTSlib layers, preserving dependency cache reuse across source commits.
- HCC audit synthetic tests use a separate test-only library identity, so a
  dirty development checkout can exercise negative and exception paths while
  every production audit executable retains the zero-commit fail-closed rule.

### Safety

- Production truth-isolation and validated-only presentation boundaries are release blockers.
- Release tests use always-on checks, and synthetic scratch artifacts are
  process-isolated so parallel build configurations cannot overwrite one another.
- Release attestation remains `NOT_READY`; P3/P4/P5 parity, production
  `longlineage run`, query/export/evaluate execution and full seven-dataset
  evidence cannot be bypassed.
- A fresh current-tree GCC Debug/Release replay passes 47/47 tests in each
  configuration. The earlier aggregate implementation-verification receipt
  remains an immutable 44/44 historical snapshot and is not silently rebound.
  Both are synthetic implementation evidence and do not replace the two real
  HCC1395 validator receipts or P7.
- P2 remains `IN_PROGRESS`: the HCC1395 dataset-gate path exercises the complete
  worker input bundle, one-pass projection and validator/freeze boundary, but
  the governed production entrypoint, physical process-wide memory proof and
  seven-dataset worker matrix remain open.
- The publication candidate is reconstructed from `origin/main` on the
  `fix/release-repair-verify` branch. Current-tree pre-commit hygiene passes;
  the introduced-history scan and hosted CI remain pending until a real commit
  exists. The unsafe local recovery branch remains forbidden from push, merge
  or cherry-pick.
- The HCC1395 report remains presentation-gate evidence rather than P8 external
  handoff: important-claim IDs and a registered closed-shape report JSON schema
  are still required.
- The Python-v2 regional profile is descriptive/evaluation-only: it never
  upgrades the formal topology claim, consumes no truth artifact, and its
  observed historical speed ratio is not a controlled full-language benchmark.
- Durable FAILED staging receipts, independent HTS semantic-preflight replay,
  validator-aware recovery, elimination of the non-cooperating same-UID
  publication race, P3-P8 completion, full P7 and a controlled
  scope-matched C++/Python comparison remain unfinished release blockers.
