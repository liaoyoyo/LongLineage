# ADR-0002: BGZF Native Artifacts and Legacy Export

Status: Accepted

## Context

The historical layout can create roughly 1.4 million small files and cannot be
published atomically at practical cost.

## Decision

Use versioned BGZF TSV/JSONL native artifacts with file and semantic digests. Rebuild
legacy layout only through `longlineage-export-legacy`.

## Consequences

Native query and validator code become mandatory. Legacy output is derived and never
an analysis input.
