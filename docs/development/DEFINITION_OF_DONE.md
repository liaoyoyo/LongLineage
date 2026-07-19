# Definition of Done

A code change is done only when every applicable row has replayable evidence.

| Change class | Required evidence |
|---|---|
| Docs/governance | schema/links/hygiene checks; no contradiction with protected decisions |
| C++ behavior | format, warning-free Debug build, focused regression, full relevant CTest |
| Data schema | catalog/type/status binding, positive fixture, unknown/duplicate/order/null negative fixtures |
| Scientific kernel | frozen authority hash, falsifier, exact discrete parity, independent implementation |
| Runtime/concurrency | 1/2/4-worker semantic SHA equality, injected worker failure, bounded-memory evidence |
| Validator/query | producer-kernel linkage audit, fault injection, non-frozen rejection, read-only check |
| Release | GCC/Clang, Debug/Release, ASan/UBSan, pinned OCI, all phase and full-scope gates |

## Negative gate bindings

Every non-header row in `governance/gate_registry.tsv` has one unique,
machine-resolvable `negative_fixture` binding:

- `ctest:<exact-test-id>` binds to one stable CTest name. The name must be listed
  exactly once by `ctest -N`; a regex, label or display-only alias is not accepted.
- `fixture:<repo-relative-path>` binds to one non-empty, non-symlink regular file
  below `tests/fixtures/`. Absolute paths, traversal and duplicate bindings are
  rejected.

`scripts/ci/check_gate_test_coverage.sh` validates these rules in both
`--allow-declared-blocked` and `--strict` modes. A fixture-only binding proves
referential integrity and preserves the intended falsifier input; it does **not**
prove that a negative test executed. Promotion evidence must name a passing stable
test that consumes the fixture or otherwise replays the falsifier. Every required
fixture-only gate is therefore counted as one declared blocker:
`--allow-declared-blocked` reports it without hiding it, while `--strict` fails.

The following are never sufficient on their own: source presence, successful
compilation, one synthetic happy path, a partial probe, an old receipt, or a producer
self-check. Likewise, the existence of a `fixture:` binding alone is never test
execution evidence.

## Review separation

- Author and independent validator reviewer are separate roles.
- A scientific producer and its validator may share schemas and scalar utilities but
  not statistical, clustering, exact-test or solver kernels.
- Any change to identity, coordinates, RNG, denominator, status precedence or claim
  language requires an ADR or a living-note decision before merge.
- A failed or superseded result remains traceable and is never silently overwritten.
