# ADR-0006: Topology Unit v2 and Bounded Exact Structural Kernel

Status: Accepted contract; bounded implementation evidence only

## Context

ADR-0005 separates four questions that the v1 topology record could conflate:

1. Is the minimum hidden-state objective \(h^*\) proved?
2. Is the complete family of minimum vertex sets enumerated?
3. Is the abundance endpoint numerically certified, including its complete tie class?
4. Is a parent-edge endpoint independently evaluated for one fixed vertex set?

The v1 fields `winner`, `parent_score_evidence_sha256`,
`best_additive_edge_score` and `best_parent_tie_count` mixed those questions.
In particular, a complete structural family did not imply a complete abundance
rank, and a vertex-set abundance score did not identify parent edges.

The bounded algorithm design follows the receipted 2026-07-18 InterSubMod
hypercube plan. This ADR migrates the machine contract and implements synthetic
exact kernels. It does **not** promote P5, update an authority manifest, or claim
HCC1395/production verification.

## Decision

### 1. Replace `topology_unit` 1.0.0 with 2.0.0

The record has mandatory, independent state machines and evidence bindings:

- `objective_state`, `objective_h`, exact `objective_bounds`,
  `objective_evidence_sha256` and `objective_reason`;
- `family_state`, `family_representation`, `family_evidence_sha256`,
  `candidate_family_digest`, exact decimal counts and `family_reason`;
- `ranking_state`, frozen endpoint ID, evaluation mode, independent evidence,
  numerical certificate, complete best-score tie class and `ranking_reason`;
- an independent `edge_endpoint` with its own status, endpoint ID, evidence,
  parent-edge tie count and optional unique mapping.

There is no v2 `winner`. `published_rank` contains vertex-set digests only and
rejects a parent mapping. Candidate structural records contain legal parent
choices plus exact decimal counts, but no abundance or edge score.

An incomplete family publishes no partial candidate array, family digest,
tree count or rank. `FAMILY_INCOMPLETE_CAP` may coexist with a certified
objective only when the objective search was exhausted; it never means the
family is complete.

### 2. Use dynamic-obligation antichain B&B for the bounded exact family

The normalized Evo-M0 problem has at most 12 active mutation bits. Vertex zero
is mandatory. Full states are mandatory vertices and each partial subcube is
an existential terminal group.

For a search state `(selected, excluded)`, obligations are rebuilt dynamically:

- every terminal group not hit by `selected`;
- every selected non-root vertex without a selected Hamming-1 predecessor.

Excluded vertices are removed from each domain. An empty domain closes the
branch. If obligation \(A \subseteq B\), \(B\) is removed because hitting
\(A\) necessarily hits \(B\). Equal domains are consolidated. Singleton
domains are propagated to a fixed point.

The next obligation is selected by minimum remaining values (MRV). Its
candidates are ordered canonically. Branch \(i\) selects candidate \(i\) and
excludes all earlier candidates. This partitions every solution by the first
selected member of that obligation, so no feasible solution is discarded.

The safe lower bound is:

```text
current hidden count
  + max(greedy pairwise-disjoint obligation count,
        maximum missing predecessor-chain length)
```

A greedy disjoint family is a valid, possibly weak hitting-set lower bound.
The connection term is also a lower bound; the two terms are combined with
`max`, never summed. Branches are pruned only when `lower_bound > incumbent`.
Equality is retained so every optimum tie remains enumerable.

The default search has no node or family limit. A configured node limit
withholds both objective and family. A configured family limit does not stop
the objective proof: search continues, but an overflow returns
`FAMILY_INCOMPLETE_CAP` and clears all partial candidates.

### 3. Use group-terminal subset DP only as an objective proof

For small terminal count, `dp[v,S]` is the minimum hidden-node weight of a
directed arborescence rooted at vertex `v` that hits terminal subset `S`.
Mandatory non-root vertices are encoded as singleton terminal groups with
zero node weight. The recurrence combines:

- coverage at `v`;
- extension to a Hamming-1 child;
- two non-empty terminal subsets merged at `v`, subtracting `v` once.

Vertices are processed in reverse hypercube topological order and terminal
subsets in increasing mask order. The DP returns only `objective_h`; it has no
candidate, family, rank or edge output. State-cell and terminal-count limits
abstain explicitly instead of returning a partial proof.

### 4. Factor parent mappings once per fixed vertex set

For every non-root vertex, legal parents are its selected Hamming-1
predecessors. The total number of legal directed edges is the sum of local
choice counts and the number of parent mappings is their product.

Both counts use `boost::multiprecision::cpp_int` and are serialized as exact
decimal strings. No Cartesian product of mappings is materialized. A vertex
without a selected legal predecessor makes the fixed set invalid.

### 5. Require bounded differential routing

R/A/X inputs are compressed onto active ALT loci. For `m <= 4`, the new B&B
result is compared with the pre-existing exhaustive small-q oracle:

- exact `h^*`;
- the canonical complete minimum vertex-set family.

Any disagreement clears objective and family outputs and returns
`ABSTAIN_DIFFERENTIAL_MISMATCH`. When the subset DP is within its configured
state boundary, its objective is also compared with B&B and disagreement
fails closed. `5 <= m <= 12` routes to B&B without pretending that the
small-q oracle covers it. `m > 12` abstains.

### 6. Keep model and projection boundaries explicit

- Evo-M0 recurrence-allowed remains the primary structural model.
- Evo-M1 strict infinite-sites is sensitivity-only and requires an explicit
  CN/LOH gate and evidence when evaluated; it cannot change the primary result.
- Evo-M2 loss-supported Dollo remains unresolved without loss evidence.
- A no-read unary chain is represented only as a candidate-bound
  multi-mutation edge equivalence with unresolved order, exact order count,
  eligibility evidence and `projection_only=true`.

## Machine-enforced invariants

- A certified objective has a zero-gap exact bound and objective evidence.
- An incomplete or abstaining family has zero published candidates and no rank.
- A complete rank requires a complete explicit family, frozen endpoint,
  independent evidence, complete tie class and either exhaustive evaluation or
  replayable outward-rounded interval exclusions.
- An abundance rank cannot contain a parent mapping.
- Edge evidence and mappings occur only in `edge_endpoint`.
- Exact counts are decimal strings and do not inherit machine-integer limits.
- Every cap, resource stop, unsupported dimension and differential mismatch is
  explicit and fail-closed.

## Bounded acceptance evidence

The synthetic suite must cover:

- v2 complete-tie, incomplete-family and abstain records;
- rejection of incomplete-family rank publication;
- rejection of a parent mapping inside abundance rank;
- rejection of unary projection without eligibility evidence;
- q=1 through q=4 oracle/B&B family differential cases;
- the `AX + XA` three-family golden;
- a four-bit `4!` family and five-bit `5!` family;
- exact parent counts beyond `uint64_t`;
- family-limit and node-limit fail-closed behavior;
- objective-only DP agreement with B&B.

Passing those checks is bounded P5 implementation evidence. Independent
validator parity, real HCC1395 output, full performance receipts and authority
promotion remain separate gates.

## Consequences

Downstream v1 producers and consumers must migrate before emitting or accepting
topology v2. Until that adapter work and independent validation are complete,
the production run remains blocked by the existing authority state. No file in
`provenance/`, `oracle/` or phase authority is changed by this decision.
