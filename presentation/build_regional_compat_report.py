#!/usr/bin/env python3
"""Build a presentation-only canonical artifact from frozen C++ receipts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sqlite3
from pathlib import Path
from typing import Any


TITLE = "HCC1395 Python 相容 sSNV 區域拓撲驗證"
PROFILE = "PYTHON_V2_DESCRIPTIVE_REGIONAL"
SOURCE_DATA_AS_OF = "2026-07-21T00:15:00Z"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--validation-receipt", type=Path, required=True)
    parser.add_argument("--frozen-marker", type=Path, required=True)
    parser.add_argument("--crosswalk-receipt", type=Path, required=True)
    parser.add_argument("--performance-receipt", type=Path, required=True)
    parser.add_argument("--generated-at", required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def parse_marker(path: Path) -> dict[str, str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    require(lines and lines[0] == "VALIDATED_FROZEN", "FROZEN state is invalid")
    fields: dict[str, str] = {}
    for line in lines[1:]:
        key, separator, value = line.partition("=")
        require(bool(separator) and bool(key) and bool(value), "malformed FROZEN field")
        fields[key] = value
    return fields


def source(source_id: str, label: str, path: str, digest: str, description: str) -> dict[str, Any]:
    require(re.fullmatch(r"[0-9a-f]{64}", digest) is not None,
            f"source digest is not a lowercase SHA-256: {source_id}")
    return {
        "id": source_id,
        "label": f"{label} · SHA-256 {digest}",
        "path": path,
        "description": description,
    }


def materialized_query_source(dataset_id: str, rows: list[dict[str, Any]], generated_at: str,
                              description: str) -> dict[str, Any]:
    require(bool(rows), f"presentation dataset is empty: {dataset_id}")
    require(dataset_id.replace("_", "").isalnum(), f"unsafe dataset id: {dataset_id}")
    columns = list(rows[0].keys())
    require(all(column.replace("_", "").isalnum() for column in columns),
            f"unsafe presentation column: {dataset_id}")
    require(all(list(row.keys()) == columns for row in rows), f"ragged presentation rows: {dataset_id}")
    table = f"presentation_{dataset_id}"

    def sql_type(value: Any) -> str:
        if isinstance(value, bool) or isinstance(value, int):
            return "INTEGER"
        if isinstance(value, float):
            return "REAL"
        return "TEXT"

    connection = sqlite3.connect(":memory:")
    definitions = ", ".join(f'"{column}" {sql_type(rows[0][column])}' for column in columns)
    connection.execute(f'CREATE TABLE "{table}" ({definitions})')
    placeholders = ", ".join("?" for _ in columns)
    connection.executemany(
        f'INSERT INTO "{table}" VALUES ({placeholders})',
        [[row[column] for column in columns] for row in rows],
    )
    selection = ", ".join(f'"{column}"' for column in columns)
    query = f'SELECT {selection} FROM "{table}" ORDER BY rowid'
    replayed = connection.execute(query).fetchall()
    expected = [tuple(row[column] for column in columns) for row in rows]
    connection.close()
    require(replayed == expected, f"presentation SQL replay mismatch: {dataset_id}")
    return {
        "id": f"query_{dataset_id}",
        "label": f"Presentation SELECT for {dataset_id}",
        "path": "presentation/build_regional_compat_report.py",
        "query": {
            "engine": "sqlite",
            "language": "sql",
            "sql": query,
            "description": description,
            "tables_used": [table],
            "executed_at": generated_at,
            "filters": ["Rows are copied without scientific recomputation from validated frozen receipts."],
        },
    }


def main() -> int:
    args = parse_args()
    summary = load_json(args.summary)
    validation = load_json(args.validation_receipt)
    crosswalk = load_json(args.crosswalk_receipt)
    performance = load_json(args.performance_receipt)
    marker = parse_marker(args.frozen_marker)

    require(summary.get("profile_id") == PROFILE, "summary profile mismatch")
    require(summary.get("state") == "READY_FOR_VALIDATION", "summary state mismatch")
    require(summary.get("row_counts") == {"patterns": 106559, "regions": 8222, "units": 20119},
            "summary row-count golden mismatch")
    require(validation.get("profile_id") == PROFILE, "validation profile mismatch")
    require(validation.get("state") == "VALIDATED_FROZEN", "validation is not frozen")
    require(validation.get("row_counts") == {"patterns": 106559, "regions": 8222, "units": 20119},
            "validation row-count mismatch")
    require(all(row.get("status") == "PASS" for row in validation.get("checks", [])),
            "validation receipt contains non-PASS check")
    require(marker.get("validation_receipt_sha256") == sha256(args.validation_receipt),
            "FROZEN marker does not bind validation receipt")
    require(marker.get("producer_receipt_sha256") == validation.get("producer_receipt_sha256"),
            "FROZEN marker does not bind producer receipt")
    require(crosswalk.get("profile_id") == PROFILE, "crosswalk profile mismatch")
    require(crosswalk.get("cpp_run", {}).get("state") == "VALIDATED_FROZEN", "crosswalk run is not frozen")
    require(all(item.get("mismatches") == 0 for item in crosswalk.get("crosswalks", {}).values()),
            "crosswalk contains a mismatch")
    require(performance.get("full_cpp_correct_w24", {}).get("state") == "VALIDATED_FROZEN",
            "performance receipt does not bind validated run")
    require(performance.get("speedup_vs_historical_python", {}).get("full_controlled_claim_allowed") is False,
            "performance scope ceiling was widened")

    input_roles = {row.get("role") for row in summary.get("inputs", [])}
    required_roles = {
        "raw_bam",
        "raw_bam_index",
        "pass_biallelic_ssnv_vcf",
        "pass_biallelic_ssnv_vcf_index",
        "latest_hp_ps_sidecar",
        "latest_hp_ps_sidecar_index",
        "reference_fasta",
        "reference_fai",
    }
    require(input_roles == required_roles, "input role census is not exact")
    require(not any("truth" in str(role).lower() for role in input_roles), "truth role entered report input")

    census = summary["census"]
    timing = summary["timing"]
    cross = crosswalk["crosswalks"]
    py_seconds = performance["historical_python"]["elapsed_seconds"]
    cpp_seconds = performance["full_cpp_correct_w24"]["science_wall_seconds"]

    sources = [
        source("cpp_summary", "Frozen C++ regional summary",
               "restricted-evidence/hcc1395-regional-compat-v2/summary.json", sha256(args.summary),
               "Validated C++ row counts, censuses, parameters and timing."),
        source("validation_receipt", "Independent validator receipt",
               "restricted-evidence/hcc1395-regional-compat-v2/validation_receipt.json",
               sha256(args.validation_receipt),
               "Independent fail-closed artifact and foreign-key replay."),
        source("crosswalk_receipt", "Python-to-C++ crosswalk over frozen authorities (not independently frozen)",
               "restricted-evidence/hcc1395-regional-compat-v2/crosswalk_receipt.json",
               sha256(args.crosswalk_receipt),
               "Map-based region, unit, pattern and solver parity evidence."),
        source("performance_receipt", "Regional compatibility performance receipt",
               "restricted-evidence/hcc1395-regional-compat-v2/performance_receipt.json",
               sha256(args.performance_receipt),
               "Measured C++ timing, historical Python timing basis and cache caveats."),
    ]

    region_order = [
        ("has_ambiguous", "含多解", census["region_determinacy_has_ambiguous"]),
        ("all_determined", "全為 determined", census["region_determinacy_all_determined"]),
        ("has_capped", "含 capped", census["region_determinacy_has_capped"]),
        ("no_primary_lineage", "無 primary lineage", census["region_determinacy_no_primary_lineage"]),
        ("has_recurrence", "需 recurrence", census["region_determinacy_has_recurrence"]),
    ]
    region_determinacy = [
        {
            "outcome": label,
            "outcome_id": outcome_id,
            "regions": count,
            "share": count / 8222,
            "denominator": 8222,
            "rank": index + 1,
        }
        for index, (outcome_id, label, count) in enumerate(region_order)
    ]

    parity_layers = [
        {"layer": "Region shared map", "expected": 8222, "actual": 8222, "mismatches": 0,
         "sha256": cross["regions"]["shared_map_sha256"]},
        {"layer": "Unit shared fields", "expected": 20119, "actual": 20119, "mismatches": 0,
         "sha256": cross["units"]["shared_map_sha256"]},
        {"layer": "Pattern exact map", "expected": 106559, "actual": 106559, "mismatches": 0,
         "sha256": cross["patterns"]["exact_map_sha256"]},
        {"layer": "Solver unit replay", "expected": 20119, "actual": 20119, "mismatches": 0,
         "sha256": cross["solver_replay"]["unit_semantic_sha256"]},
    ]

    unit_counts: list[dict[str, Any]] = []
    for cohort, prefix, denominator in (("全部 units", "unit_class_", 20119),
                                        ("Primary units", "primary_class_", 10904)):
        for classification in ("determined", "ambiguous_structure", "capped", "recurrence_required"):
            value = census[f"{prefix}{classification}"]
            unit_counts.append({
                "cohort": cohort,
                "class": classification,
                "count": value,
                "share": value / denominator,
                "denominator": denominator,
            })

    parameters = [
        {"setting": key, "value": value, "meaning": meaning}
        for key, value, meaning in (
            ("TIER_R", summary["parameters"]["TIER_R"], "相鄰 sSNV gap ≤ 50 kb 形成 transitive component"),
            ("MAX_SNV", summary["parameters"]["MAX_SNV"], "超過 8 sites 時取最小 span 的連續 8 sites；平手取最左"),
            ("MAPQ_MIN", summary["parameters"]["MAPQ_MIN"], "alignment mapping quality 下限"),
            ("BASEQ_MIN", summary["parameters"]["BASEQ_MIN"], "pileup base quality 下限，與 frozen Python 相同為 0"),
            ("MINREAD", summary["parameters"]["MINREAD"], "full/subread pattern 進 solver 的最少 read 支持"),
            ("EXTRA_NODE_CAP", summary["parameters"]["EXTRA_NODE_CAP"], "每個 unit 額外 hidden node 搜尋上限"),
            ("PER_LEVEL_BUDGET", summary["parameters"]["PER_LEVEL_BUDGET"], "單一 extra-node level 的 combination 上限"),
        )
    ]

    performance_rows = [
        {"observation": "Historical Python", "seconds": py_seconds, "minutes": py_seconds / 60,
         "workers": "sample scheduler", "state": "frozen historical", "cache": "unknown",
         "comparison_scope": "launch-to-HCC-manifest; not isolated timer"},
        {"observation": "C++ w24 near-cold candidate", "seconds": performance["full_cpp_superseded_cold_w24"]["science_wall_seconds"],
         "minutes": performance["full_cpp_superseded_cold_w24"]["science_wall_seconds"] / 60,
         "workers": "24", "state": "superseded; not frozen", "cache": "cold/partially cold",
         "comparison_scope": "engineering baseline only"},
        {"observation": "C++ w24 validated", "seconds": cpp_seconds, "minutes": cpp_seconds / 60,
         "workers": "24", "state": "VALIDATED_FROZEN", "cache": "warm",
         "comparison_scope": "complete HCC1395 descriptive-profile C++ science + producer output"},
        {"observation": "C++ probe w1", "seconds": performance["bounded_parallel_determinism"]["w1_science_wall_seconds"],
         "minutes": performance["bounded_parallel_determinism"]["w1_science_wall_seconds"] / 60,
         "workers": "1", "state": "PARTIAL_PROBE", "cache": "warm",
         "comparison_scope": "same first 100 regions"},
        {"observation": "C++ probe w24", "seconds": performance["bounded_parallel_determinism"]["w24_science_wall_seconds"],
         "minutes": performance["bounded_parallel_determinism"]["w24_science_wall_seconds"] / 60,
         "workers": "24", "state": "PARTIAL_PROBE", "cache": "warm",
         "comparison_scope": "same first 100 regions"},
    ]

    validation_checks = [
        {"check": row["check_id"], "status": row["status"], "detail": row["detail"]}
        for row in validation["checks"]
    ]
    validation_checks.append({
        "check": "FROZEN_BINDING",
        "status": "PASS",
        "detail": "FROZEN marker binds both validation and producer receipt SHA-256 values.",
    })

    outputs = [
        {"file": "summary.json", "rows": 1, "purpose": "input contract, census, parameters and timing"},
        {"file": "regions.tsv", "rows": 8222, "purpose": "region membership, selected positions and determinacy"},
        {"file": "units.tsv", "rows": 20119, "purpose": "region-family role and solver result"},
        {"file": "patterns.tsv", "rows": 106559, "purpose": "full/subread supported pattern evidence"},
        {"file": "producer_receipt.json", "rows": 1, "purpose": "producer semantic and artifact SHA binding"},
        {"file": "validation_receipt.json", "rows": 1, "purpose": "independent validator result"},
        {"file": "FROZEN", "rows": 1, "purpose": "authoritative immutable state marker"},
    ]

    flow = [
        {"stage": "1. Input lock", "decision": "只接受 raw BAM、PASS biallelic sSNV VCF、truth-free HP/PS sidecar、reference",
         "verification": "8 個 input roles 精確匹配；truth roles=0"},
        {"stage": "2. Region plan", "decision": "50 kb transitive grouping；singleton 不進 multi-locus；MAX8 densest window",
         "verification": "79,687 = 8,279 + 44,306 + 27,102"},
        {"stage": "3. Parallel extraction", "decision": "一個 region 一個 task；每 worker 持有獨立 indexed genomic I/O handle",
         "verification": "24 workers + main；ordered publication；100-region w1/w24 byte-identical"},
        {"stage": "4. Read evidence", "decision": "MAPQ≥20、BQ≥0、exact alignment identity sidecar join；R/A/O/X",
         "verification": "1,934,226 exposures = exact joins；conflict=0"},
        {"stage": "5. Family/pattern", "decision": "HP prefix family；full/subread pattern 各自 MINREAD≥3",
         "verification": "106,559 pattern rows exact map mismatch=0"},
        {"stage": "6. Legacy solver", "decision": "min hidden nodes；extra cap=4；level budget=150k；CPython 3.9 capped fallback",
         "verification": "20,119 unit shared fields + solver replay mismatch=0"},
        {"stage": "7. Classification", "decision": "determined / ambiguous_order / ambiguous_structure / recurrence_required / capped",
         "verification": "unit and region aggregate counts exact"},
        {"stage": "8. Fail-closed closeout", "decision": "producer writes six governed files；independent validator replays before FROZEN",
         "verification": "13 receipt checks + atomic output transaction + FROZEN binding PASS"},
    ]

    query_datasets = {
        "region_determinacy": region_determinacy,
        "parity_layers": parity_layers,
        "unit_counts": unit_counts,
        "parameters": parameters,
        "flow": flow,
        "performance": performance_rows,
        "validation_checks": validation_checks,
        "outputs": outputs,
    }
    for dataset_id, rows in query_datasets.items():
        sources.append(materialized_query_source(
            dataset_id,
            rows,
            args.generated_at,
            f"Selects reviewed {dataset_id} presentation rows from frozen receipt-derived values.",
        ))

    charts = [{
        "id": "region_determinacy",
        "title": "Region determinacy 分布",
        "subtitle": "完整 8,222 regions；分類互斥且總和守恆。",
        "type": "bar",
        "dataset": "region_determinacy",
        "sourceId": "query_region_determinacy",
        "encodings": {
            "x": {"field": "outcome", "type": "ordinal", "label": "Region outcome"},
            "y": {"field": "regions", "type": "quantitative", "label": "Regions", "format": "number"},
            "tooltip": [
                {"field": "regions", "type": "quantitative", "label": "Regions", "format": "number"},
                {"field": "share", "type": "quantitative", "label": "Share", "format": "percent"},
                {"field": "denominator", "type": "quantitative", "label": "Denominator", "format": "number"},
            ],
        },
        "yAxisTitle": "Regions",
        "valueFormat": "number",
        "layout": "full",
    }]

    tables = [
        {"id": "parity", "title": "逐層 Python-to-C++ crosswalk",
         "subtitle": "所有比較均以 key map 對齊，不依賴舊 JSON 的顯示順序。", "dataset": "parity_layers",
         "sourceId": "query_parity_layers", "defaultSort": {"field": "expected", "direction": "asc"},
         "columns": [{"field": "layer", "label": "Layer", "type": "text"},
                     {"field": "expected", "label": "Python", "format": "number"},
                     {"field": "actual", "label": "C++", "format": "number"},
                     {"field": "mismatches", "label": "Mismatch", "format": "number"}]},
        {"id": "unit_classes", "title": "Unit class 計數",
         "subtitle": "全部 20,119 units 與 10,904 primary mutation lineages。", "dataset": "unit_counts",
         "sourceId": "query_unit_counts", "defaultSort": {"field": "count", "direction": "desc"},
         "columns": [{"field": "cohort", "label": "Cohort", "type": "text"},
                     {"field": "class", "label": "Class", "type": "text"},
                     {"field": "count", "label": "Count", "format": "number"},
                     {"field": "share", "label": "Share", "format": "percent"},
                     {"field": "denominator", "label": "Denominator", "format": "number"}]},
        {"id": "settings", "title": "Frozen 方法設定",
         "subtitle": "設定值是 compatibility contract，不是 runtime 自動調參。", "dataset": "parameters",
         "sourceId": "query_parameters", "defaultSort": {"field": "setting", "direction": "asc"},
         "columns": [{"field": "setting", "label": "Setting", "type": "text"},
                     {"field": "value", "label": "Value", "format": "number"},
                     {"field": "meaning", "label": "Meaning", "type": "text"}]},
        {"id": "flow", "title": "端到端判斷與驗證流程",
         "subtitle": "每個 stage 都有明確輸入、決策和可觀察驗證。", "dataset": "flow",
         "sourceId": "query_flow", "defaultSort": {"field": "stage", "direction": "asc"},
         "columns": [{"field": "stage", "label": "Stage", "type": "text"},
                     {"field": "decision", "label": "Decision", "type": "text"},
                     {"field": "verification", "label": "Verification", "type": "text"}]},
        {"id": "performance", "title": "執行時間與比較條件",
         "subtitle": "Full 與 probe scope、cache 狀態及 validation state 分列，禁止直接混算。", "dataset": "performance",
         "sourceId": "query_performance", "defaultSort": {"field": "minutes", "direction": "desc"},
         "columns": [{"field": "observation", "label": "Observation", "type": "text"},
                     {"field": "minutes", "label": "Minutes", "format": "number"},
                     {"field": "workers", "label": "Workers", "type": "text"},
                     {"field": "comparison_scope", "label": "Scope", "type": "text"}]},
        {"id": "validation", "title": "Independent validator checks",
         "subtitle": "Receipt 內 13 項檢查加上 FROZEN SHA binding。", "dataset": "validation_checks",
         "sourceId": "query_validation_checks", "defaultSort": {"field": "check", "direction": "asc"},
         "columns": [{"field": "check", "label": "Check", "type": "text"},
                     {"field": "status", "label": "Status", "type": "text"},
                     {"field": "detail", "label": "Detail", "type": "text"}]},
        {"id": "outputs", "title": "Frozen bundle 輸出契約",
         "subtitle": "科學資料由 C++ 產生；Python 只包裝此報告。", "dataset": "outputs",
         "sourceId": "query_outputs", "defaultSort": {"field": "file", "direction": "asc"},
         "columns": [{"field": "file", "label": "File", "type": "text"},
                     {"field": "rows", "label": "Rows", "format": "number"},
                     {"field": "purpose", "label": "Purpose", "type": "text"}]},
    ]

    blocks = [
        {"id": "technical_summary", "type": "markdown", "sourceId": "crosswalk_receipt",
         "body": "## 完整 C++ 模式已重現 8,222 regions，逐鍵 mismatch 為 0\n\n"
                 "**結論：在 HCC1395 描述性共享欄位契約內，receipt 報告逐鍵 0 mismatch。** C++ indexed BAM pipeline 重現 frozen Python 的 50 kb grouping、HP family、MINREAD=3 與 legacy solver；8,222 regions、20,119 units、106,559 patterns 在 map-based crosswalk 全部一致。Crosswalk 本身沒有獨立 freeze/validator，故這項證據不擴張為廣義 production correctness。此端點不取代 formal M2 topology，也不宣稱 clone、祖先或時間方向。"},
        {"id": "key_finding", "type": "markdown", "sourceId": "cpp_summary",
         "body": "## 主要結果不是『每區都有唯一樹』，而是完整且誠實的分類\n\n"
                 "Region determinacy 以 primary mutation-bearing HP1/HP2 為準：5,104 個含 ambiguity、2,019 個全 determined、781 個 capped、28 個 recurrence-required、290 個沒有 primary lineage。圖表從 0 起算並以 8,222 為固定分母；`capped` 是誠實 abstain，不是失敗或任意丟棄。"},
        {"id": "region_chart", "type": "chart", "chartId": "region_determinacy", "layout": "full"},
        {"id": "parity_explanation", "type": "markdown", "sourceId": "crosswalk_receipt",
         "body": "## 一致性驗證到 region、unit、pattern，而不只比總數\n\n"
                 "Region 比較涵蓋 key、座標、selected positions、family census 與 determinacy；unit 比較涵蓋 role、read/pattern census、mutation-bearing、n_hidden、n_trees、capped 與 class；pattern 則比較每個 full/subread pattern 與支持數。最後兩個 CPython 3.9 capped fallback 差異也已納入 golden，不以『不影響主分類』為理由略過。"},
        {"id": "parity_table", "type": "table", "tableId": "parity", "layout": "full"},
        {"id": "unit_table", "type": "table", "tableId": "unit_classes", "layout": "full"},
        {"id": "scope_definitions", "type": "markdown", "sourceId": "cpp_summary",
         "body": "## Scope 與分母先固定，才談數字\n\n"
                 "分析 cohort 是 HCC1395 chr1–22 上 79,687 個 frozen PASS biallelic sSNV。相鄰 gap ≤50 kb 以 transitive closure 分區；8,279 個 positional singleton 不進 multi-locus topology，408 個過密 component 套 MAX8 後排除 44,306 sites，保留 27,102 sites。輸入只含 raw BAM、其 index、PASS VCF、truth-free latest HP/PS sidecar、reference 與索引；沒有 truth role，也沒有 persisted truth-tagged BAM。"},
        {"id": "settings_table", "type": "table", "tableId": "settings", "layout": "full"},
        {"id": "methodology", "type": "markdown", "sourceId": "cpp_summary",
         "body": "## 判斷規則逐步鎖定，避免模糊 fallback\n\n"
                 "每個 region 是一個 parallel task；worker 自有 BAM/Tabix handle，結果依 submission order 發布。Read identity 使用 QNAME、alignment coordinates、FLAG 與 CIGAR digest 做 exact sidecar join。Allele call 保留 R/A/O/X；legacy topology pattern 將 OTHER、missing、deletion 與 refskip 明確映射為 X。HP 以 prefix 1/2、exact 3/4、其餘 none 分 family；full 與 subread pattern 各自需至少 3 reads。Solver 搜尋最少 hidden nodes，extra cap=4、per-level budget=150,000；超限走已重現 CPython 3.9 set-table 行為的 capped fallback。"},
        {"id": "flow_table", "type": "table", "tableId": "flow", "layout": "full"},
        {"id": "performance_finding", "type": "markdown", "sourceId": "performance_receipt",
         "body": "## C++ wall time 觀察值較短，但不可視為受控 speedup\n\n"
                 f"Validated 24-worker C++ science wall 為 **{cpp_seconds:.3f} 秒**；frozen historical Python observation 為 **{py_seconds:.3f} 秒**，表面比值 17.023×。然而 Python 是四樣本 scheduler 下的 launch-to-manifest observation，C++ validated run 又是 warm NFS cache，因此 `full_controlled_speedup_claim_allowed=false`。較接近 cold cache 的 C++ candidate 為 534.462 秒（約 9.517×），但因兩個 capped display fields 尚未修正而未 freeze，只能當工程 baseline。相同前 100 regions 的 workers=1/24 科學 wall 為 14.947/7.333 秒，輸出 byte-identical，證明可以 deterministic parallel 執行。"},
        {"id": "performance_table", "type": "table", "tableId": "performance", "layout": "full"},
        {"id": "bottleneck", "type": "markdown", "sourceId": "performance_receipt",
         "body": "## 主要瓶頸是 NFS BAM I/O 與 region depth 偏斜，不是 solver\n\n"
                 f"Full run 的 summed input time 為 {timing['summed_input_seconds']:.3f} 秒，solver 僅 {timing['summed_solver_seconds']:.3f} 秒；10 秒 telemetry 平均約 309.2% process CPU、111 MiB/s read。24 workers 實際建立 25 threads，但無法接近 24-core 飽和，表示多數時間受遠端 BAM 讀取、解壓與不同 region 深度限制。增加到更多 threads 預期先增加記憶體與 handle 成本，而不是線性加速。"},
        {"id": "validation_finding", "type": "markdown", "sourceId": "validation_receipt",
         "body": "## Fail-closed validator 獨立重播後才允許 FROZEN\n\n"
                 "Validator 不連結 producer solver kernel；它拒絕 missing、extra、symlink、截斷、錯序、row-count mismatch、partial probe 與已測的 snapshot drift/mutation window。這不證明非合作同 UID writer race 已消除。Descriptive compatibility bundle 通過 layout、checksum、schema/profile、三份 TSV、foreign key 與穩定性檢查，然後先發布 validation receipt，再發布綁定 receipt SHA 的 FROZEN marker。"},
        {"id": "validation_table", "type": "table", "tableId": "validation", "layout": "full"},
        {"id": "output_table", "type": "table", "tableId": "outputs", "layout": "full"},
        {"id": "limitations", "type": "markdown",
         "body": "## 限制與不應延伸的結論\n\n"
                 "- 此 profile 是 evaluation/descriptive compatibility；formal M2 topology 的 fail-closed gate 保持不變。\n"
                 "- CN/LOH 是舊 Python 的 post-tree annotation，未納入本 endpoint；不能把 `unavailable` 解讀成 neutral。\n"
                 "- 沒有重跑完整 workers=1 BAM；determinism 證據是完整 w24 crosswalk 加相同 BAM 的 100-region w1/w24 byte comparison。\n"
                 "- Crosswalk 比較兩份 SHA-bound frozen authorities，但 receipt 本身未由獨立 validator freeze；0 mismatch 僅適用於列出的共享欄位契約。\n"
                 "- Historical Python 與 C++ cache/scheduler scope 不同，因此只能列出較短的觀察值，不能稱受控語言 benchmark。\n"
                 "- Current-tree pre-commit hygiene已驗證；post-commit introduced-history scan與GitHub CI仍待實際commit後執行。GPL/source-origin公開稽核亦尚未完成，因此不可改成public visibility或建立release。"},
        {"id": "next_steps", "type": "markdown",
         "body": "## 建議下一步\n\n"
                 "1. 將此 profile 保持為獨立 compatibility CLI，供舊 8,222-region 描述性結果與回歸測試使用。\n"
                 "2. 若要正式比較效能，在專用 cache 條件下各跑一次 isolated Python/C++，固定同一 process scope 並記錄 NFS counters。\n"
                 "3. 將相同 frozen contract 推廣到其他樣本前，先各做 region/unit/pattern map crosswalk，不只驗 aggregate。\n"
                 "4. 完成首個commit後的introduced-history scan，再推送history-safe private draft branch並等待GCC/Clang/sanitizer/container CI全綠；公開前另做GPL/source-origin稽核並取得明確授權。"},
        {"id": "further_questions", "type": "markdown",
         "body": "## 仍待回答的問題\n\n"
                 "- 其他樣本是否也有 CPython capped fallback 對 set-table order 敏感的 edge cases？\n"
                 "- 在 BAM 本機 SSD cache 或 CRAM/region batching 下，I/O wall 能再下降多少？\n"
                 "- 是否需要獨立 query/export CLI 讀取此 compatibility frozen bundle，而不污染 production catalog？"},
    ]

    artifact = {
        "surface": "report",
        "manifest": {
            "version": 1,
            "surface": "report",
            "title": TITLE,
            "description": "HCC1395 descriptive compatibility report; project-partial, non-production and not P8 release evidence.",
            "generatedAt": args.generated_at,
            "cards": [],
            "charts": charts,
            "tables": tables,
            "sources": sources,
            "blocks": blocks,
        },
        "snapshot": {
            "version": 1,
            "generatedAt": args.generated_at,
            "status": "ready",
            "datasets": {
                "headline": [{"regions": 8222, "units": 20119, "patterns": 106559, "mismatches": 0}],
                "region_determinacy": region_determinacy,
                "parity_layers": parity_layers,
                "unit_counts": unit_counts,
                "parameters": parameters,
                "performance": performance_rows,
                "validation_checks": validation_checks,
                "outputs": outputs,
                "flow": flow,
                "report_notes": [{
                    "audience": "technical",
                    "delivery_mode": "html",
                    "scope": "PARTIAL_HCC1395_ONLY",
                    "claim_ceiling": "DESCRIPTIVE_NON_PRODUCTION_NOT_P8",
                    "source_data_as_of": SOURCE_DATA_AS_OF,
                    "report_generated_at": args.generated_at,
                    "chart_map": "Region determinacy / comparison bar / outcome, regions, share / single-root palette.",
                    "omitted_visuals": "Performance uses a table because only three full-scope observations exist and cache conditions differ.",
                    "python_boundary": "Python reads validated chart-ready JSON only; no BAM/VCF/sidecar science is executed.",
                }],
            },
        },
        "sources": sources,
        "package_info": {
            "originUrl": "artifact://longlineage/hcc1395-python-compatible-regional-topology",
            "controls": {"edit": False, "refresh": False, "share": False},
        },
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(artifact, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"PASS REGIONAL_COMPAT_REPORT_ARTIFACT output={args.output}")
    print("regions=8222 units=20119 patterns=106559 mismatches=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
