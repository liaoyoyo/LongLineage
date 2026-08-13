# Implementation Notes

Status: verified · PARTIAL — history-safe private draft publication scope closed; not P8 or a production release

## 2026-08-13 private-first public-preview safety foundation

- [決策] `b9aaa12a11fa00606bd174dabd0f172a5d112359`只作 private
  research-preview candidate；不直接公開、不建立tag或release，production
  `run`仍以`KernelBlocked` exit 6安全停止，P3/P4/P5/P7/P8維持`BLOCKED`。
- [決策] source-to-target contract升為1.2.0。21 rows逐列記錄真正可重播的
  source commit、replay status、license disposition、evidence、reviewer與scope；
  現況為5筆declared-origin match、12筆other-commit match、4筆hash not found，
  且21/21 license disposition皆保持pending。
- [決策] SPDX 2.3檔由`LongLineage/scripts/release/generate_sbom_spdx.sh`
  決定性產生；未知dependency license與尚未完成per-file license mapping均使用
  `NOASSERTION`及annotation，不把inventory冒充license audit PASS。
- [驗證] `LongLineage/scripts/testing/test_sbom_spdx.sh` exit 0；33 packages
  （root + 11 locked dependencies + 21 source mappings）可重生且byte-identical。
- [驗證] `LongLineage/scripts/ci/check_public_preview_gate.sh HEAD`
  預期exit 1，固定辨識5類blocker：repository license review、4 unresolved source
  rows、21 unapproved mappings、11 dependency license `NOASSERTION`及7 history
  findings。`LongLineage/scripts/testing/test_public_preview_gate.sh`把此安全失敗
  驗為PASS，避免誤認成工具故障。
- [驗證事件] hosted CI顯示moving `origin/main`已前進且不再是candidate ancestor，
  造成history checker exit 2。Gate已改從`PUBLIC_SAFETY_RECEIPT.json`讀取immutable
  audit base `5daf50f04cbe233abfade816ce9e0903f6b38954`；在更新後的remote-main狀態仍
  精確重播7 findings，CTest regression PASS。
- [驗證] safety layer configure/build exit 0，新增tests 2/2 PASS，最後完整
  CTest 49/49 PASS；clang-format 14與`git diff --check`均exit 0。47/47仍只代表
  frozen `b9aaa12` baseline，49/49則代表本輪stacked safety layer，不混為同一receipt。
- [決策] current source mapping schema為1.2.0；原1.1.0 schema以byte-identical
  versioned path保留並同時登錄，避免歷史receipt的schema ID失去repository-local
  resolution。
- [偏離] 首次手動重播gate時，shell命令誤帶`+set`/`+scripts`前綴而未執行；
  該次不作證據。移除誤植後重新執行，gate exit 1、兩個test wrappers皆exit 0。
- [驗證事件] 新gate首次由CTest build directory啟動時只回報前4類blocker，
  因history checker隱含依賴current working directory，且missing summary在
  `set -e -o pipefail`下提前結束。修正為明確在repository root執行history scan，
  並讓summary absence安全回報；此失敗不計入PASS證據。
- [驗證事件] GitHub visibility在本次稽核期間兩度被觀察為`PUBLIC`，第二次
  containment後API回報`PRIVATE`及`updatedAt=2026-08-13T03:23:45Z`。此狀態只能
  證明目前已收回，不能證明暴露期間沒有clone或download；因此仍維持
  `KEEP_PRIVATE_NO_TAG_NO_RELEASE`。
- [未決] 公開前仍需獨立完成7筆history findings處置、3個unique missing source
  hashes來源重建、21筆per-file source/license review與11項dependency license
  determination；本輪不宣告license compatibility或publication readiness。

## Decisions

- [決策 2026-07-21] Python-compatible regional endpoint擴充範圍凍結為
  `HCC1395, HCC1395_DORADO, COLO829, H1437, H2009, HCC1937, HCC1954`七個
  dataset，順序與`oracle/production_input_authority.json`完全一致。每個樣本內
  使用24 workers，樣本間序列執行；每份bundle必須先由獨立validator產生
  `FROZEN`，再進C++ region/unit/pattern crosswalk。Status: Accepted. Tier: L1。
- [決策 2026-07-21] 七樣本compat summary/producer/validator契約升為closed
  schema `2.0.0`，綁定dataset ID/order、完整八角色physical SHA-256、source
  authority、source manifest、typed timing與false claim ceiling；legacy schema
  `1.0.0`只保留既有HCC bundle重播相容。Status: Accepted. Tier: L1。
- [決策 2026-07-21] 本機HCC BAM的正式輸入是受限環境中的canonical NFS
  authority，不宣稱為本地副本。所有實體路徑由untracked closed-shape input-path
  map提供；Git只保存role、size與digest。Status: Accepted. Tier: L1。
- [決策 2026-07-21] 六個尚無full-content digest的raw BAM必須先各自完成一次
  SHA-256 freeze；既有`storage_identity_v1`抽樣digest不得冒充full SHA。正式
  producer仍逐樣本重播八角色full SHA，hash時間與science時間分欄。Status:
  Accepted. Tier: L1。
- [決策 2026-07-21] We will implement the historical Python-v2 50 kb regional
  sSNV workflow as a separate `PYTHON_V2_DESCRIPTIVE_REGIONAL` C++ endpoint.
  Status: Accepted (user-specified). Tier: L1. It may report regional
  mutation-state families, but it must not replace, relax or be counted as the
  formal M2/topology endpoint. Revisit if an independently validated migration
  contract unifies the two endpoint definitions.
- [決策 2026-07-21] We will define parity at four nested grains: ordered region
  membership, densest-eight selected loci, HP-family supported R/A/X patterns,
  and per-unit integer topology/class outputs. Aggregate `8,222` alone is an
  insufficient acceptance test. Status: Accepted. Tier: L1. Revisit if a
  stronger byte-stable cross-language serialization is frozen.
- [決策 2026-07-21] We will use raw HCC1395 5kHz BAM plus the frozen truth-free
  exact HP/PS sidecar. The March persisted tagged BAM was produced with truth
  benchmark arguments and remains evaluation-only. Status: Accepted. Tier:
  L1. Revisit only if a truth-free persisted tagged BAM with an exact stream
  equivalence receipt is generated.

- [決策 2026-07-20] HCC1395 scientific parity task以
  `VALIDATED_FROZEN_DATASET_GATE`為單資料集完整驗證目標；它不得放寬或冒充
  `PRODUCTION_7_DATASET`。Pre-decision audit分數50/100，verdict=`PROBE`，
  只有P3→P4→P5→P6 bounded checkpoints全綠後才允許full HCC run。
- [決策 2026-07-20] HCC1395唯一合法tag輸入是raw MM/ML BAM加上SHA凍結的
  latest HP/PS sidecar，採`EXACT_PROJECTION_NO_FALLBACK`；producer-facing
  tagged BAM FIFO不是persisted input，BAM舊HP fallback永遠禁止。
- [決策 2026-07-20] P4沒有成功的歷史full co-occurrence authority。C++正確性
  必須以frozen synthetic/small-real dual oracle、closed conservation與獨立
  validator建立；禁止把attempt 1-6或不存在的輸出補寫成golden parity。
- [決策 2026-07-20] 舊regional-tree只作context comparison。新P5遵守ADR-0005，
  分開objective/family/ranking，且CN/LOH-gated strict infinite-sites只是
  sensitivity；Evo-M0 recurrence-allowed仍為primary。
- [決策 2026-07-20] `topology_unit`升為v2前不得接production ranker。v2分離
  objective certificate、complete vertex-set family、BQ-aware abundance ranking與
  optional parent-edge endpoint；family incomplete、objective未證、數值certificate
  失敗或tie class未列完時，`published_rank`必為null。
- [決策 2026-07-20] P3/P4 parity採分層口徑：seed、RNG raw stream、status、
  precedence、integer counts與所有會改變decision的邊界要求exact；noncentral
  distribution等不改變decision的float才使用預先登記tolerance。Python只能生成
  synthetic/small-real frozen vectors，production及validator不得呼叫Python。
- [決策 2026-07-20] HCC dataset gate沿用
  `RUNNING(staging) -> VALIDATED(staging) -> VALIDATED_FROZEN(final)`；producer只可
  宣告`READY_FOR_VALIDATION`。validator必須是未連結producer kernels的獨立binary，
  atomic rename後但final run receipt尚未發布的目錄仍不可查詢或產報告。
- [決策 2026-07-20] BAM與reference的既有`storage_identity_v1`只足以作readiness
  及change detection，不足以滿足現行manifest的full-content lock。本輪另執行一次
  完整SHA-256並把耗時列為input-freeze成本；不得混入科學核心wall time。
- [決策 2026-07-20] M1相容性必須分開兩個數值邊界：in-memory primary先重播
  `float(ML_raw/255.0F)`的binary32值；舊Python assignment authority實際讀過
  `methylation.csv`的`fixed setprecision(4)`，因此只在legacy parity模式再套四位
  小數序列化/讀回。兩者不可默默混成同一點估計。
- [決策 2026-07-20] HCC block planner除4096-site/250000-record hard ceiling外，
  加入顯式250 kb focal-span切分，避免稀疏4096 sites形成多Mb iterator；halo仍固定
  5000 bp且每個block把halo內所有PASS partner markers納入一次CIGAR投影。
- [決策 2026-07-20] HTSlib 1.18多執行緒BGZF writer的virtual offset只有在
  queue-draining flush後穩定。Indexed writer因此以site group作多執行緒flush
  邊界；4-thread budget只留給最大宗`methyl_calls`，pair/site/topology等較小
  artifacts使用單執行緒逐row精確offset，避免每個one-row group付出10 ms barrier。
- Production, evaluation and presentation are separate trust domains.
- Data schemas and query contracts are first-class public interfaces.
- Phase status is machine-readable; unverified kernels hard stop.
- Native output is BGZF-packed; legacy small-file layout is export-only.
- Schema catalog、run membership、artifact catalog、record lineage、semantic
  digests與immutable receipts形成無循環的closed-world provenance graph；任一
  missing/extra/duplicate binding都fail closed。
- Query v1使用closed typed AST與canonical order，不提供SQL、join、aggregation、
  coercion或science recomputation；P6完成前CLI只能fail closed。
- C++ tests不得使用會在`NDEBUG`下消失的`assert`作驗證；Debug與Release必須執行
  相同判斷。
- Synthetic test output必須以process-unique scratch path隔離，因為不同compiler／
  build configuration可能同時執行CTest。
- AI task ownership採active/archive registry、parent/dependency DAG、typed
  non-overlapping write-set與time-bounded lease；blocked work釋放lease，resume時
  重新發行，不把一次性ACTIVE timestamp提交成永久baseline。
- `canonical_json`科學欄位必須再綁offline nested schema與companion
  conservation；兩個scalar形成座標時以machine-readable semantic group表達。
- Registry與source port都有獨立implementation／verification lifecycle；存在
  target或契約不等於parity完成。
- P2 block planner只切分authoritative input order；dataset/contig轉換必切，
  4,096 sites或250,000 estimated alignments任一先到即切。單一site若已超過
  cost ceiling會fail closed，因v1尚無凍結的site內拆分規則。
