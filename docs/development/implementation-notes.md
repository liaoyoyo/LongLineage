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

## Trade-offs

- Jansson is used for local C++ JSON parsing to avoid a vendored header; the pinned
  production container records its exact version.
- A small in-repo CTest harness avoids network-time test dependencies.

## Open questions

- Direct C++ HiGHS pin/build route.
- Public-release license compatibility.
- Private GitHub remote cannot be verified until the configured GitHub credential is
  valid.
- Audit envelope的source-tree Git replay已定義；recorded command output digest
  仍需由明示的獨立replay命令重跑比對，不可把digest欄位存在當作執行證據。
- P3 requires frozen PCG64/RNG/logical-digest vectors and a versioned HP-family
  mapping; P4 also requires frozen Endpoint-B O/X callability precedence vectors.

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
