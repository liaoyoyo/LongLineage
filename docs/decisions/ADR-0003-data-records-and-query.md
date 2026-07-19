# ADR-0003: Versioned Records and Validated-only Query

Status: Accepted

## Context

Readable files without stable keys, units, null semantics and provenance cannot be
audited or safely queried by humans or AI.

## Decision

Maintain a machine-readable schema catalog and reason-code vocabulary. Every artifact
defines key/order/fields/conservation/digest rules. `longlineage-query` rejects runs
that are not validated and frozen.

## Consequences

Schema changes require migrations and tests. Ad hoc Python aggregation is prohibited.
