# Implementation Notes

Status: in_progress

## Decisions

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

## Trade-offs

- Jansson is used for local C++ JSON parsing to avoid a vendored header; the pinned
  production container records its exact version.
- A small in-repo CTest harness avoids network-time test dependencies.
- Close後完整重讀BGZF計算physical SHA會增加I/O，但它是freeze/validator的獨立
  evidence，保留；不以writer內部digest取代。

## Open questions

- Direct C++ HiGHS pin/build route.
- Public-release license compatibility.
- Private GitHub remote cannot be verified until the configured GitHub credential is
  valid.
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
- 第一個performance record replay因committed harness usage字串與/tmp原型不同，
  正確拒絕harness SHA。Record更新為repo source的實際SHA後才PASS；原型binary
  digests與raw trials保留，沒有把失敗驗證冒充PASS。

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
