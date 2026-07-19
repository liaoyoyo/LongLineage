# Agent Task Records

Active task records live in `state/tasks/active/` and validate against
`governance/agent_task.schema.json`. A task moves to `state/tasks/archive/` only after
its evidence has been independently replayed; the JSON status is set to `VERIFIED`
before the move.

These records contain repository-relative paths and digests only. Real dataset paths,
coordinates, credentials and run payloads belong in the restricted run root, not Git.
Task status never upgrades a scientific phase by itself.

## Required concurrency fields

Every record carries:

- `owner_agent_id` and `parent_task_id`;
- exclusive, typed `allowed_paths` without globs or `..`;
- acyclic `depends_on`;
- `lease_state`, `lease_expires_at` and `heartbeat_at`.

`IN_PROGRESS` requires an assigned owner, at least one write claim and an ACTIVE
lease. A worker must stop writing when its lease expires. Compiled governance rejects
expired leases, unmet dependencies and overlap between live write claims; JSON Schema
rejects missing fields, absolute/traversing paths and illegal status/lease shapes.

The fixed runtime policy is: lease duration at most 86,400 seconds, heartbeat age at
most 3,600 seconds and future clock skew at most 300 seconds. Tracked baseline state
must not contain a one-off ACTIVE lease that will expire in CI; conclude unfinished
work as `BLOCKED + RELEASED`, then issue a fresh lease when work resumes.

Use a narrow `FILE` claim when one file is sufficient. Use `DIRECTORY_TREE` only when
the task genuinely owns every descendant. A parent task must release or narrow a
claim before delegating an overlapping child claim.

## Completion and audit lookup

The `evidence` array is completion evidence, not a note field. `VERIFIED` requires at
least one independently replayed row with `exit_code=0`, an existing
repository-relative evidence path and matching SHA-256.

Repository audit snapshots use `governance/audit_evidence.schema.json`. Query them by
`task_id` and scope, validate their source tree and command-output digests, build the
`supersedes` DAG, and accept exactly one non-superseded tip. Never select a result by
filename date or prose search alone.
