<!--
建立時間: 2026-07-19
目標: 記錄 LongLineage foundation 的可重播驗證證據與明確未完成邊界
處理範圍: P0-P2/P6 foundation；不包含 P3-P8 科學與全量驗證
關聯檔案:
  - LongLineage/state/project_state.json
  - LongLineage/state/phase_ledger.json
  - LongLineage/state/tasks/active/20260719-foundation.json
  - LongLineage/governance/gate_registry.tsv
  - LongLineage/docs/data/RECORD_AND_QUERY_STANDARD.zh-TW.md
-->

# 2026-07-19 LongLineage foundation 實作與驗證紀錄

> **TL;DR**：AI治理、資料契約、typed I/O、runtime／small-q foundation與
> fail-closed CLI 已建立並在 Debug、Release、ASan/UBSan 各通過 25/25
> synthetic tests；這不是 P0-P8 完成證明，strict release gate仍以12項缺證據
> 阻擋。

## 任務分類與主張邊界

- Task type：B — Comprehensive validation。
- 本次完整範圍：新 repository foundation 的程式、契約、治理與 synthetic
  verification。
- 本次未涵蓋：M1 parity、正式 M2/co-occurrence、完整 topology family、獨立
  artifact replay validator、7-dataset 24/40 worker runs、雙語HTML release。
- Phase ledger仍為 P0/P1/P2/P6 `IN_PROGRESS`，P3/P4/P5/P7/P8 `BLOCKED`。
- 未讀取或提交真實 BAM/VCF/sidecar payload；production CLI schema禁止truth
  欄位；未執行歷史 Python 科學程式。

## Step → Verify

| Step | 輸入 | 實際命令 | 輸出／實際片段 | Exit |
|---|---|---|---|---:|
| Debug tests | `LongLineage/build-verify-debug/` | `ctest --test-dir build-verify-debug --output-on-failure` | `100% tests passed, 0 tests failed out of 25` | 0 |
| Release tests | `LongLineage/build-verify-release/` | `ctest --test-dir build-verify-release --output-on-failure` | `100% tests passed, 0 tests failed out of 25` | 0 |
| ASan/UBSan tests | `LongLineage/build-verify-sanitize/` | `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-verify-sanitize --output-on-failure` | `100% tests passed, 0 tests failed out of 25` | 0 |
| Format | 33個 C++ `.cpp/.hpp` | `scripts/ci/check_format.sh /tmp/clang-format-14-root/usr/bin/clang-format-14` | `FORMAT RESULT: PASS ... files=33` | 0 |
| Shell syntax | `LongLineage/scripts/**/*.sh` | `find scripts -type f -name '*.sh' -exec bash -n {} +` | 無stderr | 0 |
| Debug foundation gate | repository＋Debug binaries | `scripts/ci/check_all.sh build-verify-debug` | `CHECK ALL RESULT: FOUNDATION_PASS`；`NO-NETWORK RESULT: PASS` | 0 |
| Release foundation gate | repository＋Release binaries | `scripts/ci/check_all.sh build-verify-release` | `CHECK ALL RESULT: FOUNDATION_PASS`；`NO-NETWORK RESULT: PASS` | 0 |
| Strict release evidence | 24-row gate registry＋Release CTest registry | `scripts/ci/check_gate_test_coverage.sh build-verify-release --strict` | `GATE TEST COVERAGE RESULT: FAIL missing=12` | 1（預期阻擋） |
| AI readiness | repository＋Debug governance binary | `scripts/ai/check_readiness.sh build-verify-debug` | `PASS failures=0 warnings=1`；warning為Git尚未初始化 | 0 |

Sanitizer第一次在未設定leak例外時執行，所有process完成主判斷後被
LeakSanitizer以「current tracing environment不支援」拒絕。該次退出碼130的
中止run不列為正向證據；表中為保留ASan/UBSan、只關閉LSan的完整重播。

## 可查詢資料契約

| Registry／契約 | 實測數量 | 查詢方式 |
|---|---:|---|
| Offline schema IDs | 22 | `jq '.schemas | length' schema/id_registry.json` |
| Native artifact definitions | 16 | `jq '.artifacts | length' schema/catalog.json` |
| Required gate rows | 24 | `awk 'END {print NR-1}' governance/gate_registry.tsv` |
| Status/reason closed vocabulary | 87 | `awk 'END {print NR-1}' contracts/v1/status_reason_codes.tsv` |
| Source-to-target mappings | 12 | `jq '.mappings | length' provenance/source_to_target_manifest.json` |

資料查詢不得靠filename日期或任意SQL猜測。權威路徑為：

1. `schema/id_registry.json`：schema ID → immutable path＋physical SHA。
2. `schema/catalog.json`：artifact role → record schema、primary/index key、order。
3. `contracts/v1/*.tsv`：status、type、lifecycle、transform、query operator closed
   vocabulary。
