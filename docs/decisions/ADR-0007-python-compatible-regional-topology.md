# ADR-0007: Python-v2-compatible descriptive regional topology endpoint

Status: Accepted; full HCC1395 validation complete

## Context

The frozen 2026-07-13 Python endpoint reports 8,222 HCC1395 spatial regions
and 20,119 region-by-HP-family units. The current LongLineage formal topology
endpoint reports zero units after M2, joint-signature and directionality gates.
Those counts answer different questions: the former is a descriptive 50 kb
regional reconstruction, while the latter is a guarded formal endpoint.
Treating either count as a regression oracle for the other is invalid.

The user requires a fast C++/HTSlib implementation that exactly reproduces the
Python endpoint, uses the same truth-isolated raw BAM plus authoritative HP/PS
sidecar, runs in parallel and retains independently validated provenance.

## Decision

### 1. Add a separate profile and binaries

The endpoint ID is `PYTHON_V2_DESCRIPTIVE_REGIONAL`. It is implemented by
`longlineage-regional-compat`; the independent
`longlineage-regional-validate` binary does not link the producer/solver
library. Neither binary modifies the production dataset gate, formal M2
membership, production artifact catalog or production validator.

This profile may describe regional mutation-state families. It may not claim a
clone, ancestor, temporal order, formal topology discovery or production
release. Formal topology zero and descriptive regional 8,222 remain explicitly
`NOT_COMPARABLE` endpoints.

### 2. Freeze exact Python-v2 membership semantics

- autosomal PASS biallelic sSNVs are grouped by adjacent gap `<=50,000` bp;
  transitivity is retained and total component span is unbounded;
- a component with more than eight sites contributes only the minimum-span
  consecutive eight-site window, with the first window winning a tie;
- BAM evidence uses MAPQ >=20, BASEQ >=0, the frozen `samtools` pileup flag
  exclusion (`UNMAP|SECONDARY|QCFAIL|DUP`) and includes supplementary records;
- alignment identity is raw QNAME, contig, start0, end0, FLAG and BLAKE2b-8 of
  CIGAR; identity conflicts are dropped and authoritative sidecar joins are
  exact with no embedded-BAM fallback;
- observed bases map to R/A; OTHER, deletion, refskip and absence map to X in
  the downstream vector; an all-X vector does not enter pattern evidence;
- HP is mapped by the frozen prefix rule to `1`, `2`, `3`, `4` or `none`;
- full and subread patterns independently require at least three reads.

### 3. Preserve the legacy solver, including its cap boundary

The compatibility solver minimizes extra nodes over the ALT-bit hypercube,
requires every full node and every partial subcube to be covered, and requires
each non-root node to have a unit-flip predecessor. Search is limited to four
extra nodes and 150,000 combinations per level exactly as in the frozen source.
Parent mappings are counted analytically for each minimum node set.

The frozen capped fallback iterates a CPython 3.9 set. Although capped outputs
cannot certify a complete candidate family, their displayed `n_hidden` and
`n_trees` are reproduced with the frozen CPython 3.9 frozenset hash and table
iteration policy. They remain explicitly `capped`; no completeness claim is
derived from fallback fields.

The newer obligation B&B/subset-DP kernel remains the formal/new-method path.
It is not substituted into this compatibility endpoint because doing so would
change the historical census.

### 4. Use bounded deterministic parallelism

One ordered task processes one region and all its HP families. Each of at most
64 workers owns persistent BAM, BAM-index, sidecar and Tabix handles. A single
bounded `OrderedThreadPool` publishes results in genomic submission order.
No worker creates nested HTSlib decompression pools. Worker count must not
change `regions.tsv`, `units.tsv` or `patterns.tsv` semantic content.

### 5. Require a closed bundle and independent freeze

The producer emits exactly:

- `summary.json`
- `regions.tsv`
- `units.tsv`
- `patterns.tsv`
- `producer_receipt.json`
- `checksums.sha256`

It records `READY_FOR_VALIDATION`, never a frozen claim. The validator rejects
missing, extra, symlinked, corrupt, truncated, unordered, partial-probe or
foreign-key-inconsistent content. Only after before/after file-identity replay
and all checks pass may it write `validation_receipt.json` and `FROZEN`.

The three closed schemas live under `schema/compat/`. They intentionally remain
outside the production artifact catalog because this profile is not a
production/formal-topology endpoint; the frozen full-run summary, producer
receipt and validation receipt must nevertheless validate against them.

## Acceptance gates

1. Synthetic grouping, HP, MINREAD, solver and negative tests pass under
   warnings-as-errors.
2. Frozen MLHP replay returns exactly 8,222 regions, 20,119 units and the
   complete all-unit/primary class census.
3. A real-BAM probe has zero region, pattern and unit mismatch against frozen
   Python evidence; the validator must reject it as partial.
4. Full HCC1395 workers=24 output has exact Python region/unit/class and
   per-key evidence parity, then passes the independent validator/freeze.
5. A controlled workers=1 replay or run must have identical semantic science
   rows before any speedup statement is accepted.

## Validation outcome

The full HCC1395 chr1–22 workers=24 bundle passed all gates: 8,222 regions,
20,119 units and 106,559 pattern rows; region/unit/pattern key-map mismatch is
zero. The independent validator passed 13 checks before publishing `FROZEN`.
A same-input 100-region workers=1/24 probe produced byte-identical TSV files;
this proves bounded deterministic parallel execution but is not mislabeled as
a full workers=1 BAM run. The historical Python/C++ elapsed-time ratio remains
an observational comparison because scheduler and cache conditions differ.

## Consequences

The project gains a reproducible historical endpoint without weakening the
formal science gates. Additional I/O optimization is permitted only when the
typed rows remain identical. CN/LOH is post-structure annotation and is not
required for L0/L1 production-compatible membership; its historical boundary
off-by-one behavior is not silently introduced into this truth-isolated mode.