- Indexed BAM reader要求caller明示index路徑，每個pool worker以stable worker
  index綁定自己的move-only reader；不得共享mutable HTSlib stream/iterator。
- Result reorder由payload type的`retained_bytes() const noexcept`提供唯一logical
  sizing authority，使用`capacity-max_item`非frontier額度與一筆frontier reserve。
  Oversize不分sequence或completion order一律`ITEM_TOO_LARGE`；duplicate、late、
  gap、zero byte與writer exception皆進terminal failure。
- Pool terminal observer在worker error/manual cancel後、且不持有pool result
  mutex時喚醒sink；sink failure由worker轉成exception回傳pool，避免雙鎖與永久
  wait。
- 這個cap只約束sink admitted logical payload，不涵蓋HTSlib/BGZF buffer、
  allocator overhead、thread stack、task queue、callback-local payload或RSS；
  production仍需global permit pool與實測證據。
- Performance claim採`IMPLEMENTED / COMPONENT_MEASURED / BOUNDED_MEASURED /
  FULL_PRODUCTION_MEASURED / DERIVED / ESTIMATED / PLANNED`分級。Bounded record
  固定`production_claim_allowed=false`；只有P7兩個VALIDATED_FROZEN receipts可
  支持production比較。
- BGZF TSV row只建立一次canonical `row+LF` payload，同一buffer依序寫入BGZF與
  semantic SHA。Field validation、decompressed bytes、logical/physical bytes、
  close後physical SHA及writer-thread-independent semantic identity不變。
- Local benchmark是versioned governance record；CI重播baseline Git blobs、
  candidate source、harness SHA、raw-trial median與偽production-claim負向案例。
- CI的CMake authority與本機toolchain SoT一致：hosted runner先明示安裝apt
  CMake，再只使用`/usr/bin/cmake`。需要Git history或`.github` control files的
  tests標為`repository-context`並由full-history hosted checkout執行；production
  Docker context不攜帶這些repository metadata。
- Mutable Ubuntu apt archive不再使用會被汰換的patch-version pins。Immutable
  base image digest與HTSlib SHA仍是hard authority；實際builder/runtime package
  versions寫入image provenance manifests，release再以final image digest封存。

## Deviations

- None accepted. Local non-release builds may record a dependency-version deviation,
  but release requires exact HTSlib 1.18.
- Local ASan/UBSan replay sets `ASAN_OPTIONS=detect_leaks=0`; address and undefined
  behavior checks remain active, while leak-only verification is delegated to CI/
  container where LeakSanitizer is supported.
- Bootstrap期間先啟動三個subagent，之後才建立machine lease/write-set規則；
  這些歷史寫入沒有逐agent task record，且不得倒填lease冒充事前授權。主agent
  已記錄此process deviation；規則生效後的後續寫入由單一root lease完成，未來
  delegation必須先註冊child task並縮小parent claim。
- `/usr/bin/jsonschema` 3.2.0會把未識別的2020-12 metaschema fallback到Draft 7。
  本輪只引用實際正負fixture結果；正式release前仍需統一schema draft或pin真正
  支援宣告draft的validator。
- P2修正期啟動兩個唯讀review subagent前未先建立child task record。兩者沒有
  filesystem寫入，所有建議均由root在既有lease內重作與驗證；此流程偏離不倒填
  偽造歷史lease。後續delegation必須先註冊child task與read/write scope。
- HCC1395 audit receipt v2補強期間，一個subagent在isolated worktree寫入
  `tests/CMakeLists.txt`、C++ test與fixture，formal root task雖已事前包含這些
  exact paths，但該subagent沒有獨立child task、non-overlapping write-set與
  lease。此事記為process deviation，不倒填歷史授權；subagent停止寫入後由
  `codex:root`接管，並須在commit A的clean source上獨立重跑format、Debug/
  Release、full CTest、sanitizer、Draft 2020-12與privacy gates，才可建立
  superseding receipt。
- 同一輪另啟動historical/report inventory、integration governance review與
  w40 runtime monitor三個read-only subagents，亦未先建立child task record。
  三者沒有filesystem寫入、沒有啟動或終止run，也沒有產生可直接升格的PASS
  evidence；其發現須由`codex:root`以formal task lease重新執行與綁定。此項同樣
  記為process deviation，不倒填task或lease。
- HCC1395正式報告階段另偵測到一個不在目前subagent tree內、但共用formal
  worktree與root lease的AI session。該session先建立未追蹤報告、執行C++ audit
  與browser QA，再提交mobile table containment修正`89f0cc7`。root在沒有
  同時寫入該檔的前提下，逐項重驗diff、audit receipt與QA receipt；先行mobile
  QA明確為FAIL，不升格為完成證據。這仍屬缺少獨立child task／write-set的
  process deviation，不能因commit合理就倒填授權。
- CI hotfix留在既有`docs/method-performance-audit` branch，因它是draft PR #1
  的失敗head；這是一次性快速修復branch-name deviation，後續行為變更恢復
  `fix/*`或`feat/*`。

## Trade-offs

- [折衷 2026-07-21] 七樣本batch wrapper採可稽核`--resume`：只跳過已有
  `FROZEN`的完整樣本；任何存在但未freeze的sample directory一律停下並保留，
  不自動清除或覆寫。這使長時間NFS run可安全續跑，但需要validator/cohort
  receipt在最後再次整批確認。Status: Accepted. Tier: L1。
- [折衷 2026-07-21] 共享NFS在啟動時load1約64、I/O wait約70%，高於預登記
  `max_load=60`。大型hash/science先守門，只做local build、oracle replay與
  contract tests；待load進入門檻後才依序啟動，等待時間不得混入science wall。
  Status: Accepted. Tier: L1。
- [折衷 2026-07-21] We will reproduce Python-v2 L0/L1 sSNV semantics exactly,
  while setting post-tree CN/LOH to unavailable in the production-compatible
  bundle. This permits exact sSNV membership/class parity without importing
  SEQC2 truth-derived selection. Status: Accepted. Tier: L1. Revisit if an
  explicitly isolated evaluation adapter is requested.
- [折衷 2026-07-21] We will preserve the legacy enumeration cap and incomplete
  classification in compatibility mode even though LongLineage has a faster
  exact B&B/DP solver. The modern solver may be measured as a non-authoritative
  sensitivity, but it cannot silently change Python-compatible counts. Status:
  Accepted. Tier: L2. Revisit after exact per-unit differential evidence proves
  an output-preserving substitution.

- HCC1395 gate先提供一個完整單資料集science/reproducibility證據鏈，換取比七資料集
  P7更早暴露方法與效能問題；代價是`production_claim_allowed=false`，不能從單樣本
  外推跨資料集效能或泛化。
- 不保存約292 GB級的第二份tagged BAM；改以raw BAM加1.43 GB sidecar exact join。
  這降低儲存與重寫I/O，但validator必須重播完整identity conservation。
- Jansson is used for local C++ JSON parsing to avoid a vendored header; the pinned
  production container records its exact version.
- A small in-repo CTest harness avoids network-time test dependencies.
- Close後完整重讀BGZF計算physical SHA會增加I/O，但它是freeze/validator的獨立
  evidence，保留；不以writer內部digest取代。

## Open questions

- [未決 2026-07-21] Status: Resolved. Tier: L5. The user term `bip7 HCC1395
  BAM` is operationalized as the governed `<BIG8_INPUT_ROOT>` HCC1395 authority path
  mounted on this machine; no distinct `<LOCAL_WORK_ROOT>` copy was found or claimed.
  Every v2 summary still binds logical/canonical path plus device/inode identity.
- [未決 2026-07-21] Status: Open. Tier: L5. A scope-matched, formally timed
  Python-only HCC1395 baseline is not yet present; the historical 1:24:47 value
  is derived from launch/manifest timestamps and cannot by itself support an
  exact speedup factor.

- P3/P4完整logical golden digest、PCG64/tie/relabel、HP-family registry與Endpoint-B
  O/X precedence vectors是否能從現有frozen corpus完整取得；若不能，需先建立新的
  truth-blind dual authority，而不是默認舊Python輸出。
- HCC q>4 topology family在exact bitset obligation-B&B、dynamic subcube antichain
  與small-q terminal subset DP後是否仍有不可控output-sensitive family；任何
  cap/deadline案例必須abstain。
- 上游20260711 receipt未凍結hostname。P6新receipt必須加入producer/validator
  hostname、kernel、executable SHA與mount/input identity，避免「本機產生」只能靠
  路徑與platform推論。
- Direct C++ HiGHS pin/build route.
- Public-release license compatibility.
- [驗證 2026-07-22] Status: Resolved. `gh auth status` confirms the authenticated
  private remote and `repo`/`workflow` scopes; no token was printed, recreated or stored
  in the repository. Current-head CI still must pass before merge.
- Runtime performance collector尚未實作；run receipt schema存在不等於wall/RSS/I/O
  等欄位已被正確收集。
- Audit envelope的source-tree Git replay已定義；recorded command output digest
  仍需由明示的獨立replay命令重跑比對，不可把digest欄位存在當作執行證據。
- P3 requires frozen PCG64/RNG/logical-digest vectors and a versioned HP-family
  mapping; P4 also requires frozen Endpoint-B O/X callability precedence vectors.
- P2 production completion still requires persistent VCF/Tabix,
  latest-HP/PS-sidecar/Tabix and FASTA/FAI handles, exact sidecar join in the
  worker bundle, one-pass CIGAR projection for all partner markers, a physical
  process-wide memory bound, and staging→independent-validator→atomic-freeze
  integration.

## Lore

- Sidecar exact identity includes FLAG and CIGAR digest; QNAME is insufficient.
- MM orientation is as-sequenced, not reference-forward.
- `O` and `X` allele states must never be collapsed into `R`.
- Fixed `/tmp` filenames can race across independently launched CTest build trees
  even when each tree runs its own tests serially.

## Verification incidents

- Release build initially exposed that `assert`-based typed-I/O tests compile away
  under `NDEBUG`. Replaced by always-on checks and replayed Debug/Release.
- A parallel Debug/Release CTest replay initially produced one BGZF content mismatch.
  Root cause was cross-process scratch-file collision, not semantic-digest drift.
  PID-scoped synthetic paths made the same parallel replay pass in both builds.
- A sanitizer replay was first launched without `ASAN_OPTIONS=detect_leaks=0`.
  LeakSanitizer cannot operate under the current sandbox tracing boundary, so the
  run failed after otherwise-passing processes. The run was stopped, explicitly
  classified as environment-invalid evidence, and replayed with leak detection
  disabled while AddressSanitizer and UndefinedBehaviorSanitizer remained active;
  the governed replay passed 25/25.
- After the first real audit envelope was bound into the phase/task ledgers, three
  negative-test scratch repositories copied the ledger but not its referenced
  `state/audits` files. They therefore failed at baseline setup before reaching the
  intended injected fault. The task/phase tests now copy the complete immutable
  audit set; the audit-DAG tests instead remove baseline AUDIT references before
  creating their controlled synthetic graph. Debug, Release and ASan/UBSan focused
  replays then passed 3/3.
