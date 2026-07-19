# Query Guide

## Current implementation status (P6 not yet VERIFIED)

```bash
longlineage-query --run-root /path/to/run --artifact m1_sites \
  [--key FIELD=VALUE] [--limit N]
```

The scaffold currently performs only the `VALIDATED_FROZEN` receipt and
`truth_fields_seen == 0` gate. After that gate it deliberately returns
`QUERY_REJECTED` (exit 8), because the independent index/digest replay and record
reader are not implemented. It returns no rows and writes nothing. Therefore the
examples and operations below are the frozen **P6 target contract**, not commands
that are available in the P0/P1 scaffold.

## P6 target response

Every response includes:

- run ID and run state;
- artifact/schema name and version;
- normalized filter;
- matched row count and truncation flag;
- source artifact and index physical SHA-256;
- source artifact semantic SHA-256;
- frozen run, producer and validation receipt SHA-256 plus checksum-manifest SHA-256;
- query executable SHA-256 and schema catalog SHA-256.

The query tool never recomputes p-values, groups, FDR, topology or summaries. For a
new aggregate, add a C++ chart-ready producer plus schema and independent validator;
do not improvise in a notebook or presentation script.

## Allowed operations

After P6 is implemented and independently verified, the allow-list is
machine-readable in
`LongLineage/contracts/v1/query_operators.tsv`. Version 1 supports:

- artifact discovery and schema inspection;
- exact dataset/site/unit lookup through a validated site index;
- equality filters on declared scalar fields;
- inclusive numeric ranges on declared scalar fields;
- deterministic projection of existing columns;
- a bounded sequential scan only when `--allow-scan` is explicit.

The registry's `implementation_status` and `verification_status` are part of the
contract. In the current snapshot every operator is `PLANNED/NOT_VERIFIED`; this
prevents a schema-aware client from treating the allow-list as evidence that the
stub CLI can execute the operation. Promotion requires implementation evidence,
independent query fixtures and an updated SHA binding in
`LongLineage/schema/core/contract_registry_bindings.schema.json`.

All predicates are an AND-only normalized AST. The closed nodes are `eq(field,
value)`, `in(field, values[1..256])` and inclusive `range(field, lower, upper)`;
projection is stored separately as `projected_fields[1..256]`. Field names must
exist in the bound record schema and operand JSON types must match exactly. In
particular, the query layer does not coerce a string to an integer, a float to an
integer, `"."` to null or a status label to a synonym. Normalized predicates are
ordered by field then operator; output rows always retain canonical artifact order.
Nested JSON object properties use an unambiguous dot-separated field reference such
as `artifact.artifact_id`; array indexing, wildcards and keys containing dots are
not queryable in v1. Plain TSV fields retain their exact schema name such as
`site_order`.

There is no SQL, regex, expression evaluation, join, group-by, derived column,
imputation, sorting override, random sampling, p-value recomputation or topology
re-ranking. Results retain canonical artifact order.

## Safety limits

- Default limit: 100 rows; hard CLI limit: 10,000 rows.
- Scans report `scan_rows` and `scan_complete`. If the scan is incomplete,
  `matched_rows` is `null` and `truncated` is true; a partial count is never
  described as a census.
- The response includes a closed `query_plan`. `SITE_INDEX_EXACT` binds the
  receipted nested index SHA-256; `SEQUENTIAL_SCAN` requires `--allow-scan` and
  records its row budget.
- `SITE_INDEX_EXACT` is available only when equality predicates supply the complete
  catalog-declared `index_key` (for example `dataset_order + focal_site_order` for
  co-occurrence pairs). A partial key never pretends to be indexed.
- A query fails if the run receipt, validation receipt, checksums, artifact digest,
  catalog digest or reconstructed index binding is inconsistent.
- `RUNNING`, `FAILED`, `VALIDATED` staging roots and legacy exports are rejected.
- Query output is written to stdout unless an explicit new output path is supplied;
  the run root is opened read-only and never locked for mutation.

The response envelope follows
`LongLineage/schema/core/query_response.schema.json`. Exit codes are defined in
`LongLineage/contracts/v1/cli_exit_codes.tsv`.

Synthetic contract fixtures are
`LongLineage/tests/fixtures/contracts/query_response.valid.json` and
`LongLineage/tests/fixtures/contracts/query_response.invalid_partial_count.json`.
The latter must fail because an incomplete scan cannot publish a numeric
`matched_rows`.
