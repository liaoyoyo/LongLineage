# Contributing

Read `LongLineage/AGENTS.md` and `LongLineage/docs/development/WORKFLOW.md` before
editing. Every change must identify its task type, LL-G goal, affected schema/claim,
and Step→Verify evidence.

Pull requests must:

- contain no real genomic data, coordinates, run output or credentials;
- add regression/negative tests before changing scientific behavior;
- update schema/ADR/implementation notes when a contract changes;
- keep unverified phases blocked;
- pass `LongLineage/scripts/ci/check_all.sh`.

Scientific behavior changes require two reviews: implementation and independent
validation. The validator may share schema/I/O code but must not link producer
statistical or solver kernels.