- The first P2 TSan matrix reported a double lock in the test-only
  condition-variable activation barrier. Replacing it with one atomic arrival
  flag per stable worker removed the report. A subsequent TSan run exceeded the
  production `<=46` process-thread assertion only because the TSan runtime adds
  helper threads; that assertion remains enabled and passed in ordinary Debug/
  Release, while TSan continues to verify all P2 memory races.
- 第一個clean-commit readiness replay把未連結到governance target的producer
  header mtime誤判為stale binary而exit 1。Readiness現在只比較
  `longlineage-governance`的實際source/CMake inputs，並新增
  `ai_readiness_independent_governance_target` CTest；原失敗不列入PASS
  envelope，修復後才重新擷取證據。
- `738b13f` detached source的Release full CTest在擷取後超過一小時才重播，
  active task heartbeat因此依治理契約過期；42/44通過，只有
  `governance_check_state`及其readiness wrapper fail closed。這不是科學核心
  regression，也不列入PASS receipt；最終證據必須在task正式archive後由
  clean final commit重建並取得44/44，消除time-decaying active lease。
- 第一版HCC1395 HTML的desktop QA通過，但390 px mobile因寬表格使document
  scroll width為671 px而FAIL。修正後每個table wrapper具備明確100% containment、
  keyboard focus與accessible label，且builder self-test鎖定7個wrapper；只有
  desktop、mobile、print與console/network checks全數PASS後才能提交報告。
- 第一個performance record replay因committed harness usage字串與/tmp原型不同，
  正確拒絕harness SHA。Record更新為repo source的實際SHA後才PASS；原型binary
  digests與raw trials保留，沒有把失敗驗證冒充PASS。
- 第一個CI hotfix image在apt/CMake前置問題解除後，正確揭露三個
  repository-context assumptions與一個archived-parent scratch缺口。最終將
  full-history/host-control tests留在hosted gate，production image執行30個
  hermetic tests；scratch則複製archived task與其digest-bound evidence。
- CI hotfix commit `80481dd38790f52eb0027dec8696af6d509afef5`的push與PR
  workflows各7/7 PASS；format、GCC/Clang Debug/Release、ASan/UBSan、TSan與
  pinned-container合計14/14 PASS。PR #1仍是draft，沒有因CI修復自動merge。

## Foundation verification snapshot

- Debug CTest: 25/25 PASS.
- Release CTest: 25/25 PASS.
- ASan/UBSan CTest: 25/25 PASS with the recorded sandbox-specific leak exception.
- Debug and Release `scripts/ci/check_all.sh`: `FOUNDATION_PASS`, including the
  no-network authority check.
- Gate registry: 24/24 bindings structurally valid; strict release coverage is
  intentionally blocked by 12 gates whose negative evidence is fixture-only or
  still lacks an executable validator fault-injection test.
- Catalog: 22 offline schema IDs, 16 artifacts and 87 closed status/reason rows.
- Full command, input/output and blocker evidence is recorded in
  `docs/audits/20260719_foundation_verification.md`.
- Machine audit `20260719-foundation-verification-001` binds source commit
  `7a8f75e8d23302d14c32c545218de19658667d7d`, 197 tracked blobs and canonical
  tree SHA-256
  `8c8ccdbcad239b2612175852008b36b37ce737dbe78cc3f20578c2aea9952710`.
- Current audit `20260719-foundation-verification-002` supersedes `001` and
  binds evidence/fix commit `8b62261a384bd2dd2a469f5b2ad27df2e34f3c8d`, 198
  tracked blobs and tree SHA-256
  `eb59c1f1856692569729742378d00ca396ad3a1ed125bc4aa0b395903a155bd3`.

## P2 synthetic component verification

- Debug/Release/ASan+UBSan full CTest: 31/31 PASS in each build.
- TSan focused CTest: 5/5 P2 tests PASS after the test-barrier repair.
- Worker/chunk replay: workers `1,2,4,24,40` × block sizes `1,2`, plus a
  40-worker/4-BGZF-writer replay.
- Frozen synthetic semantic SHA-256:
  `9179c42faf14000c9b1c87386a09cd33cae4bce27078d7d5af2798269c4fead0`.
- Every replay decompresses 80 ordered rows and checks an independent covering
  read oracle; ordinary 40-worker/4-writer execution remains at or below 46
  process threads.
- This evidence is explicitly `PARTIAL`, not a claim about seven real datasets
  or the still-missing complete production worker input bundle.

## HCC1395 bounded real C++ worker probe

- Input：frozen HCC1395 raw BAM/BAI、latest HP/PS sidecar/TBI、PASS
  sSNV VCF/CSI及GRCh38 FASTA/FAI；未讀取truth。
- C++ VCF census/plan：113,061 PASS biallelic sSNVs，79,687 autosomal，
  8,105 blocks，81,274 marker occurrences；wall 1.37 s、peak RSS 33,372 KiB。
- Dense stress block（chr6，256 focal / 259 marker）：3,987 iterator records，
  3,223 retained，2,672 exact-joined unique reads，24,327 R/A matrix rows，
  16,711 ALT rows，3,379,905 matrix cells，356,742 admitted CpG calls。
- Dense block timings：input census+plan 0.780 s、BAM 2.397 s、sidecar 0.046 s、
  reference 0.005 s、one-pass projection+site matrices 0.636 s、total 4.225 s；
  `/usr/bin/time` wall 4.47 s、peak RSS 331,588 KiB。
- Verdict：`REAL_BLOCK_PARTIAL`、`production_claim_allowed=false`。此probe證明
  本機raw BAM + sidecar exact join與C++ block路徑可行；尚未含完整M1 null、
  global co-occurrence barrier、P5 ranking、validator/freeze或全量時間。

## HCC1395 production-shaped readiness（2026-07-20）

- 正式planner參數為4,096 focal ceiling、250 kb span、250,000 planning-unit
  ceiling與5 kb halo；census為113,061 PASS biallelic sSNV、79,687 autosomal、
  8,098 blocks、80,905 marker occurrences。最密集block 3171有518 focal sites。
- Block 3171完整producer path產生3,055,612 methyl rows、3,661 co-occurrence
  pairs與99 stable assignments；wall 439.98 s、peak RSS 429,736 KiB、
  peak reorder payload 224,588,051 bytes，低於384 MiB readiness與512 MiB hard
  ceiling。Semantic SHA為
  `0dc407ad92135b76030102a1f4433dc595592d190631ba85e1ecd2204f42880e`。
- 第一次真實producer probe正確fail closed：4-thread BGZF下逐row
  `bgzf_tell()`未前進，不能建立可信index。修正後，以group-boundary flush取得
  stable offsets；八種index的order/range/contiguity/tail均零錯誤，逐group
  row-count與range SHA重算亦零 mismatch。失敗輸出保留並明確標示，不冒充成功。
- Blocks 3160–3184的1-worker reference：4,511 sites、23,253,205 methyl rows、
  29,198 pairs、wall 3,143.51 s、peak RSS 481,372 KiB、semantic SHA
  `19f02a1d9b9fdc2a9b3ac73e08da80d91180b6129a5449ad4efea8db5c4fdfe7`。
- 同一25-block window的24-worker replay：wall 570.52 s、peak RSS
  7,056,756 KiB、peak reorder 1,511,943,566 bytes；相對serial約5.50x。
  17個science/index files的physical SHA逐檔相同、總bytes皆498,019,515；
  所有science counters與semantic SHA exact match。
- Conservation exact：`raw_matched=551,322`減
  `RG-only duplicate occurrences=86,224`等於`raw_expected=465,098`；
  latest-tag exact joins為378,874，沒有fallback。
- Readiness仍明確為`PARTIAL`，`production_claim_allowed=false`。正式claim只由
  committed binary執行全79,687 sites、獨立validator replay並原子freeze成
  `VALIDATED_FROZEN_DATASET_GATE`後成立。

## HCC1395 formal attempt 1 與 IUPAC reference hotfix（2026-07-20）

- 第一次正式dataset-gate使用manifest SHA
  `ff42a18fed13b59f63047a4ae0f62756176b32562081068ff7ee44503385c510`、
  source commit `5e7b16c43831dbe36c640946ec9cc8dd3888aeda`與producer SHA
  `58b473e46cc9d5bee640878ea5ecf2b7c93d34651e887f90f4339242384b12af`。
  它在42:58.10、peak RSS 6,724,292 KiB後，以exit 9 fail closed；block 681
  的GRCh38 context含合法DNA IUPAC `M`與`R`，而reader錯誤只允許`ACGTN`。
- 失敗staging只有五個未封閉partial artifacts，沒有producer、validation或
  run receipt；已搬到restricted run workspace的`failed_runs/`，partial bytes
  與time log均保留，禁止作任何science count或validation evidence。
- [決策] Reference reader接受並保留uppercase DNA IUPAC
  `ACGTRYSWKMBDHVN`；不猜測ambiguity allele，也不不可逆collapse成`N`。
  Reference CpG仍只由literal `C`後接literal `G`成立，任何含ambiguity的motif
  一律拒絕，不得產生methyl call。
- [決策] `U/X/Z/-/./*`及其他非DNA-IUPAC bytes仍fail closed；錯誤現在包含
  contig、1-based位置與decimal byte，避免下一個invalid reference只得到模糊
  訊息。
- Unit falsifier覆蓋完整15-symbol大小寫IUPAC、非法`Z`、以及每個ambiguity位於
  `?G`或`C?`時皆為0 CpG admission。持久producer integration另在halo加入
  lowercase `m/r`，並鎖定既有exact-CG methyl rows不增加。
- 同一真實block 681 bounded replay修復後exit 0：6 focal sites、6 markers、
  1,566 unique reads、39,135 matrix cells、206,175 admitted literal-CG calls；
  wall 2.64 s、peak RSS 251,596 KiB。此結果仍標
  `REAL_BLOCK_PARTIAL / production_claim_allowed=false`。
- 真實blocks 680–682的producer regression以1-worker與24-worker各重跑一次：
  兩次皆為15 sites、1,084 site-read rows、29,399 methyl rows、896 exact
  latest-tag joins，且17個science/index files逐檔physical SHA完全相同，
  semantic SHA均為
  `0f7fea59de15c7051db505fa7d127da5f533c6a65b4bb336a58732b6a9e48e89`。
  此三block工作量下24-worker因handle與thread啟動成本較慢（9.88 s vs
  5.39 s），因此只作determinism/IUPAC regression，不外推為全量效能結論。

## HCC1395 formal attempt 2 與 iterator resource-gate hotfix（2026-07-20）

- 第二次正式dataset-gate使用manifest SHA
  `e9dda5e92fc3b454fcbdb9d148dbfb4d8e03b906e89c283a232e1276f7a6fa91`、
  source commit `2a87853b652199b78315aebe295f54079001809f`與producer SHA
  `28a4c1df0801c30fb19b65f5bb8f5ffc831d99291e92e3f4383552087ed6b3d5`。
  它已越過block 681 IUPAC案例，但在45:15.83、peak RSS 8,220,668 KiB後，
  於block 2117以exit 9 fail closed；raw BAM indexed iterator命中704,276筆，
  超過舊的250,000 raw-record ceiling。
