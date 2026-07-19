# AI Development Workflow

## Session start

Run:

```bash
scripts/ai/check_readiness.sh
```

The script verifies required SoT files, phase ledger syntax, schema catalog, git
state and build-tool availability. A warning is not permission to bypass a gate.

## Change discipline

AI work must preserve this chain:

`user/spec → ADR/schema → failing test → implementation → independent validation → receipt`

When a change touches two or more of RNG, statistics, identity, coordinate,
denominator, status precedence or topology, append a living-note decision before
editing code.

Subagents receive bounded inputs and must return paths, commands, digests and
limitations. The primary agent independently reruns critical evidence.

Every nontrivial task has a repository-relative JSON record under
`state/tasks/active/`, validated by `governance/agent_task.schema.json`. Status may
advance to `VERIFIED` only after the primary agent replays the declared commands.

## Ownership, write sets and leases

Before any edit, every worker must have one active task record containing:

- a framework-qualified `owner_agent_id` and nullable `parent_task_id`;
- non-glob, repository-relative `allowed_paths`, each typed as `FILE` or
  `DIRECTORY_TREE`;
- an acyclic `depends_on` list whose members are already `VERIFIED`;
- `lease_state=ACTIVE`, a future `lease_expires_at`, and a recent `heartbeat_at`.

The owner must heartbeat before the lease expires. An expired or released lease
authorizes no write. A handoff changes the owner and issues a new lease; it does not
silently reuse the previous worker identity. Active write sets must not overlap:
equal file claims, equal directory claims, or a directory/descendant pair are
conflicts. JSON Schema closes each record shape; compiled governance is responsible
for time comparison, dependency lookup, path normalization and cross-task overlap.

The compiled lease policy is closed at a 24-hour maximum lease, a one-hour maximum
heartbeat age and five minutes of future clock skew. `PLANNED` tasks are unowned;
`IN_PROGRESS` tasks require a recent ACTIVE lease; `BLOCKED`/`FAILED` tasks release
or expire it. `VERIFIED` records move to `state/tasks/archive/`. The checker loads
both directories, rejects missing dependencies/parents and cycles, and checks all
live write claims after lexical normalization and symlink inspection.

`evidence` is the completion-evidence list. A task can become `VERIFIED` only when it
contains at least one independently replayed evidence row with `exit_code=0`. Task
completion does not promote a scientific phase.

## Phase projection

`state/project_state.json.open_gates` is not a manually curated summary. It is the
exact gate-ID projection of every P0-P8 row in `state/phase_ledger.json` whose status
is not `VERIFIED`. The compiled governance check recomputes this set and rejects
missing or extra gates. Serialization uses canonical P0-P8 order.

## Immutable audit evidence

Machine-consumable audit snapshots validate against
`governance/audit_evidence.schema.json`. Each envelope binds:

- a stable `snapshot_id` and owning `task_id`;
- explicit FULL/PARTIAL scope and included/excluded repository-relative paths;
- source snapshot kind, optional Git commit and mandatory canonical tree SHA-256;
- exact command argv, working directory, exit code, output digests and timestamps;
- capture agent/time and machine-readable `supersedes`/`superseded_by` fields.

Audit envelopes are immutable. A new snapshot declares old IDs in `supersedes`;
consumers derive reverse `superseded_by` edges from the complete envelope set and
reject cycles or self-edges. The stored `superseded_by` value is only a capture-time
hint and must agree with the derived graph if non-null. A phase may cite an audit only
through the envelope's repository-relative path and physical SHA-256; mutable prose
alone is not phase evidence.

Envelopes live under `state/audits/`. Compiled governance validates closed fields,
command IDs/argv/timestamps, task identity, physical ledger binding, supersession
edges/cycles and one current tip per task/scope. A `VERIFIED` phase requires an
all-passing machine envelope. Human-readable Markdown is a `REPORT`, never an
`AUDIT`.

For `GIT_COMMIT` envelopes, replay the canonical source tree independently:

```bash
scripts/ci/check_audit_source_snapshot.sh state/audits/<snapshot_id>.json
```

The script resolves the commit locally and hashes sorted
`path<TAB>blob_bytes<TAB>blob_sha256<LF>` rows selected by the envelope scope. It
does not execute recorded argv; command output digests are compared only during an
explicit, separately authorized replay.

To query the current audit for a task/scope, validate every candidate envelope,
recompute its tree and output digests, construct the supersession DAG, and select the
single node with no incoming `supersedes` edge. Zero or multiple current nodes is
fail-closed.

Contract smoke checks:

```bash
/usr/bin/jsonschema -i state/project_state.json \
  governance/project_state.schema.json
/usr/bin/jsonschema -i state/tasks/active/20260719-foundation.json \
  governance/agent_task.schema.json
/usr/bin/jsonschema -i tests/fixtures/governance/audit_evidence.valid.json \
  governance/audit_evidence.schema.json
```

## Context recovery

If context is compacted or a new session starts, do not restart or infer. Read:

1. CURRENT_FOCUS
2. phase ledger
3. implementation notes
4. latest ADRs
5. `git status` and recent commits

## Protected decisions

Truth isolation, Python science prohibition, latest-sidecar-only HP/PS, incomplete
no-winner, and claim ceiling are user-protected decisions. AI may propose an ADR to
supersede them but cannot change them without explicit user approval.

Definition of Done is maintained in
`LongLineage/docs/development/DEFINITION_OF_DONE.md`; phase gates are stricter and take
precedence.
