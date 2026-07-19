# ADR-0005: Separate Topology Objective, Candidate Family and Abundance Ranking

Status: Accepted

## Context

The structural solver and abundance ranker answer different questions. A certified
minimum hidden-node objective does not prove that every minimum vertex set has been
enumerated, and a complete candidate family does not prove that abundance ranking is
complete or numerically certified.

The 2026-07-19 InterSubMod handoff is bound by SHA-256
`7b412e89561924f0dff95a42bc69319b6d7e296ab30792cbe236a1bd16feb306`.
Its R3 authority pointer, hard-25 receipt and compressed-ranking receipt were replayed
read-only before this decision. They establish:

- optimized hard-25 objective certification is 25/25, while family completion is
  16/25 and nine units remain incomplete;
- no incomplete family was sent to ranking;
- the small exhaustive abundance oracle passes, but ordinary floating-point upper
  bounds do not provide a machine-certified pruning certificate;
- production promotion and an overall speedup claim remain `NO_GO`;
- the current primary endpoint is a base-quality-aware read-pattern mixture
  likelihood evaluated once per candidate vertex set;
- that endpoint does not identify different parent-edge assignments within the same
  vertex set.

The existing `topology_unit` 1.0.0 record has only objective and family states. Its
`winner`, `parent_score_evidence_sha256` and additive edge-score fields can therefore
conflate structural uniqueness, vertex-set ranking and parent-edge selection.

## Decision

1. Treat `objective_state`, `family_state` and `ranking_state` as independent,
   mandatory state machines. No downstream code may infer a later state from an
   earlier one.
2. Start primary abundance ranking only after the candidate family is complete, or
   after a separately specified exact compressed representation proves the same
   complete family and tie class.
3. Freeze the primary endpoint as
   `BQ_AWARE_READ_PATTERN_MIXTURE_V1`. Score each distinct candidate vertex set once.
   The score may select a vertex-set tie class; it must not be reused to choose
   parent edges inside a selected vertex set.
4. Keep the historical scalar read-AF/VAF monotonicity heuristic as a separately
   named sensitivity endpoint. It cannot be merged into the primary likelihood or
   silently used as a tie-break.
5. A pruned ranking is publishable only when every exclusion is backed by replayable
   outward-rounded interval certificates. Ordinary floating-point bounds may order
   work or produce diagnostics, but cannot exclude candidates from a published
   winner or tie class.
6. Keep legal-parent enumeration and exact tree counts as structural outputs.
   Structural uniqueness may be reported independently from abundance ranking.
7. Keep explicit candidate enumeration as the current artifact contract. An exact
   compressed family/count/tie representation requires a new ADR, schema version,
   independent expansion/count oracle and query semantics. A large family may be
   output-sensitive; a cap or deadline remains incomplete and produces no rank.
8. Keep Evo-M0 recurrence-allowed as primary. Directed-hypercube compatibility is
   not strict infinite-sites, and molecule/read states are not clone, ancestor,
   temporal or cellular-lineage proof.
9. Do not implement the production ranker against `topology_unit` 1.0.0. First
   migrate the record schema and fixtures so the three state machines and endpoint
   boundaries are machine-enforced.

## Required machine invariants for the schema migration

- any incomplete or abstaining family has `published_rank = null`;
- a complete ranking binds an exact ranking-evidence digest, complete best-score tie
  class and either exhaustive evaluation or outward-rounded interval certificates;
- numerical-certificate failure has no published best candidate or tie class;
- every published vertex-set digest resolves to exactly one complete-family entry;
- a vertex-set score occurs once per distinct vertex set and carries no parent
  mapping;
- parent mappings are structural records, with exact legal-parent and tree counts;
- scalar monotonicity and BQ-aware mixture evidence use distinct endpoint IDs,
  digests, receipts and status fields;
- cap/deadline/incomplete cases never produce a rank or a structural tree winner.

## Consequences

The current q<=4 exhaustive C++ oracle remains valid as bounded structural evidence:
it certifies objective, complete minimum vertex sets, legal parents and tree counts,
and abstains for q>4. It is not evidence that the BQ-aware ranker exists.

P5 remains blocked until the schema migration, recurrence-aware family completion,
production DP/B&B/HiGHS route, independent C++ parity and numerical-certificate
requirements pass. The 7-dataset run and any production speedup claim remain
separate P7 gates.