- 失敗staging只有五個未封閉partial artifacts，沒有producer、validation或
  run receipt；已搬到restricted run workspace的`failed_runs/`，並建立
  `failure_record.json`。partial bytes與原始time/stderr log均保留，禁止作
  science count、validation evidence或後續run輸入。
- 精確forensics顯示block 2117的兩個focal sites相距214,120 bp；224,121 bp
  fetch interval掃入704,276個raw hits，其中556,230筆由FLAG排除、142,346筆
  MAPQ不足、771筆query過短，固定filter後僅保留4,929筆（0.6999%）。
  `556230 + 142346 + 771 + 0 + 4929 = 704276`守恆完全成立，沒有BAM/BAI
  corruption證據。
- [決策] Raw iterator hits是streaming telemetry，不再作記憶體proxy；250,000
  fail-closed上限移到固定FLAG/MAPQ/length/MM/ML filters後的filter-eligible
  records，另以512 MiB限制實際decoded retained evidence。這不改read filter、
  CIGAR投影、sidecar exact join、site/block identity、M1、co-occurrence或
  topology規則。
- [決策] `BlockReadPolicy`只承載不可變科學條件；record/byte ceiling分離為
  `BlockReadResourceLimits`，避免測試或呼叫端把資源限制誤認為可調科學政策。
  超限分別回報`POSTFILTER_RECORD_LIMIT`與`DECODED_LOGICAL_BYTE_LIMIT`，兩者
  仍是fail closed。
- Synthetic邊界測試鎖定exact-limit成功與limit-minus-one失敗。真實block 2117
  replay得到704,276 iterator records、4,929 retained records、4,149 unique
  reads、193,622,549 decoded logical bytes；wall 62.93 s、peak RSS
  469,324 KiB、0 swap、exit 0。
- 真實blocks 2116–2118以1-worker及24-worker各執行完整producer：
  兩者皆為13 sites、1,157 site-read rows、30,041 methyl rows、23
  co-occurrence pairs、6 stable assignments；17個science/index files逐檔
  physical SHA完全相同，semantic SHA均為
  `7fa542827c1beee258739dfe118c5c475c80bd5e5241ca42749df1fa6771962c`。
  w1 wall 67.44 s、peak RSS 472,348 KiB；w24 wall 70.93 s、peak RSS
  3,532,652 KiB。此小窗口只證明determinism與hotspot可執行，不作全量加速結論。
- 一次局部probe依摘要手動重組路徑時誤把`research_rounds`寫成
  `observation_workspaces`，在0.01 s、開啟VCF前退出1；後續改為直接從frozen
  manifest解析絕對路徑。該操作錯誤不含科學運算，也不列入PASS evidence。

## HCC1395 formal attempt 3 與零群組 Schema 1.0.1（2026-07-20）

- 第三次 producer（v3 iterator）已完整跑完79,687 sites，輸出
  7,578,727 site-read rows、261,130,339 methyl rows、134,278
  co-occurrence pairs；producer self wall 3,770.18 s，outer wall因重算
  292 GB BAM input SHA為1:40:39，peak RSS 15,648.24 MiB，0 swap。producer只宣告
  `READY_FOR_VALIDATION`，不等同freeze。
- 第一個獨立validator於75:29、peak RSS約15.0 GiB後exit 7：
  `JSON array length is outside schema bounds`；未建立final root，
  `production_claim_allowed=false`。失敗receipt與logs保留，禁止改寫成PASS。
- 後續full-array forensics確認M1 78,629筆assignment、1,146,026個split、
  co-occurrence inner K×2 rows及其他bounded arrays均符合契約。唯一衝突是
  134,278個pair中的11筆合法tail case：
  `group_count=0`、`group_allele_counts_json=[]`、
  `n_informative=min_group_n=ref_n=alt_n=0`，且endpoint A明確not-testable。
- 帶artifact、1-based data row、field與observed/min/max的fail-fast validator
  在14.03 s、peak RSS 542,996 KiB重現第一筆：
  `cooccurrence_pairs` data row 433、observed 0、舊schema minItems 1/maxItems 10。
  這排除resolver、canonicalization與資料損壞假說。
- [決策] 不捏造`[0,0]` group、不刪除pair、不對舊receipt作validator特判。
  保留1.0.0 schema與原SHA，新增`group_allele_counts`及
  `cooccurrence_pairs` 1.0.1；新版本接受0–10 groups，仍鎖定每列K×2、
  array-length、matrix totals、minimum及canonical SHA守恆。
- [決策] v3保持failed schema-contract evidence；catalog、ID registry、
  contract registry與producer metadata全部綁1.0.1，使用新run ID v4全量重產。
  這遵守historical schema不覆寫與receipt append-final原則。
- [決策] validator同時補足embedded `schema_id == nested $id`檢查、
  Draft-07 tuple `items:[...]`逐index canonicalization，以及按physical size
  決定性fail-fast replay；解析結果仍以artifact ID map回傳，科學順序與結果
  不變。
- Positive regression新增合法`[]` fixture；既有non-empty及malformed K×2
  fixtures保留。JSON schema suite與producer/validator兩個targeted CTest均
  exit 0。正式claim仍等待v4 full producer、獨立freeze及不同worker
  determinism replay。

## Method/performance audit snapshot

- Historical strategy labels are separated into MEASURED, DERIVED, ESTIMATED and
  PLANNED; candidate-window and topology bounded results are not promoted to
  production claims.
- Native catalog implies 16 artifacts plus 8 indexes; the 24-file target is a
  contract-derived count pending P7 filesystem census.
- BGZF row-payload local benchmark: 500,000 rows, 8+8 trials, median
  `0.9010325 s → 0.8295770 s` (`7.930402067%` component wall reduction);
  logical bytes, physical bytes and semantic SHA are identical.
- Machine record:
  `state/benchmarks/20260719-bgzf-row-payload-local.json`; it explicitly forbids
  a production claim.
- The 2026-07-19 topology/VAF handoff and both cited R3 receipts were replayed
  read-only: three machine predicates, two receipt hashes and seven source
  bindings passed. The result remains `NO_GO_PRODUCTION`.
- [決策] ADR-0005 separates objective certification, complete-family
  enumeration and ranking completion. Primary BQ-aware likelihood scores each
  candidate vertex set once and cannot select parent edges within that set.
- [偏離] The existing `topology_unit` 1.0.0 schema has only objective/family
  states and additive parent-score winner semantics. No production ranker will
  target it; a versioned schema migration is now an explicit P5 blocker.
- [折衷] Exact compressed output for very large candidate families is not
  invented in this audit. Explicit output remains the contract; compression
  requires a separate ADR, expansion/count oracle and query semantics.
- GitHub device authentication succeeded. A restricted-network `gh auth status`
  replay initially misreported the fresh token as invalid; the allowed-network
  replay passed without exposing credentials. `liaoyoyo/LongLineage` was
  created PRIVATE, `main`, `feat/p2-block-pipeline` and
  `docs/method-performance-audit` were SHA-verified after push, and draft PR #1
  was opened.

## HCC1395正式w24/w40 closeout（2026-07-21）

- 本輪完成HCC1395 chr1–22單資料集完整science gate；scope是1/7、
  `production_claim_allowed=false`，不是P7七資料集或P8 release。
- Production-facing input是raw MM/ML BAM加frozen latest HP/PS sidecar，
  `latest_tag_join=EXACT_PROJECTION_NO_FALLBACK`。沒有persisted tagged BAM、
  BAM舊HP fallback、truth BED、truth VCF或truth-derived production field。
  一個bounded BAM檢查在1,004 reads中見MM=1,004、ML=1,004、HP=0、PS=0；
  全量sidecar exact joins為6,186,297，missing/conflict/multimatch皆0。
- Science producer commit為
  `061ac166a83f72e3fcbfeb33b5b0b6e43ff1e9aa`；audit/report生成commit為
  `25805c3cb69c85d0af0e49259bd07d3b04c55b94`；sanitized report commit為
  `5115c1fe5358e091784ed38ab178b6f36735e379`。
- w24 manifest SHA為
  `5e19b7fc861b814fae2b93298f26b677bdbd774861d6d4cff022ff7e610bdb19`，
  run receipt SHA為
  `2c3bb31cf4d071396d02421bc1038577d0795faa33dbb3c9fed3984fdbc306f2`，
  validation receipt SHA為
  `c6339819899481509019bfac1f269a237fef3adc2c48476cad30998b7bc32203`。
- w40 manifest SHA為
  `acaecae13a6ce97cafc2251ed2c5af582246ba4f0109cc80fdc910362c8f2fbd`，
  run receipt SHA為
  `449769c5354cec323d1442353238693e9a8af06d6fa0c13a3f80938b63455eb5`，
  validation receipt SHA為
  `dabbe303b27933eb64b306f59650ccde86c3a50c3645eac669a4246f65680c5b`。
- 每個run均為validator 13/13 PASS、frozen checksum 21/21；兩次8個
  run-ID-invariant science artifacts的schema、row count與semantic SHA完全
  相同。C++ historical/determinism audit 8/8 PASS，receipt SHA為
  `9f317a643b62cc9c068005e74f49d88ce60a070c7d2950f87769e5ca9af7050c`。
- 結果為79,687 sites、7,578,727 site-read rows、261,130,339 methyl rows、
  78,629 M1 evaluable、12,851 stable、134,278 co-occurrence pair records、
  2個eligible exact-family pairs、1個formal BY-confirmed pair及0 topology
  units。Pair row不是positive discovery；topology 0是合法abstention，不是
  生物學上「沒有結構」的結論。
- Historical M1 stable為12,838，新版為12,851；淨差+13伴隨
  true→false 1,180與false→true 1,193，symmetric difference 2,373，
  Jaccard 11,658/14,031。正式結論為`COMPARABLE_DIFFERENT`，不能稱exact
  legacy parity或「增加13個clone」。
- 舊流程沒有durable formal full co-occurrence結果；regional-tree的membership、
  CN/LOH gate與方法亦不同。因此co-occurrence與topology old/new均不得做
  zero-difference或直接數量parity claim。

### 完整時間與效能判讀

| 階段 | w24 | w40 | 判讀 |
|---|---:|---:|---|
| C++ science core | 3,448.2765 s | 3,099.7985 s | w40快10.1059% |
| producer outer | 5,170 s | 5,200 s | w40慢0.5803% |
| pre-validator checksum | 42.86 s | 43.19 s | measured pre-stage |
| independent validator/freeze | 4,734 s | 4,796 s | 兩者皆PASS |
| trusted active-stage total | 9,946.86 s | 10,039.19 s | w40慢0.9282% |
| producer peak RSS | 16,634,988 KiB | 20,968,032 KiB | w40高26.0478% |
| peak threads | 30 | 46 | 皆在manifest ceiling內 |

- w40 read I/O是w24的4.5867倍，兩次`cache_condition=UNKNOWN`；這不是
  controlled scaling benchmark。現階段推薦w24，且不得建立「C++比Python快」
  的語言層級claim，因歷史7,061.9845秒流程是Python編排不同C++ binary、
  不同science/output/validator scope。
