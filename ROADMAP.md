# LongLineage Roadmap

| Phase | Scope | Required evidence | Status |
|---|---|---|---|
| P0 | authority, provenance, private repo | oracle hashes; truth fields=0 | IN_PROGRESS |
| P1 | typed I/O and preflight | parser/status negative tests | IN_PROGRESS |
| P2 | reader, thread pool, packed writer | forced-order synthetic replay plus bounded real-data w24/w40 semantic SHA; physical RSS/I/O boundary | IN_PROGRESS |
| P3 | M1 parity | zero decision/status mismatch | BLOCKED |
| P4 | M2/co-occurrence parity | frozen boundaries and first authority | BLOCKED |
| P5 | topology parity | objective/family/parent/tree digest | BLOCKED |
| P6 | validator/export/query | independent input content/identity and artifact replay; publication TOCTOU fault injection; export/query logical-row parity | IN_PROGRESS |
| P7 | seven datasets | 24/40-worker identical semantic SHA | BLOCKED |
| P8 | validated-only report/release | browser/print/a11y and license gates | BLOCKED |

`BLOCKED`代表證據尚未存在，不代表方向已放棄。只有machine-readable phase ledger與
independent validation可升為`VERIFIED`。

2026-07-21 bounded checkpoint：HCC1395 1/7 w24與w40皆通過獨立validator，
八項science artifact semantic SHA一致，且sanitized HTML QA PASS。此證據不升級
任何phase：P3仍有2,373-key stable-membership差異、P5只有合法empty topology、
P7尚未執行七資料集。完整判讀見
`docs/reports/20260720_HCC1395完整科學運算與parity報告_01.json`。

2026-07-22 implementation checkpoint：summary 2.0.0已綁定run-local phase scope與
M1 representation；canonical validator已重算manifest、八類input identity/content、
input snapshot/lock與lineage，並用合作式publication lock/snapshot拒絕已測的
rename前後變更。這些是implementation evidence，並未升級P6；durable FAILED
staging receipt、validator-aware `FINAL_UNPUBLISHED` recovery、獨立HTS semantic-preflight
replay、非合作同UID publication race及
query/export/evaluate logical parity仍未完成。P3-P8狀態維持不變。
