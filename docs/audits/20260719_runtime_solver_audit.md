<!--
建立時間: 2026-07-19
目標: 稽核 P2 runtime/artifact foundation 與 q<=4 topology exhaustive oracle 的實作與驗證證據
處理範圍: Byte-bounded queue、ordered thread pool、BGZF TSV/semantic SHA、run-state guard、small-q oracle
關聯檔案: LongLineage/include/longlineage/{runtime,artifact,solver}/、LongLineage/src/{artifact,solver}/、LongLineage/tests/unit/test_runtime_solver.cpp
-->

# P2 Runtime 與 Small-q Solver 實作稽核

## 結論與任務邊界

本次為 **B 類 bounded comprehensive component validation**，服務：

- **LL-G3**：q≤4 的 exact、完整 family 或誠實 abstain。
- **LL-G4**：worker-count-independent ordering、可重算 semantic SHA 與 fail-closed lifecycle。

P2 runtime/artifact foundation 已通過 C++17 warnings-as-errors build、3 個正式
CTest entry、ASan/UBSan 與 TSan。P5 僅完成 q≤4 的獨立 exhaustive oracle；
q>4 固定輸出 `ABSTAIN_KERNEL_NOT_VERIFIED / SOLVER_ROUTE_NOT_VERIFIED`，不產生
objective、candidate family 或 winner。這不是 P2/P5 production phase 全面
`VERIFIED`，也不代表 bitset B&B、HiGHS、全量資料或獨立 validator 已完成。

## Step → Verify

1. 建立 byte-bounded cancellable queue
   → 驗證：零 byte、超過 capacity、close、cancel、blocked producer wake-up、
   peak bytes 全有 synthetic regression。
2. 建立單一 thread pool 與 deterministic ordered batch
   → 驗證：1/4 workers 的 logical result vector 完全相同；worker exception
   回傳 `WORKER_ERROR`，公開 result vector 為空。
3. 建立 BGZF TSV writer 與 semantic SHA
   → 驗證：物理 preamble、`#header`、兩筆 row、canonical 28-byte BGZF EOF
   逐 byte 通過；1/2 compression threads 與不同 run_id 的 semantic SHA 相同。
4. 建立 lifecycle transition guard
   → 驗證：只接受 `RUNNING → FAILED` 或
   `RUNNING → VALIDATED → VALIDATED_FROZEN`；validator false、錯誤 digest 與
   `RUNNING → FROZEN` 均被拒。
5. 建立 observed-ALT active-q≤4 exhaustive oracle
   → 驗證：`AX + XA` 得 `h*=2` 與完整 3 families；`AAAA` 得 `h*=3`
   與完整 `4! = 24` families；AR/RA/AA 的唯一 vertex set 有 2 個 parent
   mappings，因此 winner 為 null。
6. 驗證未實作 route fail closed
   → 驗證：q=5 回
   `ABSTAIN_KERNEL_NOT_VERIFIED / SOLVER_ROUTE_NOT_VERIFIED`，objective、
   family、winner 全空。

## 實作契約

### Runtime

- `ByteBoundedQueue<T>` 的 admission 以呼叫者提供的 logical byte charge 計算。
- `close()` 允許既有 task drain；`cancel()` 清除未啟動 task並喚醒所有 waiter。
- `OrderedThreadPool<Result>` 使用單一 pool；task 可 out-of-order 執行，
  `finish()` 只在全數成功時依 submission sequence 發布結果。
- 任一 worker exception 取消尚未啟動工作並禁止 partial batch publication。

### BGZF 與 semantic identity

物理解壓內容固定為：

```text
##longlineage_schema=<schema_name>
##schema_version=<schema_version>
##run_id=<run_id>
#<field_1>\t<field_2>...
<row>...
```

Semantic SHA-256 僅依下列 canonical stream，故不受 run_id、路徑、BGZF block
或 compression thread count 影響：

```text
<schema_name>\t<schema_version>\n
<bare-header>\n
<row>...\n
```

