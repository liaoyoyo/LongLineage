# Development Workflow

## Configure and build

Input: source tree and system HTSlib/Jansson/OpenSSL.

```bash
/usr/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLONGLINEAGE_BUILD_TESTS=ON
/usr/bin/cmake --build build -j4
```

Expected output: `LongLineage/build/bin/` executables and zero compiler warnings.

## Verify

```bash
ctest --test-dir build --output-on-failure
scripts/ci/check_all.sh build
```

The foundation check runs:

1. repository hygiene and truth-leak scan;
2. schema/catalog validation;
3. unit and integration tests;
4. preflight negative fixtures;
5. deterministic semantic digest tests;
6. gate-to-test coverage reporting.

CI additionally runs the pinned formatting contract:

```bash
scripts/ci/check_format.sh clang-format-14
```

The command fails if the formatter is absent or any C++ source/header differs from
the repository `.clang-format`. The host PATH does not provide clang-format; local
foundation verification uses the hash-recorded, extracted Ubuntu package documented
in `LongLineage/docs/development/TOOLCHAIN.md`. CI independently installs
`clang-format-14` and remains the merge authority for this gate.

`check_all.sh` may end with `FOUNDATION_PASS` while explicitly reporting a
declared release-phase blocker. In particular, independent validator fault
injection is not implemented during P0/P1 and therefore has zero tests. That is
not a validator PASS.

Only the strict release command requires every registered gate test to exist and
pass:

```bash
scripts/ci/check_release_gate.sh build-release
```

The release command must fail until `validator_fault` and all other required
gate patterns resolve to at least one real test, and P0-P8 are `VERIFIED` with
evidence.

## Scientific change protocol

1. Add the oracle/source hash and frozen input/output vector.
2. Add a negative/falsifier case.
3. Implement producer code.
4. Implement an independent validator path without producer kernel linkage.
5. Compare status, discrete decision and semantic digest exactly.
6. Compare non-decision floating values with the documented tolerance only.
7. Update ADR, schema version, implementation notes and phase ledger.

## Branch and review

- `feat/*`: new isolated capability.
- `fix/*`: regression with reproducer.
- `docs/*`: no behavior changes.

One permanent branch: `main`. A scientific change needs separate implementation and
validator reviews. Release builds forbid `-ffast-math` and `-march=native`.

The verified local dependency/tool paths and deviations are recorded in
`LongLineage/docs/development/TOOLCHAIN.md`.

## Failed runs

Worker failure stops new work, joins every thread and leaves a `FAILED` staging root.
Never delete failed evidence. Archive it and record the incident. No partial output
may contain a VALIDATED receipt.
