<!--
created_at: 2026-07-19T22:10:00+08:00
goal: LL-G4, LL-G5
task_type: E
scope: GitHub Actions toolchain, dependency provenance, and CI-only regression repair
related_files: .github/workflows/ci.yml, containers/Dockerfile, containers/versions.lock.tsv, scripts/ci/check_dependency_lock.sh, tests/CMakeLists.txt
-->

# CI toolchain hotfix audit

Status: `LOCAL_VERIFIED_GITHUB_PENDING`

## TL;DR

PR #1 的七個失敗 job 已定位為兩個初始工具鏈問題，並在修復後揭露、收斂三個
repository-context test 假設與一個不完整 scratch fixture。最終本機完整
repository gate 為 33/33 PASS，production image 的 hermetic/no-network gate 為
30/30 PASS；GitHub rerun 尚未完成，因此本文件不得宣稱遠端 CI 已恢復。

這是 Task Type E hotfix，服務 LL-G4/LL-G5。沒有修改 scientific kernel、
artifact schema、phase ledger或release attestation。

## 失敗證據與根因

失敗 run：
[GitHub Actions 29674244956](https://github.com/liaoyoyo/LongLineage/actions/runs/29674244956)，
head `fbc7873c85daa6ec4247d8d404b1de938197fd17`。

| Job | 原始結論 | 根因 |
|---|---:|---|
| gcc-debug | failure | workflow呼叫`/usr/bin/cmake`，但未明示安裝apt CMake |
| clang-debug | failure | 同上 |
| gcc-release | failure | 同上 |
| clang-release | failure | 同上 |
| clang-asan-ubsan | failure | 同上 |
| gcc-tsan-p2 | failure | 同上 |
| pinned-container | failure | Ubuntu archive已不提供舊的`ca-certificates`、`curl`、`libssl` patch pin |

第一個修復版image在編譯後又正確暴露四個原先被前置失敗遮蔽的測試問題：

1. `performance_benchmark_records`需要完整Git baseline object，但GitHub checkout
   預設為shallow，production Docker context也刻意排除`.git`。
2. `hygiene_untracked_forbidden_negative`需要host Git工具。
3. `dependency_lock_negative`需要`.github/workflows/ci.yml`，但production Docker
   context刻意排除`.github`。
4. agent-task negative scratch複製active child，卻未複製其archived parent與
   digest-bound evidence，因此在預期的overlap fault之前先fail closed。

## 修復契約

### CMake與checkout

- Hosted matrix明示安裝`cmake`，並以`/usr/bin/cmake` probe、記錄version、configure
  與build；這與`docs/development/TOOLCHAIN.md`的apt-owned toolchain SoT一致。
- Synthetic matrix使用`fetch-depth: 0`，使versioned performance record可重播
  baseline Git blob。
- dependency-lock checker會拒絕缺少apt CMake、非apt-owned CMake路徑與shallow
  synthetic checkout。

### apt與image provenance

- 仍以immutable Ubuntu base digest與HTSlib 1.18 tarball SHA-256作硬authority。
- Ubuntu security/update archive在image build時解析；不得偽稱已被archive汰換的
  patch版本仍可重建。
- Builder與runtime的實際`package<TAB>version`分別寫入：
  - `/opt/longlineage/share/provenance/builder-apt-packages.tsv`
  - `/opt/longlineage/share/provenance/runtime-apt-packages.tsv`
- Release authority必須再綁最終immutable image digest；不得以未來重build會得到
  相同apt layer作假設。

### Repository-context與production-context

`LONGLINEAGE_BUILD_REPOSITORY_CONTEXT_TESTS`預設為`ON`。Hosted matrix執行完整
33-test gate；production Docker build明示設為`OFF`，不把Git history或GitHub
control files塞入production build context，並執行其餘30個hermetic tests。
三個受控測試都有`repository-context` label；這不是靜默skip。

## Step → Verify

| Step | Input | Command | Output | 實際結果 |
|---|---|---|---|---|
| Dependency policy正負測試 | workflow、Dockerfile、lock TSV | `scripts/ci/check_dependency_lock.sh`與`test_dependency_lock_negative.sh` | stdout | exit 0；positive與negative皆PASS |
| Archived-parent scratch修復 | active/archive task registry | `test_agent_task_negative.sh <governance> <repo>` | 13個fault cases | exit 0；13/13 PASS |
| Release build | source tree、HTSlib 1.18 | `/usr/bin/cmake -S . -B build-ci-hotfix ...`；`/usr/bin/cmake --build ... --parallel 4` | `build-ci-hotfix/` | exit 0；所有targets built |
| Full repository gate | `build-ci-hotfix/`與完整Git history | `scripts/ci/check_all.sh build-ci-hotfix` | gate log、CTest | exit 0；33/33 PASS；`FOUNDATION_PASS` |
| Repository-context focus | full checkout | `ctest --test-dir build-ci-hotfix -L repository-context` | CTest | exit 0；3/3 PASS |
| Production image | sanitized Docker context | `docker build --file containers/Dockerfile --tag longlineage:ci-hotfix .` | local OCI image | exit 0；30/30 no-network PASS |
| Image provenance與smoke | local OCI image | `docker image inspect`；offline TSV schema/sort/unique/SHA/runtime-dpkg checks；`longlineage --version` | image ID與manifest census | exit 0；image `sha256:aff89e6b8120284c640511a06991765956703ffe3065decd2831deeab56b0b9d`；builder 207 rows／`f556d5b0603e912d7415650bd41e8fbb9a38c2db2445b6bd368c3c9d5c7107c5`；runtime 105 rows／`174a2633d22c0ade4f3419c800c55ddc20831082f14a3c1f3efed55a16a93e8f`；runtime逐列match；`longlineage 0.1.0` |

本機PATH沒有`clang-format-14`，因此直接format probe明確exit 1；本次未修改C++，
且hosted `gcc-debug` job仍會安裝並執行clang-format 14 gate。遠端結果回來前不可
把format標為PASS。

## 分支與發布限制

修復留在既有`docs/method-performance-audit`，因它是draft PR #1的失敗head。
這是為了快速修復同一PR而記錄的branch-name deviation；後續行為修改必須使用
`fix/*`或`feat/*`。

本hotfix只恢復CI可執行性與provenance真實性。P3/P4/P5/P7/P8、validator
fault-injection及full-data release gates仍維持既有BLOCKED狀態。

## GitHub closeout（待填）

- Push commit：`PENDING`
- GitHub Actions rerun：`PENDING`
- Draft PR #1：保持draft；未取得明示授權不得merge。
