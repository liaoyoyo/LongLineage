# Data Contracts

The machine-readable source of truth is `LongLineage/schema/catalog.json`. Human
documentation is explanatory; when it conflicts with a schema or registry, the
machine-readable contract wins and the conflict is a release blocker.

## Identity

- `Position1`: positive 1-based genomic position.
- `Interval0`: 0-based half-open `[begin,end)`.
- `ReadProjectionIdentity`: QNAME, contig, start0, end0, MAPQ, strand.
- `FullAlignmentIdentity`: projection plus FLAG, CIGAR and typed auxiliary tags.
- Sidecar full key: QNAME, contig, start0, end0, FLAG, CIGAR_B2.

An absent projection is `MISSING_PROJECTION`; a projection that exists but whose
FLAG/CIGAR differs is `FULL_IDENTITY_MISMATCH`. Conflicting HP/PS values and
multiple distinct full identities retain separate reasons. None may fall back to an
older BAM tag.

## Allele vocabulary

- `R`: REF observed.
- `A`: ALT observed.
- `O`: another base observed.
- `X`: not observable.

O/X are retained in four-state and callability records and never recoded as REF.
Pair records store the complete focal-by-partner 4×4 matrix in row-major
`RR,RA,RO,RX,AR,...,XX` order. A single `o_count` or `x_count` is forbidden because
cells such as `OX` otherwise have no unambiguous denominator or precedence.

## Latest HP and methylation vocabularies

Upstream LongPhase-S HP is closed to
`. / 1 / 2 / 3 / 4 / 1-1 / 2-1 / 1-2 / 2-2`. Native records normalize upstream `.`
to explicit `0`; no other spelling is changed or inferred. HP family is a separate
derived field and may only be added after its mapping is versioned in the transform
registry.

Methylation v1 accepts exactly one `C+m?` MM group. Therefore native
`methyl_calls.mm_skip_mode` is always `QUESTION` and `skip_semantics` is always
`UNKNOWN`. Dot/unspecified skip modes are rejected instead of being silently treated
as the question-mark contract. ML byte `N` records the interval
`[N/256,(N+1)/256)`; it is not converted to a midpoint probability.

## Parse status

Every parser returns value plus `status` and `reason`. Empty data is represented by
`OK_EMPTY`, not by a generic empty container. Errors use a stable reason code from
`LongLineage/contracts/v1/status_reason_codes.tsv`.

## Input authority profiles

Every manifest names one closed authority profile:

- `PRODUCTION_7_DATASET`: exactly the seven frozen datasets, in frozen order. The
  sidecar/index and PASS VCF/index size and SHA-256 values must match
  `LongLineage/oracle/production_input_authority.json`; a role label alone is never
  sufficient. `longlineage run` accepts only this profile.
- `SYNTHETIC`: reproducible `synchr*` fixtures for preflight, probe and tests. It can
  never enter a production run or create validation evidence.

Both profiles still full-SHA-lock every manifest input. Production raw BAM, BAM
index, reference and FAI are locked by the run manifest because the upstream closeout
did not provide full-content hashes for the multi-hundred-gigabyte BAM/reference
files. A release run recomputes those full hashes before and after analysis.

## Physical record rules

- UTF-8, LF, no NUL.
- TSV BGZF files begin with the exact metadata preamble below, followed by the exact
  tab-delimited header declared by the record schema:

  ```text
  ##longlineage_schema=<schema_name>
  ##schema_version=<schema_version>
  ##run_id=<run_id>
  #<field_1>\t<field_2>...
  ```

- Missing values use `.` only where the field dictionary permits.
- Keys are unique and sorted by frozen dataset/VCF order.
- Floats use locale-independent `LONGLINEAGE_SCI17`: one decimal digit, `.`, exactly
  sixteen fractional digits, lowercase `e`, a mandatory sign and a three-digit
  exponent (for example `1.2500000000000000e-003`). This is 17 significant digits,
  round-trips binary64 and is independent of a standard library's “shortest”
  algorithm. NaN, Inf and negative zero are forbidden; positive zero is
  `0.0000000000000000e+000`.
- Booleans are serialized as `true` or `false`; integers have no leading `+` or
  insignificant leading zero.
- JSONL requires one UTF-8 object per LF-terminated line and explicit schema
  name/version in every object. Duplicate JSON keys and non-finite numbers are errors.
- Every BGZF artifact must contain the canonical 28-byte BGZF EOF marker. A normal
  gzip stream, concatenated garbage, or a missing EOF marker is invalid.
- BGZF physical bytes have a file SHA; logical records have a separate semantic SHA.

## Semantic SHA-256

The logical digest is computed after parsing and validating the physical encoding:

1. emit `schema_name<TAB>schema_version<LF>`;
2. emit the exact schema header joined by TAB and terminated by LF;
3. emit every record in declared sort order, using schema field order and the scalar
   serialization above, terminated by LF;
4. hash those bytes with SHA-256.

