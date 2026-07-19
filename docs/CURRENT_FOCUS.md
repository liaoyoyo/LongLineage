# Current Focus

Last updated: 2026-07-19

## Active phase

P0/P1/P2/P6 foundation work remains in progress. The repository now has
machine-enforced AI ownership/lease/write-set rules, an immutable audit-envelope
contract, a versioned schema and lifecycle registry, typed I/O, deterministic
runtime/artifact foundations, truth-isolated preflight and independent
validation/query skeletons. These foundations are tested; none of those phase
labels is upgraded to `VERIFIED` by scaffolding alone.

P2 now has a synthetic component implementation for deterministic block
planning, explicitly indexed BAM reads with one handle per worker, central
logical retained-byte sizing, frontier-reserved result reordering, pool/sink
cancellation and 1/2/4/24/40 worker replay. This is `PARTIAL` evidence: the
production worker bundle still lacks persistent VCF/sidecar/FASTA handles,
one-pass multi-marker projection, a physical process-wide memory bound and
validator/freeze integration.

A repository-wide method/performance audit is active. The first lossless output
change now shares one canonical row payload between BGZF write and semantic
digest. A governed local component benchmark records 8+8 trials and identical
logical/physical bytes plus semantic SHA; its measured `7.93%` median wall
reduction applies only to that synthetic writer workload and explicitly does not
authorize a production speedup claim.

Draft PR #1的CI toolchain hotfix已通過本機與遠端驗證。Hosted
matrix現在明示使用apt-owned `/usr/bin/cmake`與full Git history；production
image保留immutable base digest與HTSlib SHA，mutable apt解決結果改由image內
builder/runtime package manifests如實保存。本機full repository gate為33/33
PASS，sanitized production-context no-network gate為30/30 PASS；GitHub push與
PR runs各7/7 PASS。三者scope不可互相冒充。

## Phase status mirror

This table is a governed mirror of `state/phase_ledger.json`; it is not an
independent source for changing phase state.

| Phase | Status |
|---|---|
| P0 | IN_PROGRESS |
| P1 | IN_PROGRESS |
| P2 | IN_PROGRESS |
| P3 | BLOCKED |
| P4 | BLOCKED |
| P5 | BLOCKED |
| P6 | IN_PROGRESS |
| P7 | BLOCKED |
| P8 | BLOCKED |

## Current truth

- Exact topology historical authority SHA is frozen in the oracle manifest.
- Latest HP/PS raw-all sidecar upstream closeout is 7/7 PASS.
- M1 frozen screen has a source-attested census but no C++ parity yet.
- M2 eligibility census exists; formal full co-occurrence output does not.
- Historical frozen-v2 real M2 pilot timed out and is NO-GO.
- Exact-preserving remediation is PASS_FOR_PROBE, not full-scale proof.
- The SHA-bound 2026-07-19 topology/VAF handoff remains `NO_GO_PRODUCTION`:
  optimized objective certification is 25/25, but family completion is 16/25
  and hard-case ranking is not complete. Objective, family and ranking are now
  governed as three distinct completion axes by ADR-0005.
- Primary abundance ranking is BQ-aware and scores each candidate vertex set
  once; it does not identify parent edges. The historical scalar AF monotonic
  heuristic remains a separate sensitivity endpoint.
- Historical candidate-window, sidecar and topology observations are classified
  as bounded/context evidence. Truth-derived windows and bounded speedups cannot
  be promoted into production.
- The native artifact catalog implies 16 artifacts plus 8 indexes, but the
  24-file target remains a derived plan until P7 measures the frozen root.

## Active blockers

- Draft PR #1 review and gated merge; private visibility and all three uploaded
  branch SHAs are already verified.
- A Draft 2020-12-capable JSON Schema validator pinned in the release toolchain.
- P3 M1 PCG64/RNG/tie parity and a frozen, versioned mapping from normalized
  HP states to HP families. No family grouping may be inferred ad hoc.
- P4 exact K×2/co-occurrence first formal authority, Endpoint-B O/X callability
  precedence vectors and the same HP-family registry.
- P5 complete-family and direct HiGHS parity, plus topology-record schema
  migration to an independent `ranking_state`, recurrence-aware family
  completion and outward-rounded interval certificates. Current
  `topology_unit` 1.0.0 must not be used by a production ranker.
- P2 production input bundle (VCF/Tabix, latest-tag sidecar/Tabix and
  FASTA/FAI), one-pass CIGAR multi-marker projection and staging→validator→
  freeze integration. The current sink limits logical admitted payload bytes;
  it does not bound HTSlib/BGZF buffers, allocator overhead, task results,
  thread stacks or process RSS.
- P7 seven-dataset 24/40 worker full runs.
- Runtime performance collector, I/O operation counts, transient peak bytes and
  production comparison receipts.
- Public release license/source-origin audit.

## Next legal actions

1. Review draft PR #1 and keep the repository private; merge only after the
   intended branch scope and required checks are accepted.
2. Pin a validator that actually implements each schema's declared draft and
   replay the full positive/negative fixture set.
3. Extend the verified indexed-BAM component into the complete per-worker
   BAM/VCF/sidecar/FASTA bundle and wire exact latest-tag join plus one-pass
   multi-marker projection into staging.
4. Migrate the topology record contract per ADR-0005 before implementing the
   production ranker; keep q<=4 as an independent structural oracle and keep
   any hard-case best/tie unpublished without interval certificates.
5. Freeze the missing M1 RNG/logical-digest and HP-family vectors before porting
   the scientific kernel.
6. Keep P3-P5 and P7-P8 blocked until their independent evidence exists; keep
   P6 `IN_PROGRESS` until validator/query/export parity evidence exists.
7. Implement the performance collector, persistent worker input bundle,
   one-pass multi-marker decoder and streaming indexes before making any
   production efficiency claim.
