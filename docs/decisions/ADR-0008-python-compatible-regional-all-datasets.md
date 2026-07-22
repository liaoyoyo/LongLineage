# ADR-0008: Governed seven-dataset regional compatibility execution

Status: Accepted for implementation; full execution pending authority and resource gates

## Context

ADR-0007 established a separate descriptive endpoint and HCC1395 exact Python-to-C++ parity.
The next requirement is to run the same method for the exact seven-dataset production authority.
The scientific kernel is already dataset-neutral, but the HCC entry point, receipts and validator
do not yet bind dataset identity or independently prove all manifest input digests.

Six raw BAM files have only sampled storage identities in the existing capture receipts. Those
identities are useful change-detection evidence but are not full-content SHA-256 and cannot satisfy
the LongLineage physical input-lock contract.

## Decision

### 1. Keep one descriptive science profile

The endpoint remains `PYTHON_V2_DESCRIPTIVE_REGIONAL`. No grouping, read projection, HP-family,
MINREAD=3 or legacy solver rule changes. The seven-dataset work is an I/O, authority, provenance,
validator and orchestration generalization—not a new biological method.

### 2. Select one dataset from an exact seven-dataset source manifest

New full runs use `authority_profile=PRODUCTION_7_DATASET` and require an explicit dataset ID.
The manifest must contain exactly, in order:

1. HCC1395
2. HCC1395_DORADO
3. COLO829
4. H1437
5. H2009
6. HCC1937
7. HCC1954

The producer verifies the repository contract binding and `oracle/production_input_authority.json`,
then selects exactly one row. Legacy `HCC1395_DATASET_GATE` remains accepted only for backward
compatibility and must still bind its dedicated HCC authority. `SYNTHETIC` cannot enter full run.

### 3. Recompute selected physical input hashes

Before science, all eight selected roles are verified with `verify_locked_file()`:

- raw BAM and BAI;
- PASS biallelic sSNV VCF and CSI;
- latest truth-free HP/PS sidecar and TBI;
- reference FASTA and FAI.

The producer records full-SHA verification timing separately from science timing. It takes a rich
canonical-path/device/inode/size/mtime/ctime snapshot and replays it after science. Production
manifest parsing rejects every path containing a symbolic-link component. Repository scripts
therefore require an external untracked `longlineage.regional_compat_input_paths@1.0.0` map whose
rows use canonical regular-file paths. Neither aliases nor canonical private paths are embedded in
Git. A sampled storage identity is never accepted in a physical SHA field. Because the six non-HCC
raw BAM hashes did not exist at first capture, their first full scan was an authority-building cost,
not solver runtime.

### 4. Introduce a closed multi-dataset receipt contract

New bundles use compat schema version 2.0.0 and bind the following in summary, producer receipt,
semantic digest, validation receipt and FROZEN marker:

- dataset ID and order;
- source authority profile and production authority SHA;
- source manifest SHA/run ID;
- exact eight input roles, sizes and verified physical SHA values;
- frozen parameters and explicit claim ceiling;
- preflight, science, writer and total timings.

The independent validator supports the already frozen 1.0.0 HCC bundle as a read-only legacy
contract, but only a fully closed 2.0.0 bundle may represent a new seven-dataset execution.

### 5. Fail closed at both dataset and cohort levels

Each dataset produces an independent flat bundle. The producer can only emit
`READY_FOR_VALIDATION`; the separate validator replays schema, dataset bindings, checksums, TSV
types/order/foreign keys and TOCTOU before publishing `VALIDATED_FROZEN`.

The cohort wrapper runs in authority order, one 24-worker sample at a time. It publishes a cohort
receipt only when all seven child validation receipts and FROZEN bindings pass. Partial completion
is recorded honestly but cannot be labeled full seven-dataset validation.

### 6. Compare only descriptive fields to frozen Python

All seven 2026-07-13 frozen Python outputs are available. A C++ crosswalk may compare grouping,
selected loci, family/read/pattern membership, solver fields, unit classes and determinacy. CN/LOH
post-tree annotations are outside this production-compatible comparison. Python may render charts
and standalone HTML only after consuming C++-validated chart-ready receipts.

Cross-language parity and controlled speedup are separate claims. A zero-mismatch crosswalk does
not make the historical Python scheduler time a controlled benchmark. The current crosswalk receipt
is produced by the compatibility executable and is hash-bound to both frozen corpora, but has no
second independent validator/FROZEN lifecycle; cohort output must therefore state
`crosswalk_receipts_independently_frozen=false`.

## Rejected alternatives

- **Reuse HCC label for every sample**: rejects real identity and permits bundle confusion.
- **Create seven special authority profiles**: duplicates policy and invites drift; the exact
  production seven-dataset authority already exists.
- **Use sampled BAM identity as SHA**: cryptographically and semantically false.
- **Run samples concurrently at 24 workers**: exceeds 48 logical CPUs and worsens shared NFS I/O.
- **Let Python recompute cohort science**: violates the C++ science boundary and creates a second
  implementation to reconcile.
- **Relax formal M2 because descriptive regions are nonzero**: compares different endpoints and
  violates the claim boundary.

## Acceptance gates

1. Negative tests reject unknown/reordered/missing datasets, wrong authority digest, missing role,
   wrong physical SHA, ungoverned alias/target swap and any truth-bearing manifest content.
2. Validator rejects missing/unknown summary fields, duplicate input roles, mismatched dataset
   identities and re-sealed bundle swaps.
3. Existing HCC frozen 1.0.0 validation remains readable; a new HCC run from the full manifest has
   byte-identical science TSVs.
4. Dataset-neutral parallel machinery passes synthetic w1/w24 determinism, and the same HCC1395 BAM
   bounded region set remains byte-identical at w1/w24; the seven full w24 runs are each checked
   against their frozen Python map rather than paying seven extra full-input SHA scans for probes.
5. Seven full w24 runs independently freeze and the C++ Python crosswalk reports zero mismatch.
6. Cohort HTML is generated only from validated C++ receipt data and states resource/cache limits.

## Consequences

End-to-end provenance becomes slower on the first run. Authority creation physically hashes the six
previously unfrozen BAMs (about 1.65 TB); the seven governed producers then independently rehash all
seven BAMs (about 1.94 TB) before science. Those costs are intentional and reported separately.
Subsequent scientific runtime remains dominated by indexed NFS BAM access, while the topology solver
remains a small fraction of wall time. The work strengthens reproducibility without changing the
scientific census.