For JSON/JSONL, keys are emitted in the order declared by `required` followed by any
optional properties in schema-property order; strings use JSON escaping with no
insignificant whitespace and floating numbers use `LONGLINEAGE_SCI17`. Arrays retain
order. The validator independently constructs this stream. Compression level, BGZF
block boundaries, worker count, filesystem path and timestamps therefore cannot
change the semantic digest.

## Site indexes

Queryable multi-record artifacts have a sibling
`indexes/<artifact_id>.site_index.tsv.bgz`. Each row binds a frozen site or unit key to
the first and past-the-end BGZF virtual offsets, logical row count, and row-range
semantic SHA. The exact schema is
`LongLineage/schema/records/site_index.record.json`.

Indexes are navigation aids, never authorities. The independent validator reconstructs
each index from the artifact and rejects overlap, gaps, out-of-order keys, wrong row
counts, invalid virtual offsets, or mismatched range digests. Query output always cites
both artifact and index physical SHA-256.

## Record lineage

Every artifact entry in `run_receipt.json` declares:

- producer executable digest and transform ID;
- typed input bindings: source kind, stable source ID, digest kind and SHA-256;
- record schema name/version;
- physical and semantic SHA-256;
- a closed nested index binding (path, schema/version, row count and both digests) or
  explicit `null`;
- logical row count, primary-key range and sensitivity class.

Transforms are allow-listed by
`LongLineage/contracts/v1/transform_registry.tsv`. Unknown transforms or schema
versions fail closed.

Repository registries are not free-standing prose. The immutable const contract
`LongLineage/schema/core/contract_registry_bindings.schema.json` binds the physical
SHA-256 of `schema/catalog.json`, `artifact_roles.tsv`, `lifecycle_codes.tsv`,
`transform_registry.tsv` and `query_operators.tsv`. For artifact rows, the required
set relation is:

- every `binding_scope=ARTIFACT` role equals one catalog `artifact_id`, with no
  missing or extra ID;
- the sole `SUBORDINATE_INDEX` role is `site_index`, bound to the catalog's
  `site_index_schema`;
- the transform and query registries are exact-byte SHA locks.

The binding schema itself is offline-ID registered and SHA-locked by
`schema/id_registry.json`. A registry edit therefore requires an intentional binding
schema and ID-registry digest update plus an exact row-set replay. P6 remains blocked
until the independent C++ validator/query executable performs the same replay at
runtime; the contract does not claim that stub CLIs already enforce it.

Input bindings use `RUN_ARTIFACT + SEMANTIC_SHA256` for an upstream artifact in the
same run, and `MANIFEST_INPUT/CONTRACT + PHYSICAL_SHA256` for locked external bytes.
A bare digest without a source ID is invalid because it cannot support reverse
lineage queries. The validator checks that the transform graph is acyclic and that
every reference resolves to the immutable producer receipt or manifest.

## Embedded canonical JSON

`canonical_json` defines physical JSON canonicalization only. Every scientific
embedded JSON field additionally names an offline nested schema ID, path and
physical schema SHA in its parent record's `embedded_json_bindings`:

- `group_allele_counts_json` is an ordered array of `[REF_count, ALT_count]` rows in
  canonical M1 group order. Its length, matrix total, column totals, minimum row
  total and canonical JSON SHA must equal the declared companion fields.
- `compatible_relation_models_json` is one of the eight closed subsets of
  `FOCAL_FIRST`, `PARTNER_FIRST`, `BRANCHING` in that fixed order. Its length equals
  `n_compatible_relation_models`; these labels describe compatibility, not proven
  ancestry or time.
- `joint_partner_orders_json` is one to three unique frozen
  `partner_site_order` values. Presence is equivalent to
  `joint_signature_status=PASS`, its nullability matches the complete-read-set
  digest, and referenced positions must pass the declared 20 bp spacing lookup.

The nested schemas are separately offline-ID registered. A syntactically canonical
but shape-invalid value is rejected; changing a nested shape or model vocabulary
requires a new schema version and parent-record hash.

## Cross-field semantic groups

Tabular scalar fields can form a typed semantic group. `site_reads.start0/end0` are
bound by the machine-readable `semantic_groups` entry
`alignment_reference_interval` with:

- `semantic_type=Interval0`;
- `coordinate_system=ZERO_BASED_HALF_OPEN`;
- `members.begin=start0`, `members.end=end0`;
- required relation `LT(start0,end0)`;
- producer and independent-validator enforcement requirements.

The prose invariant remains explanatory; the semantic group is the machine contract.
Equal or reversed endpoints are negative fixtures. P1 typed I/O enforces the
relation in C++; P6 is still blocked until the independent artifact validator replays
it over serialized rows.

## External regional input path map

Private physical paths for the descriptive seven-dataset endpoint live only in an
untracked `longlineage.regional_compat_input_paths@1.0.0` JSON file. Its closed
contract is `schema/compat/regional_compat_input_paths.schema.json`; a safe,
regenerable example is `tests/fixtures/regional_compat/input_paths.valid.json`.