- External performance observation
  `<RESTRICTED_HCC1395_GATE_ROOT>/performance_observation/20260721_HCC1395_w24_w40_performance_edge_observation.v1.json`
  SHA為
  `7be3f47bb8b373d6036ab955ef8fd5c1e23e48da06e2c942a85e8fa7d50ec478`；
  Draft 2020-12 schema 0 errors、32/32 formula/source checks及41/41 file
  bindings PASS。把`production_claim_allowed`改成true的negative mutation
  正確exit 1。

### Presentation、verification與流程偏離

- Final HTML SHA為
  `b881e3aea1362d909d2524bcd01178a1ff87544293153a052be5996e53a80413`；
  JSON SHA為
  `14c512ece5f6bb108fc3e59a42ca48cab2a91ec29ac95cb0ef6b44b2bed4cc8e`。
  Browser QA receipt SHA為
  `f35cfe46d6e7f8fd43d77397e8ca3cb2fc5221f172b7d992d898a2ce9fbc0473`：
  desktop/mobile各12/12、print、offline、console/page/external-request皆PASS。
  補充keyboard/contrast為10/10 PASS，最低contrast ratio 5.7488648723。
- Final source verification在Debug、Release、ASan/UBSan皆44/44 CTest PASS；
  `ASAN_OPTIONS=detect_leaks=0`只表示此環境沒有leak-only coverage，不得描述成
  完整LSan PASS。Patch-scoped format/privacy皆PASS，但repo-wide歷史hygiene
  debt仍存在。
- Report生成後兩個額外Python assertion曾先後引用不存在的頂層`status`與錯誤
  的historical M1欄位路徑，皆在讀取後、任何mutation前fail closed。主agent
  讀取sealed report結構後改用正確欄位，26/26 validator checks、8/8 artifacts、
  counts、privacy與SHA重跑PASS；這兩次operator-side檢查錯誤不冒充report failure。
- 補充keyboard probe attempt 1因測試腳本保留URL hash又假設固定Tab順序，
  造成2個focus checks失敗；9/9 contrast本身已PASS。失敗receipt保留，
  修正版從無hash URL以實際focus loop重跑10/10 PASS，未修改HTML或builder。
- 另一個共享AI session在主agent讀取後同時更新ROADMAP、CURRENT_FOCUS、
  phase ledger、project state與task heartbeat，造成兩次`apply_patch`
  verification fail closed。主agent沒有覆寫，等待寫入穩定後逐檔審核、
  重播report SHA與compiled governance 7/7，再以`fc2f467`接收。該session曾把
  `state/project_state.json`加入task claim並於同一未提交batch修改；此事如實
  記為process deviation，不倒填成事前獨立child lease。

## HCC1395 report顯示比例與phase-scope incident（2026-07-21）

- [偏離] 外部report稽核發現舊HTML把M1 incomplete-distance的14/79,687顯示成
  0.20%，但正確比例為0.01757%（四捨五入0.02%）。底層JSON count與所有守恆
  皆正確；錯誤只在presentation builder把最小可見bar寬度同時當成顯示值。
- [決策] `bar_row()`分離`actual_percent`與`visual_width`：文字使用前者，
  CSS最小0.20%只用後者。positive self-test明確鎖定
  `style=width:0.2000%`但文字為`0.02% of 79,687`。
- [決策] `phase_status_at_producer_closeout`改為
  `producer_run_local_phase_status`，並固定scope為
  `RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER`，避免把run-local
  P0–P5誤讀為project phase promotion。
- [驗證] `python3 presentation/build_hcc1395_validated_report.py --self-test`
  exit 0，positive fixture PASS、49/49 negative cases PASS。舊report SHA在重生
  與browser QA前視為待替換，不再宣稱final。
- [未決] P8仍需important `claim_id`／`data-claim-id`及獨立registered
  `longlineage.hcc1395_validated_report@2.0.0` closed-shape schema；這不改底層
  C++ data correctness，但阻止external machine-readable handoff完成。

### Hotfix closeout與最終binding

- [偏離] 第一輪v3 evidence assembly在execution-evidence envelope建立期間發生
  同時寫入競態；該份`ed516`鏈已搬入restricted
  `superseded_race_ed516/`，不得作final authority。最終v3b從immutable
  `a3e41c9952037c72ec04f0dc0bacfe3fdd441895` source checkout重建，不沿用
  競態中的report JSON。
- [驗證] Release重新configure/build皆exit 0；Release CTest 44/44 PASS，
  85.55秒。C++ audit exit 0，receipt SHA
  `f8bd82eb…493`；最終execution evidence v3b SHA
  `cbb5a07e…2d44d`。
- [驗證] Final standalone HTML SHA `9cd86244…5fda`，machine JSON SHA
  `7e0b650b…62eb`。HTML文字為`0.02% of 79,687`，CSS visual width仍為
  `0.2000%`；machine JSON具有明確run-local phase scope且
  `production_claim_allowed=false`。
- [驗證] v3b static browser QA與keyboard QA皆PASS；desktop/mobile document
  scroll width分別為1440/1440與390/390，console/page/external-request error
  均為0，最低contrast ratio為5.7488648723。Print PDF為A4、17頁、無JavaScript，
  但`Tagged: no`仍列為P8 accessibility缺口。
- [驗證] Final performance observation v3 SHA `0599332b…10d3`；Draft 2020-12
  schema、`production_claim_allowed=true`負例、32/32 invariants及41/41 source
  size/SHA bindings重播；`checksums_v3.sha256`（SHA
  `93c3ef9a…ff55`）獨立封存record與10個最終驗證檔案。
  v1→v3只更新presentation/evidence binding與發布blocker描述；C++ counts、
  八項semantic SHA、runtime與六個edge observations均未改變。
- [偏離] v2曾把會顯示observation SHA的human query guide也列為source，造成
  循環provenance。v3改為observation單向綁定guide，guide只指向不被observation
  反向綁定的`checksums_v3.sha256`，保留41個source closure且打破循環。
- [偏離] v3 validator第一次以未加pinned module path的system Python執行，
  在import `jsonschema`時exit 1，尚未讀取record或產生negative fixture；失敗
  log保留。加入既有`<PINNED_JSONSCHEMA_PATH>`後重播exit 0、32/32 PASS。

## Python-compatible regional topology（2026-07-21，完成）

- [決策] 新增獨立 `PYTHON_V2_DESCRIPTIVE_REGIONAL` profile；它重現舊
  50 kb/HP-family描述性端點，不修改formal M2/topology gate，也不能宣稱
  clone、ancestor或時間方向。契約見ADR-0007。
- [決策] Fair comparison固定讀取bip7主機上的同一份
  `restricted://hcc1395/raw_bam`，
  加July truth-free sidecar與PASS VCF。三月persisted tagged BAM曾以truth
  VCF/BED產生，只能evaluation，禁止作production-compatible輸入。
- [驗證] C++ synthetic test已鎖定50,000/50,001 boundary、transitive
  component、densest-eight first tie、HP prefix、MINREAD fail-closed及第一個
  HCC family unit。Release warnings-as-errors build與CTest均PASS。
- [驗證] C++直接讀五個frozen MLHP JSON，在27.63秒、48,460 KiB RSS內重播
  8,222 regions與20,119 units；all-unit class為12,738 determined、6,395
  ambiguous_structure、956 capped、30 recurrence；primary為4,217/5,875/
  784/28，全部exact。
- [偏離] 第一輪100-region BAM probe雖有100/100 region與808/808 pattern
  exact，但兩個capped fallback的非決策`n_hidden/n_trees`因C++ set iteration
  不同而漂移。沒有忽略；實作CPython 3.9 frozenset hash、bulk set-merge、
  table resize與set-copy iteration後，兩個translated synthetic golden分別固定為
  `7/4`與`9/4`，再重播20,119 units逐欄exact。
- [驗證] 獨立validator不連producer core；synthetic valid/corrupt/missing/
  extra/truncated/order/row-count/partial-probe矩陣PASS。實際probe被exit 7拒絕
  freeze，未產生validation receipt或FROZEN。
- [驗證] 正式24-worker全autosome C++/HTSlib run在298.8119秒science wall、
  302.26秒outer wall完成；8,222 regions、20,119 units、106,559 patterns，
  validation receipt 13/13 pre-publication checks PASS，atomic receipt/FROZEN
  transaction亦PASS。validation receipt SHA為
  `aac8234081d042e88e9be292f6a4d6119471bed4592d2ae2d34e0172b01e1053`。
- [驗證] Frozen Python crosswalk採key-map而非row-order：8,222 region shared rows、
  20,119 unit shared rows與106,559 exact pattern rows皆mismatch=0；三個canonical
  digest依序為`fc4d252b…4850`、`da2d26a2…48dc`、`82eaff81…4242`。
  Read exposure與sidecar exact join皆1,934,226，identity conflict=0。
- [驗證] 100-region controlled worker matrix中，workers=1與24的三份TSV皆
  byte-identical；science wall為14.9467與7.3334秒（2.038x）。24 workers實際
  建立25 threads；full run 10秒telemetry平均309.2% process CPU、約
  113,753.6 KiB/s讀取，顯示主要瓶頸為NFS BAM I/O與region depth偏斜，solver
  summed time僅28.7839秒。
- [折衷] 未重跑完整workers=1 BAM：由24-worker summed input time推估會額外
  佔用約1.9小時NFS讀取，且不增加Python parity證據；determinism以相同BAM的
  bounded 1/24 worker byte comparison加全量24-worker crosswalk證明。這是對原
  task草案「full w1+w24」的明示偏離，不冒充full-w1驗證。
- [折衷] Repo規範禁止重新執行historical Python science，因此不建立新的
  controlled Python timer。Frozen歷史觀察由worker檔建立到HCC manifest完成為
  5,086.676秒；相對validated warm-cache C++為17.023x，但因舊run是四樣本
  scheduler observation且cache不同，`full_controlled_speedup_claim_allowed=false`。
  第一輪近冷cache C++ engine為534.462秒（約9.517x），但該run因上述兩個
  capped display field而未freeze，只作工程效能baseline。
- [驗證] 最終Release build exit 0，完整CTest 46/46 PASS（含新
  `regional_compat`與`regional_compat_validator`）；governance `check-all`、
  30-row gate registry、regional正負gate binding與source-to-target JSON schema
  均PASS。重跑已freeze bundle的validator會以exit 7拒絕覆寫，符合immutable
  output transaction，而不是validation regression。
- [偏離] Canonical portable report第一次因64-byte digest欄與寬表觸發桌面
  overflow；縮減display-only欄後仍發現共用renderer的`100vw` top bar在15 px
  傳統捲軸下多出8 px。最終交付器只加入scrollbar-safe top-bar width override，
  不改artifact、chart或數據；正式QA在1440/390 viewport、source dialog與鍵盤
  semantic click全PASS，20 blocks、1 chart、7 tables。
