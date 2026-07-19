# Authority Map

Machine-readable authority: `LongLineage/oracle/authority_manifest.json`.

Key boundaries:

- `LongLineage/oracle/production_input_authority.json` binds the exact seven-dataset
  order plus sidecar/index and PASS VCF/index sizes and SHA-256 values from the 7/7
  closeout. It stores no private source path and records that no tagged BAM was
  persisted.
- M1 frozen screen is a historical Python authority pending C++ parity.
- M2 eligibility census is validated, but it is not formal co-occurrence evidence.
- No formal full co-occurrence pair/candidate authority currently exists.
- Hypercube solver source hash is frozen; current scale status remains
  `PASS_FOR_PROBE`.
- Frozen-v2 real M2 ranking is NO-GO because the terminal receipt is absent.

LongLineage never executes these Python sources. It records their hashes and compares
C++ outputs with separately frozen vectors.

Machine-readable source-port lifecycle is
`LongLineage/provenance/source_to_target_manifest.json`, validated by offline schema
ID `https://longlineage.local/schema/source_to_target_manifest-1.1.0.json`.
`PLANNED`, `SKELETON` and `IMPLEMENTED` are distinct from
`NOT_VERIFIED`, `CONTRACT_VERIFIED` and `PARITY_VERIFIED`; target presence, kind,
digest kind/SHA and evidence ID follow closed conditional nullability rules. Current
P3–P6 entries remain planned or skeleton unless their target and evidence genuinely
exist.