The primary key is `(dataset_id, dataset_order, role)`. Dataset and role order are
fixed by the schema, so consumers query one path without positional guessing:

```bash
jq -er --arg dataset H2009 --arg role raw_bam \
  'first(.datasets[] | select(.dataset_id == $dataset) |
         .files[] | select(.role == $role) | .path)' PRIVATE_PATH_MAP.json
```

The two authority/manifest builders require `--input-paths`, reject unknown fields,
relative or truth-bearing paths, and bind the map's SHA-256 before reading. They
replay the SHA immediately before atomic publication and print only
`INPUT_PATH_MAP_SHA256`; the private map pathname and individual paths must not be
copied into Git, task evidence or public logs. The final restricted run manifest may
contain physical paths because it remains outside Git and is independently frozen.

## Source-port lifecycle

`provenance/source_to_target_manifest.json` uses a closed, independently validated
lifecycle:

- implementation: `PLANNED`, `SKELETON`, `IMPLEMENTED`;
- verification: `NOT_VERIFIED`, `CONTRACT_VERIFIED`, `PARITY_VERIFIED`;
- target kind/presence: `FILE|DIRECTORY` and `ABSENT|PRESENT`;
- digest kind: `FILE_SHA256`, `TREE_SHA256`, or JSON `null`.

`PLANNED` requires an absent target, null digest and no evidence. `SKELETON` requires
a present, digested target but cannot claim verification. A verified mapping must be
`IMPLEMENTED`, have a target digest and name a stable evidence gate ID. Thus a listed
port is never interpreted as completed merely because a target string exists.

The transform and query registries carry the same implementation/verification axes.
Their vocabulary is closed by `contracts/v1/lifecycle_codes.tsv`.
At this snapshot, P3–P6 scientific/query transforms remain planned or skeleton and
not verified.

Site indexes are subordinate bindings, not independent scientific artifacts. The
parent artifact record must contain a non-null index exactly when the schema catalog
declares one, and its path must match the catalog. This prevents a query tool from
substituting an unreceipted index while keeping index files out of the scientific
artifact namespace.

To avoid self-referential hashes, `artifact_catalog.jsonl.bgz` mirrors the immutable
scientific producer artifacts but excludes itself and receipts. The closeout order is:

1. producer writes artifacts, catalog, lineage and semantic digests;
2. producer writes immutable `receipts/producer_receipt.json` with
   `producer_outcome=READY_FOR_VALIDATION` while the run state remains `RUNNING`
   (or a terminal `FAILED` outcome with a reason);
3. producer writes `checksums.sha256`, covering every producer file except itself;
4. independent validator reopens inputs and artifacts, then writes
   `validation_receipt.json`;
5. the validator's freeze step writes final `run_receipt.json`, binding the physical
   SHA-256 of the producer receipt, checksum manifest and validation receipt;
6. that same executable performs one atomic rename and publishes the root as
   `VALIDATED_FROZEN`.

No receipt is edited in place and no digest cycle exists. A failed validator leaves a
FAILED staging root and cannot create the final receipt.

The final receipt calls the original binary `production_executable`; the file itself
is authored by `longlineage-validate`. `longlineage-query` independently replays the
receipt/checksum chain before returning any row.

## Count grains and denominators

Field names include the counting unit. In particular, topology uses two different
grains that must not be combined:

- `topology_primary_hp_units`: HP-lineage units (full target: 72,994);
- `topology_regions`: genomic analysis regions (full target: 50,215);
- `topology_fully_complete_regions` and `topology_incomplete_regions` partition
  regions (42,240 + 7,975 = 50,215), not HP-lineage units.

Likewise, `raw_expected` counts canonical occurrences before permitted RG-only
duplication, while `raw_matched` includes those duplicate occurrences. The required
conservation is 9,356,980 + 196,706 = 9,553,686. Metric labels without an explicit
grain and denominator are schema errors, not presentation choices.

Topology units retain canonical `FULL_STATE` and `PARTIAL_SUBCUBE` input patterns
with multiplicity. A per-unit evidence digest binds the exact upstream row
membership and pattern stream; a second digest binds the sufficient statistics used
for parent-edge scoring. Artifact-level lineage alone does not identify which input
rows formed a topology unit.

## Lifecycle

`RUNNING → FAILED` or
`RUNNING → VALIDATED → VALIDATED_FROZEN`.

Receipt files are append-final, never patched into PASS. Atomic rename publishes the
validated staging root; failure evidence remains outside the final namespace.

The only production staging spelling is `<base>/.staging/<run_id>`, and manifest
`output_root` names that exact absolute path. After validation, the only final path is
`<base>/<run_id>`. Alternate suffix conventions, relative paths, `..`, a run-ID
mismatch, pre-existing symlink aliases, or direct writes to the final path fail
preflight.

Schema compatibility follows SemVer. Adding an optional field is minor; changing
meaning, unit, nullability, identity, ordering, enum semantics, or denominator is
major. Historical schemas remain readable and are never edited in place.