- [驗證] Repo-wide hygiene仍只列出既存13個baseline private-path檔；本次新增檔
  無新failure。Repo-wide source-boundary仍被既有
  `presentation/build_hcc1395_validated_report.py`觸發；本次兩個presentation檔
  的forbidden-reader pattern為0。這些既存debt不倒填為本次修復成功，也不阻止
  scientific parity結論，但持續阻止公開GitHub push。
- [偏離] 系統`/usr/bin/jsonschema`為3.2.0，只支援舊draft，會把2020-12
  `prefixItems`誤判成`items:false`而拒絕合法tuple；該次輸出未當成schema
  verdict。改用既有pinned jsonschema 4.26.0依`$schema`選validator後，summary、
  producer receipt與validation receipt三份2020-12 closed schema皆0 error。
- [驗證] 本機原先沒有`clang-format-14` executable；從Ubuntu pinned package
  解出14.0.0-1ubuntu1.1後，僅對本次10個新增C++ header/source/test做機械格式化，
  再以`--dry-run --Werror`重播PASS。Repo-wide舊格式debt仍存在，未以大範圍
  reformat混入本次科學修復。

## Python-compatible regional topology 七樣本擴充（2026-07-21，進行中）

- [決策] 本輪是task type B完整驗證，exact scope固定為`HCC1395`、
  `HCC1395_DORADO`、`COLO829`、`H1437`、`H2009`、`HCC1937`、`HCC1954`；
  每個樣本內24 workers、樣本間依authority順序序列執行。科學kernel不變，
  仍是50 kb transitive grouping、densest-eight、HP family、MINREAD=3與frozen
  legacy solver；改動限於I/O authority、dataset selection、receipt、validator、
  crosswalk與cohort orchestration。
- [決策] 七樣本新run只接受`PRODUCTION_7_DATASET` source manifest與repo內
  `regional_compat_all_datasets_input_authority.json`。八個角色皆需full physical
  SHA-256；六個非HCC1395 raw BAM既有receipt只有sampled identity，因此必須先做
  約1.65 TB的一次性full scan，不能把size/mtime冒充SHA。
- [驗證] Frozen Python v5 corpus已由C++ oracle replay七樣本；預期總數為
  51,815 regions、118,234 units、679,343 patterns。這只是Python authority
  baseline，不冒充production BAM run結果。
- [決策] Validator v2新增absolute repo trust root，獨立重播七樣本authority、
  base production authority、exact dataset order及八個role/size/SHA；同一份bytes
  同時parse/hash，結束前再重播identity與SHA。Validation receipt固定18個
  pre-publication checks，新增`SOURCE_AUTHORITY`、`SOURCE_MANIFEST`、
  `OUTPUT_CENSUS`、`SOURCE_AUTHORITY_STABLE`與`SOURCE_MANIFEST_STABLE`。
- [驗證] Validator從`regions.tsv`與`units.tsv`重算6-class unit、6-class primary
  與5-class region determinacy census；summary prefix maps需exact相等，且
  `emitted regions + read_unsupported_regions = multi_regions_pre_read`。Synthetic
  正向、缺repo、wrong authority、authority input drift、census drift、embedded-NUL
  等負例均PASS；父端Werror build及integration test exit 0。
- [決策] Crosswalk的`all_exact`只代表所有**共享描述性欄位**逐鍵完全一致；
  明確排除C++ region-order/pre-cap diagnostics、`n_feasible_node_sets`及Python
  post-tree CN/LOH。C++ cohort再重播七個FROZEN bundle、checksums、三份TSV、
  validation receipts及crosswalk receipt bindings；crosswalk receipt本身尚無
  independent validator/FROZEN，因此輸出固定
  `crosswalk_receipts_independently_frozen=false`，不得寫成獨立freeze。
- [驗證] Cohort core synthetic七樣本self-check為7 regions、14 units、21 patterns，
  order/FROZEN/receipt/TSV SHA/crosswalk mismatch/render-tamper及mixed source
  manifest／validator共十類負例皆拒絕；
  renderer以`chart_payload_sha256`重播所有chart-ready counts、class census與
  timing，並拒絕unknown census keys。
- [折衷] 2026-07-21 12:34至12:51主機load1維持63.8–64.8，`vmstat`顯示約
  57–63個blocked tasks及65–68% I/O wait，高於事前記錄的`max_load=60`。
  因此尚未啟動raw BAM full hash或七樣本science；等待期間只做small-file code、
  schema、build與synthetic tests。這是資源gate的可稽核延遲，不是把未跑的結果
  宣稱完成。
- [偏離] 後續逐PID確認blocked load來自8個已執行27分鐘至8小時的廣域唯讀
  `rg/find`，合計63個D-state threads，並非science或寫檔程序。只對這8個stale
  search送出SIGTERM，保留root `updatedb`與所有資料程序；load1由約65降至47.7、
  D-state歸零後才開啟hash gate。這也落實本任務「不用廣域搜尋掃大目錄」的限制。
- [偏離] 第一版hash batch的SHA讀取正確target，但log以`stat`記到H1437 symlink
  本體75 bytes。該attempt在尚未產生H1437 digest時以exit 130停止，三個檔案完整
  搬至`input_hashes/archive/attempt1_lstat_size/`，未刪除或覆寫；正式batch改用
  canonical non-symlink target與`stat -L`。COLO829獨立hash已完成，raw BAM digest為
  `ef940b7df33cd5b423e74a44b27958d5697e44dc402301ca1a50e320eafc5c9a`。
- [決策] 五個Google ONT入口是symbolic-link aliases，而production manifest
  parser明確拒絕任何symlink component。Manifest builder因此要求外部path map提供
  canonical regular-file targets；repo不保存alias或target私有路徑。內容、size與
  SHA不變，但路徑契約由必然fail-closed修成可執行且無alias的authority path。
- [驗證] 新增獨立`schema/compat/catalog.json`，把summary、producer receipt、
  validation receipt及cohort receipt四份closed schema做SHA lock；compat catalog
  明確`production_catalog_member=false`，避免污染formal production catalog。
  Catalog正向、unknown-field負例與逐檔SHA replay皆PASS；完整CTest當時46/46 PASS，
  catalog補入後的`json_schema_contract_fixtures`亦單獨PASS。
- [決策] 七樣本report builder只讀C++ cohort receipt、wrapper GNU-time logs與
  frozen Python lifecycle，拒絕不完整FROZEN/crosswalk/census。Python只建立SQL
  presentation projection、圖表與portable HTML，不開啟BAM、VCF或sidecar，也不
  重算任何科學分類；controlled speedup claim固定禁止。
- [偏離] Fresh-reader複核指出四個發布前缺口：system `jsonschema` 3.2.0不支援
  Draft 2020-12、batch log重複`OUTPUT_ROOT`、cohort仍可混用不同manifest／validator、
  report只檢查chart/source digest格式。全量science因此維持未啟動，先修正而不把
  syntactic PASS冒充provenance PASS。
- [決策] Cohort aggregation與render-time replay現在都要求七份summary共用同一
  `source_manifest_path`、manifest run/SHA及validator executable SHA；batch wrapper
  只輸出唯一key，另綁定producer/validator canonical executable path與SHA，批次結束
  再重播兩個binary SHA並輸出`EXECUTABLES_STABLE=1`。Producer executable只屬
  batch-execution provenance，未冒充sample receipt-bound欄位。
- [驗證] Report不再呼叫不相容的`/usr/bin/jsonschema`；內建的fail-closed Draft
  2020-12 subset先稽核schema keyword，再完整處理本cohort schema所用的local `$ref`、
  `prefixItems`、`items:false`、`allOf`、`oneOf`、closed object與數值邊界。它另以
  C++相同canonical row與hexfloat規則重播`source_set_sha256`及
  `chart_payload_sha256`。Synthetic正例與prefix/order、bool-as-int、digest tamper
  負例均拒絕；Python只讀JSON/log並渲染，不讀BAM/VCF/sidecar。
- [驗證] 最新warnings-as-errors targets build PASS；regional validator integration
  PASS；cohort self-check為`negatives=10`；compat schema catalog fixtures PASS；完整
  CTest 46/46 PASS（74.92秒）。這些驗證在science啟動前完成，避免把數小時全量run
  建立在尚未關閉的provenance contract上。
- [偏離] Final-three raw BAM authority SHA改為三路低優先序並行後，report逐檔
  provenance欄仍沿用「sequential」固定字串；在正式report產生前修成依dataset
  顯示`concurrent final-three`或`sequential`。這只修正證據措辭，不改C++、digest、
  timing或科學輸出；source compile與`git diff --check`皆PASS。
- [決策] Formal-run readiness reviewer確認唯一當下hard blocker是最後三份raw BAM
  SHA尚在執行，同時指出outer batch沒有結束時manifest identity重驗、crosswalk
  wrapper沒有binary lifecycle log。正式science前補成：batch結束重驗manifest
  SHA與device/inode/size/mtime/ctime；crosswalk/cohort另寫64-row exact-key log，綁定
  同一compat executable前後SHA、七份命令/輸入/輸出digest與cohort digest。Report
  會重播此log，但仍誠實維持`crosswalk_receipts_independently_frozen=false`。
- [驗證] 兩支shell `bash -n`及report source compile均PASS；synthetic七樣本
  crosswalk wrapper產生7份exact output、64-row無重複key lifecycle log，前後binary
  stable且cohort SHA exact，self-check exit 0。這是provenance hardening，不改C++
  science binary或拓撲kernel。
- [驗證] 六個需新算的raw BAM full-content SHA全部完成：`HCC1395_DORADO`
  `cfb50ccb…a1bdb`（51:25.61）、`COLO829` `ef940b7d…5c9a`（15:55.61）、
  `H1437` `1d236130…778`（37:46.73）、`H2009` `45ccc847…d10d`（2:13:05）、
  `HCC1937` `208aace3…e162`（2:35:34）、`HCC1954` `77b59276…c65`
  （1:51:58）。Final-three log有exact begin/done/batch-done，identity皆stable，所有
  GNU-time exit 0且無`.partial`；獨立6/6 receipt SHA為`57887100…fd42`。
- [偏離] Raw-hash verifier首次因`/tmp`腳本未有executable bit而在進入驗證邏輯前
  exit 126；沒有建立receipt或partial。補`chmod 0755`後以同一inputs與全新output
  重跑PASS。其後一個naive文字掃描把required policy keys `truth_fields:0`與
  `persisted_tagged_bam_allowed:false`本身當成敏感命中；authority未修改，改用語意
  closed-shape whitelist重播後PASS。
- [驗證] Path-free七樣本input authority在1:33.95內atomic發布，SHA為
  `7ae674af92af1a2328370c4351c2e3251689932af17034c8c1dbca1d408f81fb`；exact
  7 datasets、56 rows、8 roles/order、兩份base authority SHA皆符合，
  `truth_fields=0`、`persisted_tagged_bam_allowed=false`、
  `private_source_paths_stored=false`。
- [驗證] Descriptive compatibility v1七樣本producer batch（非P7）於
  2026-07-21 17:18:42+08啟動，固定authority
  order、樣本間sequential、樣本內24 workers且未使用`--resume`。`HCC1395`於
  18:13:59 FROZEN：8,222 regions、20,119 units、106,559 patterns；input SHA
  2,819.146秒、science 494.651秒、E2E 3,316秒。獨立AI唯讀重播5/5 artifact
  checksum、18/18 pre-publication checks、region/unit/pattern keys、FK與守恆皆PASS，
  truth token及`truth_fields_seen`均為0。
