# LongLineage Roadmap

| Phase | Scope | Required evidence | Status |
|---|---|---|---|
| P0 | authority, provenance, private repo | oracle hashes; truth fields=0 | IN_PROGRESS |
| P1 | typed I/O and preflight | parser/status negative tests | IN_PROGRESS |
| P2 | reader, thread pool, packed writer | forced-order 1/2/4/24/40 synthetic semantic SHA; logical retained-byte cap; failure cancellation | IN_PROGRESS |
| P3 | M1 parity | zero decision/status mismatch | BLOCKED |
| P4 | M2/co-occurrence parity | frozen boundaries and first authority | BLOCKED |
| P5 | topology parity | objective/family/parent/tree digest | BLOCKED |
| P6 | validator/export/query | fault injection and logical-row parity | IN_PROGRESS |
| P7 | seven datasets | 24/40-worker identical semantic SHA | BLOCKED |
| P8 | validated-only report/release | browser/print/a11y and license gates | BLOCKED |

`BLOCKED`代表證據尚未存在，不代表方向已放棄。只有machine-readable phase ledger與
independent validation可升為`VERIFIED`。
