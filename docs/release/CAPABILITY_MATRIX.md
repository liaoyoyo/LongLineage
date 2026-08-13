# Branch-specific Capability Matrix

This matrix prevents feature-branch code from being described as a `main` or production
capability. It describes source presence and verified test scope, not scientific truth.

| Capability | `origin/main@5daf50f` | `b9aaa12` private feature candidate | Claim ceiling |
|---|---|---|---|
| Core `longlineage` CLI | Present | Present | `run` remains `KernelBlocked` exit 6 while attestation is `NOT_READY`. |
| Independent validator foundation | Present | Present and covered by synthetic tests | Foundation evidence only; P6 remains `IN_PROGRESS`. |
| Regional descriptive compatibility | Not represented by this main baseline | Present | Descriptive/evaluation endpoint; not formal P4/P5/P7 authority. |
| `longlineage-lineage-paths` | Absent | Compiles; `--help` exits 0 | Feature preview only; no dedicated CTest at `b9aaa12`. |
| `longlineage-read-assign` | Absent | Compiles; `--help` exits 0 | Feature preview only; does not establish cellular read labels. |
| `longlineage-tag-bam` | Absent | Compiles; `--help` exits 0 | Feature preview only; not a production `run` output and not validated by the 47-test baseline. |
| Public redistribution | Not approved | Not approved | Source-origin, license and history gates must all pass first. |
| Production release | No | No | P3/P4/P5/P7/P8 remain blocked. |

The feature candidate contains interface documentation with historical status
differences. When those notes conflict, machine state in `state/phase_ledger.json`,
`state/release_attestation.json`, the current executable tests, and the public-safety
receipt take precedence.
