# LongLineage

LongLineage is a truth-isolated C++17/HTSlib pipeline for long-read
sSNV co-occurrence and lineage-compatible mutation-state families.

目前狀態：**P0/P1/P2/P6 foundation implementation in progress；不是production
release。** P3/P4/P5科學parity、P7全量run與P8 release仍明確BLOCKED。

LongLineage讀取raw ONT alignments、MM/ML methylation tags、frozen PASS biallelic
sSNV VCF與authoritative latest HP/PS sidecar，建立：

- read/site與methylation packed records；
- truth-blind M1 methylation groups；
- M2 eligibility與sSNV co-occurrence；
- exact或誠實incomplete/abstain的mutation-state families；
- independent validation、frozen provenance、legacy export與read-only query。

正式工具不接受truth。benchmark由獨立`longlineage-evaluate`在run凍結後執行；
Python只負責呈現validated C++ chart-ready artifacts。

## Quick start

```bash
/usr/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLONGLINEAGE_BUILD_TESTS=ON
/usr/bin/cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/bin/longlineage --version
./build/bin/longlineage --help
scripts/ci/check_all.sh build
```

`preflight`需要完整的八角色locked-input manifest與實際HTS indexes；repo刻意不提交
真實資料，也不以空fixture偽裝production preflight。synthetic typed-I/O負例由CTest
覆蓋。

## Canonical entry points

- Governance: `LongLineage/AGENTS.md`
- Current work: `LongLineage/docs/CURRENT_FOCUS.md`
- Architecture: `LongLineage/docs/architecture/C4.md`
- Data contracts: `LongLineage/docs/data/DATA_CONTRACTS.md`
- 資料紀錄、格式與查詢準則：
  `LongLineage/docs/data/RECORD_AND_QUERY_STANDARD.zh-TW.md`
- Development: `LongLineage/docs/development/WORKFLOW.md`
- Release gates: `LongLineage/docs/release/RELEASE_GATES.md`
- Public-preview boundary and fail-closed safety status:
  `LongLineage/docs/release/PUBLIC_PREVIEW.md`
- Branch/commit-specific capability matrix:
  `LongLineage/docs/release/CAPABILITY_MATRIX.md`
- Claim boundary: `LongLineage/docs/claims/CLAIM_BOUNDARY.md`

## License

Source files declare `GPL-3.0-only` and the repository carries the canonical
GPLv3 text in `LongLineage/LICENSE`. The project remains private and unreleased
until the source-origin and dependency compatibility audit is complete.
The current candidate is therefore a private research preview and must pass
`scripts/ci/check_public_preview_gate.sh` before any public-visibility change.
