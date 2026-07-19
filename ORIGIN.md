# Source Origin and Scientific Authority

LongLineage has a new Git history. It is a clean C++ implementation whose behavior is
defined by frozen InterSubMod sources, artifacts and decision contracts.

The scientific authority registry is
`LongLineage/oracle/authority_manifest.json`; the implementation mapping is
`LongLineage/provenance/source_to_target_manifest.json`. Together they record:

- canonical source role and repository-relative path;
- SHA-256 of the reviewed source/artifact;
- target module and parity gate;
- authority class (`VALIDATED_FULL`, `VALIDATED_INTEGRITY`,
  `REVIEWED_KNOWLEDGE`, `UNRELEASED_REFERENCE`, `PASS_FOR_PROBE` or `NO_GO`);
- claim and license notes.

The sanitized production-input lock is
`LongLineage/oracle/production_input_authority.json`. It retains only dataset order,
counts, receipt/artifact sizes and SHA-256 values from the 7/7 closeout; it contains
no private source path, read name or genomic coordinate.

Historical Python science is never executed by LongLineage. It is used only to create
and hash frozen oracle vectors outside production. Before any public release, every
copied/derived source fragment and dependency must pass a GPL/source-origin audit.
