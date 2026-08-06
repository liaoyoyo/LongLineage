# 參考實作（已被 C++ 取代，不在執行路徑上）

這三支 Python 是 C++ 版的**原型與 parity 對照**，已完成任務：

| Python 原型 | C++ 取代者 | parity 驗證結果 |
|---|---|---|
| `build_lineage_paths.py` | `cpp/lineage_paths.cpp` | **byte-level 相同（含行順序）** |
| `build_read_lineage_assignments.py` | `cpp/read_assign.cpp` | 8 項統計相同；內容排序後 100% 相同 |
| `ll_bam_tag.py` | `cpp/tag_bam.cpp` | 統計逐項相同（25,700/9,694/9,585/355/29/326） |

**已知且刻意的差異**：`read_assign` 對「一條 read 落多個 block」的輸出順序，
Python 走 dict 插入順序、C++ 走 block index 升冪。C++ 的規則不依賴查找順序，更確定。

保留原因：日後修改 C++ 時可重跑 parity 比對。**不應被任何 driver 呼叫。**