測試向量的 semantic SHA-256 為
`2b996a17c174c0f34ad615c898e3b2e118c0aa437d7c92dd9111a2b4220fbc6c`。
writer 另拒絕 invalid UTF-8、NUL、TAB/CR/LF field、欄數不符與 duplicate header。

### Run lifecycle

`VALIDATED` evidence 必須同時含：

- `all_pass=true`
- producer receipt SHA-256
- validation receipt SHA-256
- validator executable SHA-256

三個 digest 均須為 lowercase 64-hex；freeze 另要求 atomic rename 已完成與
non-empty final root。

### q≤4 topology oracle

- q 定義為 observed-ALT compression 後的 active-bit count。
- root與所有 full R/A states 是 mandatory vertices。
- 每個 partial R/A/X pattern 是一個 joint group-hit constraint。
- 每個 selected non-root vertex至少有一個 selected Hamming-1 predecessor。
- objective 是 selected vertices 相對 mandatory set 的 minimum extra count。
- exhaustive 掃描所有 selected vertex subsets，輸出全部 minimum vertex sets。
- 每個 family 分別輸出 legal parent edge清單、`legal_parent_count`（edge總數）
  與 `tree_count`（各 vertex parent choice數的乘積）。
- 只有 complete family 唯一且 parent mapping 唯一時，內部
  `winner_index` 才可非 null；這不是含 additive edge score 的最終 schema winner。

## 執行與驗證證據

### Input / command / output

輸入 source：

- `LongLineage/include/longlineage/runtime/`
- `LongLineage/include/longlineage/artifact/`
- `LongLineage/include/longlineage/solver/`
- `LongLineage/src/artifact/`
- `LongLineage/src/solver/`
- `LongLineage/tests/unit/test_runtime_solver.cpp`

Configure 命令：

```bash
REPO=<repository-root>
BUILD=<out-of-tree-build-root>
/usr/bin/cmake \
  -S "${REPO}" -B "${BUILD}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLONGLINEAGE_BUILD_TESTS=ON \
  -DLONGLINEAGE_REQUIRE_EXACT_HTSLIB=ON
```

輸出：`${BUILD}/`；exit code `0`。實際 dependency lock：

```text
HTSlib: 1.18 (production pin: 1.18)
Jansson: 2.13.1
OpenSSL: 3.0.2
Configuring done
Generating done
```

Build 命令：

```bash
/usr/bin/cmake --build "${BUILD}" --parallel 4
```

輸出：

- `${BUILD}/lib/liblonglineage_core.a`
- `${BUILD}/bin/test_runtime_solver`

exit code `0`；實際片段：

```text
[100%] Linking CXX executable ../bin/test_runtime_solver
[100%] Built target test_runtime_solver
```

Targeted CTest 命令：

```bash
/usr/bin/ctest \
  --test-dir "${BUILD}" --output-on-failure \
  -R 'runtime_solver|determinism_contract|topology_no_incomplete_winner'
```

exit code `0`；實際結果：

```text
runtime_solver ................... Passed
determinism_contract ............. Passed
topology_no_incomplete_winner .... Passed
100% tests passed, 0 tests failed out of 3
```

### Sanitizers

ASan/UBSan 與 TSan 均以相同 source直接編譯，連結 HTSlib/OpenSSL/Threads：

```bash
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
  -Iinclude $(pkg-config --cflags htslib openssl) \
  src/artifact/bgzf_tsv_writer.cpp src/artifact/run_state.cpp \
  src/solver/small_q_oracle.cpp tests/unit/test_runtime_solver.cpp \
  -o /tmp/longlineage_test_runtime_solver_asan \
  $(pkg-config --libs htslib openssl)
ASAN_OPTIONS=detect_leaks=0 /tmp/longlineage_test_runtime_solver_asan
```

```bash
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=thread -fno-omit-frame-pointer -pthread \
  -Iinclude $(pkg-config --cflags htslib openssl) \
  src/artifact/bgzf_tsv_writer.cpp src/artifact/run_state.cpp \
  src/solver/small_q_oracle.cpp tests/unit/test_runtime_solver.cpp \
  -o /tmp/longlineage_test_runtime_solver_tsan \
  $(pkg-config --libs htslib openssl)
TSAN_OPTIONS=halt_on_error=1 /tmp/longlineage_test_runtime_solver_tsan
```

