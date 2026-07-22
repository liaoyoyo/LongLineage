# ADR-0009: Scope summary phases and require independent input/publication replay

Status: Accepted

## Context

The 1.0.0 `longlineage.summary` contract exposed `phase_status.P0` through `P8` without saying that
these values described the dataset-gate producer closeout rather than the repository-wide phase
ledger. The independent validator also trusted producer-recorded input digests and allowed a
receipt-only crash recovery to publish an otherwise un-replayed final root.

## Decision

1. Preserve `schema/core/summary-1.0.0.schema.json` for historical readers and make the current
   summary schema 2.0.0.
2. Require
   `phase_status_scope=RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER`; the independent
   validator rejects a missing or different scope.
   The same summary scope also requires the exact `m1_representation`, so the historical rounded
   compatibility path cannot be silently confused with the raw binary32 endpoint.
3. Canonical dataset-gate validation requires the exact production manifest. The validator hashes
   and parses one immutable manifest byte snapshot, hashes every locked input, recomputes the
   canonical input snapshot and input-lock digest, verifies current repository contract bindings,
   and compares the frozen `site_reads` manifest-input lineage.
4. The HCC1395 profile additionally parses the bound authority contract and verifies its identity,
   claim ceiling, variant census and exact eight-role size/SHA set against the production manifest.
   Producer startup likewise derives the parsed manifest and closeout digest from one immutable byte
   snapshot; it never re-reads the manifest after science.
5. The validator owns a static transform/dependency graph for all nine scientific artifacts plus
   `artifact_catalog`, `data_lineage` and `semantic_digests`. Producer catalog/lineage rows cannot
   redefine that graph by agreeing with one another. Manifest dataset order/IDs must also equal the
   `site_reads`, `m1_sites` and ordered summary scope.
6. `contracts/v1/science_parameters.json` binds
   `m1_runtime_representation=HISTORICAL_OBSERVED_ROUND6_NULL_ROUND4`, and the summary must repeat
   that exact versioned metadata. This proves representation metadata binding only; it is not a
   claim of independent M1 numerical parity.
7. Canonical producer drafts bind the current repository `state/phase_ledger.json` SHA-256 exactly.
   Validation-receipt SHA-256 is derived from the canonical bytes already written and fsynced, not
   by reopening the renamed receipt.
8. A freeze captures an exact publication-file snapshot, repeats input replay before finalization,
   hashes the staging tree before rename, and hashes the final unpublished tree before publishing
   `run_receipt.json`. An output-base advisory lock serializes cooperating publishers.
9. Receipt-only `recover_after_rename()` is disabled. A crash leaves `FINAL_UNPUBLISHED`
   query-invisible until a future validator-aware recovery API repeats the full replay.

## Migration

- New producer artifacts declare `longlineage.summary@2.0.0` and include the required phase and M1
  representation scope.
- Historical 1.0.0 summaries remain readable only through explicitly versioned historical audit
  paths; the current producer/validator does not silently upgrade them.
- Validator CLI freeze requires `--manifest FILE`. Check-only canonical validation also fails closed
  without it; reduced physical-fault fixtures remain non-freezable.

## Verification

- A canonical fixture with real regular input files and matching manifest freezes successfully.
- Missing manifest, mutated manifest/input, HCC authority mismatch, wrong phase-ledger binding,
  self-consistent but wrong provenance graph, dataset-scope mismatch and wrong M1 representation fail closed.
- Mutation after atomic rename is detected by `PRE_FREEZE_REPLAY`; no published receipt appears and
  the root remains query-invisible.
- Receipt-only recovery returns `STATE_CONFLICT`, including when given the exact pending-receipt
  digest.

## Consequences

Independent validation performs full input I/O twice around finalization, so validation wall time
and I/O bytes increase. This is an intentional correctness cost and must be reported separately
from science runtime. Crash recovery is temporarily less convenient but cannot publish artifacts
using receipt bytes alone. The output-base lock is advisory and serializes only cooperating
publishers. A non-cooperating same-UID process can still rewrite path-based artifacts between the
last replay and receipt publication; eliminating that race requires a stronger immutable-FD or
content-addressed publication design and remains a P6 blocker.
