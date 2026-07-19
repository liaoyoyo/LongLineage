# ADR-0001: Production, Evaluation and Presentation Trust Boundaries

Status: Accepted

## Context

Truth-aware inputs or presentation-time recomputation can leak benchmark knowledge
into scientific selection.

## Decision

Production C++ accepts no truth. `longlineage-evaluate` runs only after
`VALIDATED_FROZEN`. Presentation reads only validated chart-ready artifacts.

## Consequences

Benchmark and display logic cannot improve or change production results. Additional
artifacts require a producer schema and validator before use.
