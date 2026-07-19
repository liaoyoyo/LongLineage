# Release Gates

## Universal

- build and CTest PASS under GCC and Clang;
- Debug, Release, ASan and UBSan PASS;
- exact HTSlib 1.18 in production container;
- no warnings, `-ffast-math`, `-march=native`, secrets, real data or coordinates;
- schema, authority and license catalogs PASS.
- every gate has a unique, resolvable `ctest:<exact-test-id>` or
  `fixture:<repo-relative-path>` negative binding;
- strict gate coverage lists every required CTest command and fails when any is
  absent. A resolvable fixture does not waive a missing executable test.

Run the binding and gate-coverage check with:

```bash
scripts/ci/check_gate_test_coverage.sh BUILD_DIR --allow-declared-blocked
scripts/ci/check_gate_test_coverage.sh BUILD_DIR --strict
```

The first mode permits explicitly visible missing gate tests during foundation
development, but malformed, missing, duplicate or unresolved negative bindings fail
in both modes. Every required `fixture:`-only binding adds one declared blocker
because existence is not execution evidence. The strict command must remain non-zero
while `VALIDATOR_FAULT_INJECTION` has no `validator_fault` CTest; its declared forged
receipt fixture is not a substitute for that implementation.

## Scientific

- P3: M1 read set/partition/K/status/permutation decisions exact.
- P4: M2 boundaries, precedence, exact-state ceiling and FDR family exact.
- P5 structural: objective, complete candidate-family digest, legal parents and
  tree count exact; q<=4 exhaustive-oracle PASS is bounded evidence and does not
  promote the q>4 production router.
- P5 ranking: `objective_state`, `family_state` and `ranking_state` are separately
  machine-enforced. Primary BQ-aware likelihood is evaluated once per candidate
  vertex set, only after a complete family; it never selects parent edges within
  that vertex set.
- P5 numerical: every pruned published rank has replayable outward-rounded interval
  certificates and a complete best-score tie class. Ordinary floating-point bounds,
  incomplete families, caps and deadlines publish no rank.
- P5 representation: explicit candidate enumeration is the current contract. Any
  exact compressed family/count/tie representation requires a versioned schema,
  independent expansion/count oracle and query contract before use.
- P6: all fault injections caught by independent validator.
- P7: 24/40 worker runs have identical semantic SHA and unchanged input SHA.

## Artifact

- 469,849 expected site keys; zero missing/extra/duplicate for full scope.
- M1 census: 459,928 evaluable + 9,838 insufficient ALT +
  83 incomplete-distance = 469,849 site keys; 102,842 stable assignments.
- latest HP/PS: 38,345,639 exact joins; missing/conflict/multimatch all zero.
- M2 stable-site census: 919 eligible + 948 evaluable-ineligible +
  100,974 axis-indeterminate + 1 group-count-above-ten = 102,842.
- raw recovery: 9,356,980 expected + 196,706 RG-only duplicate occurrences =
  9,553,686 matched occurrences.
- topology grains remain separate: 72,994 primary HP-lineage units; 50,215 regions =
  42,240 fully complete regions + 7,975 incomplete regions.
- all artifact file and semantic digests present.
- no cap/deadline case has a winner.
- run state is `VALIDATED_FROZEN`.

## Presentation

- builder rejects non-frozen run;
- every displayed number maps to a receipt claim ID;
- no scientific recomputation;
- offline desktop/mobile/print/keyboard/WCAG checks PASS.

## Public visibility

- user explicitly authorizes public release;
- GPL/source-origin/dependency audit PASS;
- sanitized docs contain no real locus, read, private path or run payload.
