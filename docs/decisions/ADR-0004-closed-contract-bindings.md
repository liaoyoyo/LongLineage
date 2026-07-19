# ADR-0004: Closed Registry, Embedded JSON and Semantic-group Bindings

Status: Accepted

## Context

A valid catalog and a valid side registry can still drift independently. Likewise,
`canonical_json` proves only parse/canonical form, and two valid integer fields do
not by themselves prove an `Interval0` relation. A source-to-target row also cannot
mean “implemented” merely because its planned target path is named.

## Decision

1. Register a const JSON Schema that SHA-binds the catalog and artifact, transform
   and query registries. Artifact-scope role IDs must equal catalog artifact IDs;
   `site_index` is the single declared subordinate role.
2. Bind each scientific `canonical_json` field to a separately versioned, offline
   nested schema plus companion-field conservation operators.
3. Represent serialized cross-field value types with machine-readable
   `semantic_groups`; `site_reads.start0/end0` is required `Interval0` with
   `LT(start0,end0)`.
4. Give source ports and executable transforms independent closed implementation and
   verification lifecycles. Planned targets have null digests/evidence; verified
   targets require stable evidence.

## Consequences

Any catalog/registry/nested-schema change requires deliberate SHA closure and
positive/negative fixture replay. The current contract layer is queryable offline,
but this ADR does not promote P3–P6: independent runtime validator/query enforcement,
scientific parity and full-run validation remain release gates.