4. `provenance/source_to_target_manifest.json`：來源SHA、target presence、
   implementation status、verification status分欄，不以「檔案存在」冒充parity。
5. `state/audits/*.json`：task/scope/source commit/tree digest/command digest；
   supersession DAG只接受唯一current tip。
6. Production artifact僅能經 `longlineage-query` 查詢
   `VALIDATED_FROZEN` run；P6完成前CLI維持fail closed。

完整欄位、座標、null、ordering、migration與查詢準則見：

- `docs/data/RECORD_AND_QUERY_STANDARD.zh-TW.md`
- `docs/data/DATA_CONTRACTS.md`
- `docs/data/QUERY_GUIDE.md`

## AI與開發紀律驗證

- Cold-start Q1-Q5有固定SoT：`README.md`、`AGENTS.md`、
  `docs/CURRENT_FOCUS.md`、state ledger與Git log。
- Agent先登記task type、goals、scope、inputs、expected outputs、Step→Verify。
- 平行寫入前必須有owner、parent/dependency DAG、typed write-set、ACTIVE lease與
  heartbeat；blocked work必須 `BLOCKED + RELEASED`。
- Governance C++會拒絕unknown field、`.`/`./` alias、glob、symlink ancestor、
  same/cross-task nested claim、missing dependency、DAG cycle、expired/future/stale
  lease及偽audit reference。
- 審核證據以immutable JSON envelope綁定Git source commit、canonical tree SHA與
  stdout/stderr SHA；Markdown報告本身不能升級phase。
- 正式變更走short-lived branch／review／CI／merge；只有`main`永久存在。公開
  visibility需要另行明確授權與license audit。

## Fail-closed blocker

- Private GitHub remote尚未建立／驗證。
- `/usr/bin/jsonschema` 3.2.0對2020-12 meta-schema會fallback至Draft 7；正負
  fixtures已通過，但release前仍須pin真正支援所宣告draft的validator。
- Strict gate的12個blocker多為fixture-only negative binding；fixture存在不算
  command execution evidence。
- P2尚缺block reader、byte-bounded reorder sink及1/2/4/24/40 worker semantic
  digest replay。
- P3-P5缺frozen parity vectors、formal co-occurrence authority與direct HiGHS。
- P6缺獨立artifact replay、完整validator fault injection、query AST/index執行與
  legacy logical-row parity。
- P7/P8因此不可啟動release claim；`state/release_attestation.json`保持
  `NOT_READY`。

## Git audit binding

Initial foundation source snapshot：

- Git commit：
  `7a8f75e8d23302d14c32c545218de19658667d7d`
- Canonical 197-blob tree SHA-256：
  `8c8ccdbcad239b2612175852008b36b37ce737dbe78cc3f20578c2aea9952710`
- Immutable envelope：
  `state/audits/20260719-foundation-verification-001.json`
- Envelope physical SHA-256：
  `e63ec54e2ad6ddcaf3ba085dda0bc5a9389b20bd72f02cf23a4aaadc284a80a5`
- Replay command：
  `scripts/ci/check_audit_source_snapshot.sh state/audits/20260719-foundation-verification-001.json`
- 實際片段：
  `AUDIT SOURCE PASS ... blobs=197 ... tree_sha256=8c8ccdbc...`

Envelope保存7個成功命令的完整argv、時間、exit code與stdout/stderr SHA；
phase/task只引用envelope physical SHA。它不倒填不存在的subagent lease，也不把
後續evidence commit混入先前source snapshot。

首次把真實envelope reference加入ledger後，負向測試的scratch repo暴露「複製
ledger但未複製其audit dependency」的隔離缺口。修正後，task/phase tests複製
完整audit set；audit-DAG tests則先移除baseline AUDIT reference再注入單一圖形
故障。Debug、Release、ASan/UBSan的三項focused replay均為3/3 PASS；此修正會由
下一個source snapshot envelope重新綁定。

Current superseding snapshot：

- Git commit：
  `8b62261a384bd2dd2a469f5b2ad27df2e34f3c8d`
- Canonical 198-blob tree SHA-256：
  `eb59c1f1856692569729742378d00ca396ad3a1ed125bc4aa0b395903a155bd3`
- Current envelope：
  `state/audits/20260719-foundation-verification-002.json`
- Envelope physical SHA-256：
  `414b949c3ec04641ad05a158235e85404c2ab1e1ac773c1c9c76f33f569a697b`
- Supersedes：
  `20260719-foundation-verification-001`
- Replay command：
  `scripts/ci/check_audit_source_snapshot.sh`

`002`以相同task與scope forward-supersede `001`，所以查詢時只有`002`是current
tip；task與P0/P1/P2/P6 ledger reference也只指向`002`。
