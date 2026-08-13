# LongLineage Public Preview Boundary

> **NOT_READY · RESEARCH PREVIEW · NON-PRODUCTION**
>
> Candidate `b9aaa12a11fa00606bd174dabd0f172a5d112359` must remain private. It is
> not safe to publish directly, is not a release candidate, and must not receive a
> production tag or GitHub Release.

## Permitted interpretation

This candidate is reviewable engineering evidence for a truth-isolated C++17/HTSlib
research pipeline. It can demonstrate that the repository builds, that the synthetic
foundation suite is executable, and that unfinished production science fails closed.

It cannot establish a biological clone, cellular ancestor, time order, unique lineage,
production readiness, seven-dataset parity, or public redistribution approval. The
formal claim ceiling remains:

> long-read sSNV co-occurrence and lineage-compatible mutation-state families

## Current machine state

| Item | Status | Meaning |
|---|---|---|
| P3 M1 parity | `BLOCKED` | Stable-membership parity is unresolved. |
| P4 M2/co-occurrence parity | `BLOCKED` | Seven-dataset authority and boundary vectors are incomplete. |
| P5 topology parity | `BLOCKED` | Real nonzero topology and exact-family/ranking evidence are incomplete. |
| P7 seven-dataset validation | `BLOCKED` | The 24/40-worker full matrix has not run. |
| P8 presentation/release | `BLOCKED` | License, claim-ID, schema, accessibility and strict-gate blockers remain. |
| `longlineage run` | `KernelBlocked`, exit `6` | Expected safe behavior while release attestation is `NOT_READY`. |

At the frozen `b9aaa12` baseline, GCC Debug CTest reported **47/47 synthetic repository
tests passing**. That number is a foundation test receipt only. It does not prove P3,
P4, P5, P7, P8, real-data parity, production validity, or the feature-only tagged-BAM
toolchain. Safety work layered after `b9aaa12` may add repository tests; do not keep
copying `47` as a timeless current count.

## Why this commit cannot be made public directly

The 2026-08-13 public-safety audit found all of the following:

1. Full feature-branch history scanning found seven prohibited historical blobs:
   four private-path findings and three real-coordinate-shaped findings.
2. The source-to-target manifest has 21 target digest matches, but only 5/21 source
   rows replay at its declared root origin commit. Twelve rows replay at other pinned
   commits and four rows remain unresolved, representing three unique missing hashes.
3. Every row-level public-license disposition remains
   `PENDING_PUBLIC_RELEASE_REVIEW`; the repository-level audit remains
   `PENDING_PUBLIC_RELEASE_AUDIT`.
4. The deterministic SBOM intentionally uses `NOASSERTION` for unresolved dependency
   and origin licensing. It is an inventory, not a license approval.
5. The strict gate still has 12 declared fixture-only blockers.

The machine-readable evidence is in
[`PUBLIC_SAFETY_RECEIPT.json`](PUBLIC_SAFETY_RECEIPT.json). Source and dependency
limitations are in [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) and
[`SBOM.spdx.json`](../../SBOM.spdx.json).

## Branch-specific capability

Tagged-BAM and lineage helper binaries exist only on the feature candidate and are not
part of `origin/main`. Their presence and `--help` behavior do not constitute
production validation. See [`CAPABILITY_MATRIX.md`](CAPABILITY_MATRIX.md).

## Required command behavior

```bash
# Foundation: may pass while release blockers remain visible.
scripts/ci/check_all.sh BUILD_DIR

# Public-preview safety: must currently fail closed.
scripts/ci/check_public_preview_gate.sh origin/main HEAD

# Production entry point: must currently return KernelBlocked (6).
BUILD_DIR/bin/longlineage run --manifest MANIFEST --repo REPO_ROOT
```

A passing foundation suite plus a failing public-preview gate is the correct current
combination. Do not change visibility, create a tag, or publish assets until the public
gate is independently replayed with zero blockers.
