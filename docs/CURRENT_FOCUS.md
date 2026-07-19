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

## Active blockers

- Private GitHub remote creation and privacy verification.
- A Draft 2020-12-capable JSON Schema validator pinned in the release toolchain.
- P3 M1 PCG64/RNG/tie parity and a frozen, versioned mapping from normalized
  HP states to HP families. No family grouping may be inferred ad hoc.
- P4 exact K×2/co-occurrence first formal authority, Endpoint-B O/X callability
  precedence vectors and the same HP-family registry.
- P5 complete-family and direct HiGHS parity.
- P2 production input bundle (VCF/Tabix, latest-tag sidecar/Tabix and
  FASTA/FAI), one-pass CIGAR multi-marker projection and staging→validator→
  freeze integration. The current sink limits logical admitted payload bytes;
  it does not bound HTSlib/BGZF buffers, allocator overhead, task results,
  thread stacks or process RSS.
- P7 seven-dataset 24/40 worker full runs.
- Public release license/source-origin audit.

## Next legal actions

1. Create and verify the private GitHub remote without changing visibility to
   public.
2. Pin a validator that actually implements each schema's declared draft and
   replay the full positive/negative fixture set.
3. Extend the verified indexed-BAM component into the complete per-worker
   BAM/VCF/sidecar/FASTA bundle and wire exact latest-tag join plus one-pass
   multi-marker projection into staging.
4. Freeze the missing M1 RNG/logical-digest and HP-family vectors before porting
   the scientific kernel.
5. Keep P3-P5 and P7-P8 blocked until their independent evidence exists; keep
   P6 `IN_PROGRESS` until validator/query/export parity evidence exists.
