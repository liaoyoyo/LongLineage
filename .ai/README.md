# AI Working Environment

This directory is the concise AI entry point. `LongLineage/AGENTS.md` remains the
governance authority.

Start every session:

```bash
scripts/ai/check_readiness.sh
```

Then read, in order:

1. `LongLineage/AGENTS.md`
2. `LongLineage/docs/CURRENT_FOCUS.md`
3. `LongLineage/docs/development/implementation-notes.md`
4. affected ADR/schema/test
5. `git log --oneline -10` and `git status --short`

AI agents must not infer completed phases from source presence. Query
`LongLineage/state/phase_ledger.json` and reproduce the referenced evidence.

Create active work from `.ai/templates/task.json`, validate it against
`governance/agent_task.schema.json`, and store it under `state/tasks/active/`.
The task record is the machine-readable handoff between agents; implementation notes
remain the human-readable decision log.