- [驗證] `HCC1395_DORADO`於19:05:15 FROZEN：8,385 regions、20,267 units、
  80,995 patterns，三項aggregate均與frozen Python authority一致；input SHA
  2,588.786秒、24-worker science 484.602秒、E2E 3,075秒。Semantic SHA為
  `1ee66c81f658107c652d66a619b814825378b79770c32476e5896f10f66f6f2b`；
  5/5 checksums、18/18 persisted pre-publication checks、FROZEN transaction、
  source manifest/authority stability與`truth_fields_seen=0`均PASS。
- [驗證] Runtime telemetry顯示兩樣本皆先以1 thread做full physical SHA，之後才
  切換為主執行緒加24 workers；DORADO full-SHA約43.15分鐘、science約8.08分鐘。
  因此前置NFS sequential scan是目前主要wall-time瓶頸，並非拓撲kernel未平行化。
  `COLO829`已於19:05:15依同一fail-closed流程接續執行。
- [驗證] `COLO829`於19:25:20 FROZEN：8,007 regions、17,613 units、73,222
  patterns；input SHA 1,030.860秒、science 172.978秒、E2E 1,205秒。
  `H1437`於20:19:02 FROZEN：9,238 regions、19,668 units、131,108 patterns；
  input SHA 2,476.390秒、science 742.837秒、E2E 3,222秒。兩者三項aggregate皆
  等於frozen Python authority，5/5 checksums、18/18 persisted checks、truth-free
  與false claim ceiling重播均PASS；H2009已依序啟動。
- [偏離] 外層read-only telemetry sampler在H1437 validator子程序退出的瞬間，
  PID已從`/proc`消失但該輪仍嘗試讀取`io/status/cmdline`，因此輸出數行
  `awk: cannot open /proc/...`。這是15秒抽樣的process-lifecycle race：producer與
  validator exit皆0、FROZEN及bundle receipts不受影響；最終效能報告不得把該瞬間
  缺列補值或冒充完整sample，並需另外驗證monitor receipt的outer exit code。
- [驗證] `H2009`於21:38:21 FROZEN：9,674 regions、21,973 units、187,471
  patterns，三項均精確等於frozen Python authority；input SHA 3,676.220秒、
  science 1,080.060秒、E2E 4,759秒。5/5 checksums、18/18 persisted checks、
  source authority/manifest stability、truth-free與false claim ceiling皆PASS；semantic
  SHA為`77b9c0ebf4ea8c98788562260436d131f265c646ececcb6f74db38fdcd816717`。
  `HCC1937`（本批最大raw BAM，472,119,759,082 bytes）已依序啟動。
- [驗證] `HCC1937`於23:13:55 FROZEN：3,612 regions、7,174 units、41,193
  patterns，三項均精確等於frozen Python authority；input SHA 5,202.180秒、
  science 529.768秒、E2E 5,734秒。5/5 checksums、18/18 persisted checks、
  source stability、truth-free與false claim ceiling皆PASS；semantic SHA為
  `295f91c61f8dad559161003f4c9f65fb62ba3d900cec0e05164df17ce41a4895`。
  最後一個`HCC1954`已依序啟動，尚未把6/7完成狀態冒充完整cohort。
- [驗證] `HCC1954`於2026-07-22 00:05:44 FROZEN：4,677 regions、11,420
  units、58,795 patterns；input SHA 2,744.640秒、science 361.916秒、E2E
  3,108秒。v1 descriptive bundles 7/7已產生，總wall 24,422秒（6:47:02），總計
  51,815 regions、118,234 units、679,343 patterns，逐樣本5/5 checksums與18/18
  persisted checks皆PASS，外層monitor receipt exit 0。但H2009 crosswalk後續fail closed，
  因此v1 cohort authority與P7均未成立。
- [偏離] v1第一輪C++ exact crosswalk對前四樣本三層皆mismatch=0，但於H2009
  一個已去識別化的edge-case interval之family 2 fail closed：Python為hidden=4、trees=120、
  uncapped/ambiguous，C++為hidden=3、trees=1、capped；pattern層完全一致。根因是
  `bounded_binomial`在除以index前就檢查中間乘積，將正確
  `C(45,4)=148,995 <= 150,000`誤判超額。v1 bundle與attempt-1 crosswalk完整保留，
  不覆寫、不冒充cohort完成。
- [決策] 修復採整數約分後先除再做overflow/budget guard，並以同一H2009 3 FULL
  + 10 SUBREAD witness建立回歸：必須得到120個feasible node sets、hidden=4、
  trees=120、uncapped/ambiguous；另以compile-time boundary固定C(45,4)通過而
  C(46,4) fail closed。因solver與producer binary已改變，禁止只替換H2009或混用
  v1 frozen bundle；改建全新的v2 manifest與七樣本run，完整重跑後才產cohort/HTML。
- [驗證] 修復後warnings build exit 0、clang-format-14與`git diff --check` PASS；第一次
  CTest刻意被逾時heartbeat及舊source mapping SHA擋下。更新task write-set/heartbeat與
  四筆`regional_topology.cpp` mapping digest後，第二次完整CTest為46/46 PASS
  （75.45秒）；producer SHA為`84d3c3cd…9c06`，validator維持
  `7ac5ac4e…aa82`。只有第二次log列為正式prelaunch evidence。
- [驗證] 全新v2 batch於2026-07-22 00:25:04啟動，manifest SHA
  `c266a177…485d`、樣本間sequential、樣本內24 workers、未使用resume。`HCC1395`
  於01:21:37 `VALIDATED_FROZEN`：8,222 regions、20,119 units、106,559
  patterns；input SHA 2,842.273秒、science 547.952秒、E2E 3,393秒。獨立唯讀
  稽核確認5/5 checksums、18/18 persisted checks、truth fields 0、無embedded HP
  fallback且輸入前後identity stable；三份science TSV與v1 byte-exact相同，差異只在
  新run/provenance封裝。`HCC1395_DORADO`已依authority順序接續啟動。
- [偏離] DORADO input SHA尾段時，另一session同時啟動11個廣域`rg/find`，使load1
  升至87.5、blocked tasks 97、I/O wait最高84%。為避免下一樣本在load>60時觸發
  resource gate，先SIGSTOP外層scheduler；DORADO切入science後亦於02:08:22暫停。
  這些搜尋持續約15分鐘且違反用戶「避免大量搜尋」約束後，只對11個精確唯讀搜尋
  PID送SIGTERM，未碰science、validator、服務或AI主程序；blocked立即歸零。
  DORADO於02:14:39恢復並在02:22:06 `VALIDATED_FROZEN`：8,385 regions、
  20,267 units、80,995 patterns，producer/validator exit 0且全部checks PASS。
  input SHA 2,760.220秒；science wall 865.943秒包含377秒明示pause，故不得用於嚴格
  speed comparison。load降至5.12後才於02:23:43恢復scheduler並啟動`COLO829`。
- [驗證] `COLO829`於02:44:28 `VALIDATED_FROZEN`：8,007 regions、17,613
  units、73,222 patterns；input SHA 1,055.716秒、science 186.660秒、E2E
  1,244秒。獨立唯讀稽核確認5/5 checksums、18/18 persisted checks、truth fields 0、
  無embedded fallback、identity stable，且regions/units/patterns三份science TSV與v1
  皆byte-exact。`H1437`已依authority順序接續啟動。
- [偏離 2026-07-22 03:35] 用戶重新確認原始要求是以`<LOCAL_DATA_ROOT>`本機
  truth-free production tagged BAM執行。精確provenance回查只找到
  `canonical/HCC1395/paired_full/20260314_HCC1395_paired_full_full_complete_matrix/`
  下的舊persisted tagged BAM；其`longphase_s.log`明列truth VCF與truth BED，故只能
  作evaluation，不能升格為truth-free production authority。現行v2 manifest則是
  `<BIG8_INPUT_ROOT>` NFS raw BAM加`<LOCAL_DATA_ROOT>` frozen HP/PS sidecar，雖符合ADR-0007/0008
  compatibility contract，卻不能冒充用戶指定的persisted-tagged-BAM run。
- [決策 2026-07-22 03:35] 保留已完成的raw+sidecar bundles作對照，不刪除、不改寫；
  已在跑的H1437繼續producer與validator，外層scheduler PID 3977968以SIGSTOP可逆
  暫停，避免自動啟動H2009。只有建立truth-free tagged-BAM producer、獨立fail-closed
  validator、per-sample frozen provenance與新manifest後，才能啟動用戶指定的正式全量
  tagged-BAM run；兩條run必須分開命名、分開計時與分開下claim。
- [驗證 2026-07-22 03:38] 已在跑的`H1437`於03:37:22完成並
  `VALIDATED_FROZEN`，producer/validator exit均0：9,238 regions、19,668 units、
  131,108 patterns；input SHA 2,458.682秒、24-worker science 712.662秒、E2E
  3,174秒、peak RSS 2,088,904 KiB。獨立AI只讀small-file receipt重播為5/5
  checksums、18/18 checks、truth=0、fallback=false、identity stable，且v1/v2三份
  science TSV皆byte-exact。外層scheduler仍為SIGSTOP，H2009未啟動；raw+sidecar
  control現為4/7 frozen。

## 2026-07-22 publication repair and latest-source verification

- [決策] 發布採由`origin/main`建立的乾淨分支`fix/release-repair-verify`，只移植
  最終檔案內容，不沿用含受限路徑與座標的舊feature歷史。新增history hygiene gate逐
  commit檢查新引入blob、symlink、大檔、genomics副檔名、credential、private path、
  real coordinate與Git LFS pointer；舊branch只作本機recovery reference。
- [決策] Regional input private path改為repo外必填path-map，schema固定七個dataset、
  每個dataset八個role、禁止truth role與未知欄位。freeze/build皆在發布前重算path-map
  SHA以fail closed防TOCTOU；log只記SHA，不記path-map位置或內容。
- [驗證] Repo hygiene、source-boundary AST scan、JSON schema positive/negative fixtures、
  clang-format-14與`git diff --check`已各自exit 0。Current-tree pre-commit hygiene已PASS；
  目前`origin/main..HEAD`尚為空，因此introduced-history scan及GitHub CI尚待實際
  commit後驗證。History gate對舊58-commit feature branch預期fail並偵測70項違規，
  只證明gate具負向控制，不取代本次commit範圍的正向掃描。
- [驗證] HCC1395 portable report由同一frozen JSON/receipts重建；科學欄位不變，只移除
  重複H1並加入可見`PARTIAL · DESCRIPTIVE COMPATIBILITY · NON-PRODUCTION · NOT P8
  RELEASE EVIDENCE` ribbon。Canonical portable renderer與獨立Playwright QA在1440px、390px、
  print、keyboard、external-request、console及overflow檢查皆PASS；頁面只有一個可見H1。