兩者 exit code 均為 `0`，實際片段：

```text
PASS test_runtime_solver: queue,pool,bgzf,state,small-q oracle
```

LeakSanitizer 在目前受 ptrace 管理環境無法運作，因此 ASan 使用
`detect_leaks=0`；這不等於 leak gate 已通過。

### BGZF 實際輸出

測試輸入：synthetic schema/header/兩筆 rows。
輸出：`/tmp/longlineage_test_runtime_solver.tsv.bgz`。

```text
##longlineage_schema=longlineage.synthetic
##schema_version=1.0.0
##run_id=synthetic-run-a
#key	value
1	alpha
2	beta
```

檔案尾端 28 bytes：

```text
1f 8b 08 04 00 00 00 00 00 ff 06 00 42 43 02 00
1b 00 03 00 00 00 00 00 00 00 00 00
```

## Full CTest 狀態（非本模組失敗）

Full CTest 在當次共享整合狀態為 `11/17 PASS`、exit code `8`。6 個 failure
皆已回報 owner，未在本子任務跨 scope 修補：

- `scripts/testing/expect_exit.sh` 當時無 executable bit：3 個 integration test未啟動。
- `LICENSE` 尚未建立：policy test失敗。
- `checksum.record.json` header/fields長度不符：catalog test失敗。
- production manifest invariant含 truth-aware token：truth-boundary test失敗。

因此本文件只主張 targeted runtime/solver tests與本模組 sanitizer通過，
不宣稱 repo full gate 已通過。

## Changed files

- `LongLineage/include/longlineage/runtime/byte_bounded_queue.hpp`
- `LongLineage/include/longlineage/runtime/ordered_thread_pool.hpp`
- `LongLineage/include/longlineage/artifact/bgzf_tsv_writer.hpp`
- `LongLineage/include/longlineage/artifact/run_state.hpp`
- `LongLineage/include/longlineage/solver/small_q_oracle.hpp`
- `LongLineage/src/artifact/bgzf_tsv_writer.cpp`
- `LongLineage/src/artifact/run_state.cpp`
- `LongLineage/src/solver/small_q_oracle.cpp`
- `LongLineage/tests/unit/test_runtime_solver.cpp`
- `LongLineage/docs/audits/20260719_runtime_solver_audit.md`

所有新增 C++ source/header/test首行均含
`SPDX-License-Identifier: GPL-3.0-only`。

## 已知限制與後續 gate

1. **P5 未完成**：q>4 bitset B&B、small-q DP router與direct HiGHS尚未實作；
   production不得把本 oracle 當全域 solver。
2. **尚無 frozen Python parity panel**：本次只使用手算 synthetic goldens；
   未執行歷史 Python science code，也未宣稱 real-data parity。
3. **batch result retention**：目前 ordered pool將完成結果保留到 `finish()`；
   production大規模使用前仍需加入 byte-bounded streaming reorder sink或明確
   bounded batch partition，避免 output result buffer無上限成長。
4. **writer不是 schema-specific validator**：writer保證 physical envelope、
   UTF-8、欄數與 digest；primary-key uniqueness/order、scalar type、nullability
   與 enum仍須由 producer-specific writer與獨立 validator檢查。
5. **guard只驗 evidence shape與合法 transition**：不會自行開啟或重算 receipt；
   production freeze仍必須由獨立 validator/freezer完成。
6. **final winner尚未實作**：沒有 additive edge score、tie handling與正式
   topology JSON writer；`winner_index`只是 bounded structural oracle內部訊號。
7. **CMake工具版本**：系統 `/usr/local/bin/cmake` 為 3.14.4，不符合 repo
   minimum 3.22；本次使用 JetBrains bundle CMake 3.24.2。
8. **format工具**：環境沒有 `clang-format`；已確認無超過120欄、Werror build
   通過，但正式 format gate仍應在有 pinned clang-format的 CI重跑。
