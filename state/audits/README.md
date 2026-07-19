# Machine Audit Envelopes

Immutable audit envelopes live directly in this directory as
`<snapshot_id>.json` and validate against
`governance/audit_evidence.schema.json`.

Do not edit an existing envelope. A replacement names the older snapshot in
`supersedes`; compiled governance derives reverse edges, rejects cycles and accepts
exactly one current tip per task and scope.

An envelope records evidence for a source snapshot; it does not promote a task or a
P0–P8 phase by itself. Phase and task ledgers must bind its repository-relative path
and physical SHA-256 explicitly.