- [偏離] 舊v2 scheduler先前以SIGSTOP保留。2026-07-22用精確PGID 3977781送SIGTERM，
  再以SIGCONT讓stopped child處理終止訊號；確認process group為空。沒有resume、沒有改寫
  frozen artifact。任何後續全量run必須另建governed run root並重綁input authority。
- [未決] 此draft仍不是v1 release：P0-P8 ledger為0/9 VERIFIED，strict foundation gate
  仍有12個已宣告fixture-only blockers，production `run/probe/query/export/evaluate`與正式
  七資料集w24/w40 parity尚未完成。既有區域性七樣本v1和v2 4/7只能作描述性相容結果，
  不能證成正式production correctness或受控C++/Python整體加速比。
- [決策 2026-07-22] 原生`longlineage.summary`升為2.0.0，phase狀態明示為
  `RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER`，並在scope綁定精確
  `m1_representation`。歷史1.0.0 schema保留為獨立唯讀契約，不做靜默升級。
- [驗證 2026-07-22] Canonical validator freeze現在強制提供production manifest，重算
  manifest與八類input的完整SHA/identity、input snapshot/lock及site-read lineage；
  publication期間持有合作式output-base lock，並在rename前後重播完整artifact
  snapshot，拒絕已測的mutation window。這不證明非合作同UID writer race已消除。
  mutated input/manifest、wrong scope及post-rename mutation均fail closed；producer
  1/2/40-worker synthetic E2E三次獨立replay的semantic SHA相同。
- [折衷 2026-07-22] Receipt-only `FINAL_UNPUBLISHED` recovery已停用，避免只靠pending
  receipt發布未重驗artifact。可用性讓位給正確性；未來只能由validator-aware API在完整
  input與publication replay後恢復。
- [未決 2026-07-22] Durable FAILED staging receipt/state、validator-aware
  `FINAL_UNPUBLISHED` recovery及獨立HTS semantic-preflight replay尚未完成；它們與
  非合作同UID publication race、query/export/evaluate parity仍阻止P6升級。Summary
  v2 representation binding也尚未
  用於2,373-key M1 stable-membership差異的full replay，因此P3仍為`BLOCKED`。
- [驗證 2026-07-22] Fresh v4 GCC 11.4/HTSlib 1.18在warnings-as-errors下完成
  Debug與Release編譯，並各自通過47/47 CTest；Debug wall 147.93秒、Release
  wall 102.14秒，0 failed。這是repository synthetic test-suite timing，不是七資料集
  science wall，也不是受控C++/Python speedup證據。
- [驗證 2026-07-22] HCC1395 portable report以原frozen summary/receipts重建；
  8,222 regions、20,119 units、106,559 patterns與0 mismatch未改變，新增的
  TOCTOU/history文字明示non-cooperating same-UID race與post-commit CI仍待完成。
  Machine JSON SHA為`08bada86…d04daa`，standalone HTML SHA為`ad466c79…0bb19b`。
- [偏離 2026-07-22] 第一個private draft commit `b9797605…c7183`的實際
  introduced-history scan通過（1 commit、252 blobs），PR #4維持PRIVATE/DRAFT；但
  push與pull_request兩個hosted workflow的六個compiler/sanitizer build jobs均在
  `src/cooccurrence/statistics.cpp`因缺少`boost/math/distributions/beta.hpp`失敗。
  History與browser QA job通過，故這是CI dependency declaration落差，不是科學結果
  或HTML內容失配；失敗run不列為completion evidence。
- [修復 2026-07-22] Hosted runner與container builder均加入`libboost-dev`，dependency
  lock固定Jammy Boost 1.74.x；CMake加入`find_package(Boost 1.74 REQUIRED)`與
  `Boost::boost`，讓缺少header時在configure fail closed。正向lock、移除hosted Boost、
  移除container Boost與禁用CMake Boost的負向控制均PASS；fresh Release configure
  解析Boost 1.74.0，warnings-as-errors build exit 0，完整`check_all`為47/47、
  no-network PASS與`FOUNDATION_PASS`。修復後current-head hosted CI仍須重跑全綠。
- [偏離 2026-07-22] Boost修補後的hosted矩陣已證明GCC/Clang Debug與Release、GCC
  TSan、history及browser QA可通過，但又揭露三個獨立lifetime/container問題：Clang
  warnings-as-errors拒絕一個零引用常數；LeakSanitizer在validator找到41個未decref的
  Jansson空object，並在audit negative callback找到22個未走手動close/free的BGZF
  streams；pinned context因刻意排除`.git`而得到zero commit，且builder未安裝供report
  self-test建立synthetic repo的`git`。兩輪失敗run均只作negative evidence。
- [修復 2026-07-22] 已刪除零引用常數；validator以lazy `JsonPtr`持有並重用
  unconstrained schema；compressed audit reader以BGZF/kstring RAII覆蓋exception path。
  GCC ASan+UBSan且`detect_leaks=1`重跑原三個失敗測試為3/3 PASS（75.46秒），未關閉
  leak detection。Pinned build改由workflow注入`GITHUB_SHA`，Docker與CMake都拒絕
  非40位、非lowercase或全零值，`.git`仍不進context；builder新增lock-recorded Git。
  Test-only sentinel SHA的完整image build exit 0、container no-network 43/43 PASS，原
  audit/report兩測試分別PASS 0.95秒與6.83秒。整合後current-head hosted CI仍待重跑。
- [驗證 2026-07-23] Dirty-tree precommit整合驗證完成：fresh GCC Release
  `check_all.sh`為47/47、102.58秒、no-network PASS；clean-rebuilt GCC Debug
  ASan+UBSan在`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`下為47/47、447.49秒、
  no-network PASS。第一個sanitizer replay因舊build內governance binary早於最新
  `CMakeLists.txt`而46/47（exit 8）；clean rebuild後該readiness test與完整suite均PASS，
  故失敗run保留為build freshness negative evidence。這些仍不是final commit SHA或
  七資料集production證據；commit後必須以新SHA clean build並等待hosted CI全綠。
- [未決 2026-07-23] Restricted HCC performance authority v3的歷史
  `a3e41c...`source snapshot可重現41/41，但對current checkout只匹配40/41：
  `verification_receipt_v2.json`已因history sanitization改為新SHA。Frozen counters與
  既有HTML數值未漂移，但v3不得宣稱current-head fully bound，亦不得作完整C++對Python
  production speedup結論。P8另仍缺seven-dataset final HTML、claim IDs、registered
  report schema與完整accessibility驗證。
- [修復 2026-07-23] 獨立review證明「40位小寫非零」不足以等同exact source
  binding：格式合法的虛構SHA與dirty checkout原可被嵌入。CMake現於Git metadata存在時
  強制explicit SHA等於HEAD且tracked/untracked皆clean；dirty auto-discovery改編入全零
  commit，讓production gate fail closed。`.git`-free build必須另開trusted-builder
  opt-in；hosted pinned job在docker build前後核對`GITHUB_SHA`、commit object與clean
  checkout。因此container內欄位明確是外部CI assertion，而非container自行證明Git
  object存在。實測mismatched SHA與dirty explicit checkout均exit 1，dirty auto configure
  exit 0但compile definition為全零；dependency negative suite亦PASS。
- [加速 2026-07-23] `LONGLINEAGE_GIT_COMMIT` Docker ARG移至apt與HTSlib完成後才
  consume，避免每個source commit使固定依賴層cache失效。此為build-time cache改善，
  不得解讀為science runtime或C++對Python加速證據。
- [修復 2026-07-23] Dirty-tree production commit改為全零後，最初使HCC audit
  synthetic positive test因共用production library而正確拒絕、但破壞precommit suite。
  現以獨立、不安裝的`test_hcc1395_audit_support`編入明示fixture commit；production
  `longlineage-audit`仍只連結fail-closed production library。測試identity與production
  provenance不再混用，且不得把fixture commit寫入正式receipt。
- [驗證 2026-07-23] 最新dirty-tree precommit Release從fresh build完整通過47/47、
  104.05秒、no-network PASS與`FOUNDATION_PASS`；production targets確認編入全零commit，
  HCC test-only support編入fixture commit。Hosted `verify_source_binding`現於docker build
  前後各重播HEAD equality、commit object與clean-tree三項；dependency checker驗證兩次
  呼叫必須依序包住docker build，negative suite會移除equality、cat-file、post-call及把
  ARG移回dependency前，四種mutation均須fail closed。
- [驗證 2026-07-23] 最終實作commit
  `fa1249b9b8873c0b6bc099d8662bd87a62021e8c`（canonical tree SHA-256
  `689842dc2b64d0e929454e0950d4917729e7e3619073d326f5f19059b22c2e2c`）已從fresh
  source完成Release `check_all.sh` 47/47、103.59秒、no-network PASS及
  `FOUNDATION_PASS`；另以GCC ASan+UBSan、
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`與
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`完成47/47、447.43秒，未見leak或UB。
- [驗證 2026-07-23] Hosted push run 29938942025與PR run 29938944740 attempt 2皆
  9/9 PASS；jobs涵蓋introduced-history、browser QA、GCC/Clang Debug/Release、Clang
  ASan/UBSan、GCC TSan及pinned container。PR attempt 1的pinned-container job只有
  Ubuntu `archive.ubuntu.com:80` mirror timeout，原失敗不列completion evidence，重跑後
  成功。PR #4 head與remote branch皆為`fa1249b...e8c`，repository為PRIVATE、PR為
  DRAFT/CLEAN/OPEN，沒有merge、tag或公開。
- [封版 2026-07-23] Private-draft publication scope的immutable audit為
  `state/audits/20260722-release-repair-and-publish-001.json`，實體SHA-256
  `7709d45fab7f1247dc93d7c06c022d17234dbd2c2b87b3cdc0c6038134a5a9ac`。此scope完整封閉
  history-safe draft publication，但不提升任何project phase：P0–P8仍0/9 VERIFIED，
  strict gate仍有12項fixture-only blockers；P3 2,373-key差異、P4七資料集co-occurrence、
  P5 real topology、P6完整validator/query/export/evaluate、P7 w24/w40與P8 final HTML／
  claim IDs／schema／Tagged PDF仍未完成。Restricted performance v3對current checkout仍
  只有40/41 source bindings，因此不存在完整且受控的C++對Python production speedup結論。
- [偏離 2026-07-23] 預設conda Python缺少`jsonschema` module，故該環境的schema命令
  以import error失敗且不作證據；apt-managed `/usr/bin/python3 -m jsonschema`對同一audit
  schema與instance exit 0。Hosted PR首次container嘗試亦因Ubuntu mirror暫時timeout失敗，
  attempt 2完整9/9成功；兩者都保留為環境偏離，不重寫成首次成功。
- [偏離 2026-07-23] Publication task先被移至archive，之後強化remote assertion使audit
  實體SHA改變；root在原lease已`RELEASED`後才同步SHA與timestamp，屬治理時序錯誤，不是
  資料或科學內容錯誤。沒有倒填或重新啟動原lease；後續修改前另建立
  `20260723-private-draft-closure`精確write-set task，重跑schema、audit replay、compiled
  governance與foundation gate後再封存。
