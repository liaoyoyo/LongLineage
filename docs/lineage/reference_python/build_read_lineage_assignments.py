#!/usr/bin/env python3
"""E10 — 產生 read_lineage_assignments：每條 molecule 屬於哪個 block / 拓撲單元。

這是 LongLineage 端與 InterSubMod 端的唯一交界檔。現有鏈條最細只到
site -> component，完全沒有 read 層輸出（見 docs/KNOWN_ISSUES.md E10）。

設計原則
--------
* **不改動已凍結的 exact_ps_partition_to_mlhp.py**（改它會破壞既有 parity 測試）。
  本腳本的路由與投影邏輯與該檔 :317-472 同源，並以 metrics 交叉驗證一致性。
* 輸出 `qname_sha256`，供 ll-bam-tag 串流 BAM 時即時計算 sha256(QNAME) 做 join，
  不需落地明文 read name。
* 誠實表達非唯一性：`is_full_cov` 標示 partial read（含 X），
  `tree_supported` 標示是否通過 min_read 門檻進入建樹。

用法
----
    python3 pipeline/lineage/build_read_lineage_assignments.py \
        --chrom-dir <.../chromosomes/chr1> \
        --sample HCC1395 --chrom chr1 \
        --output <out>/HCC1395.chr1.read_lineage_assignments.tsv.gz

離開碼: 0=OK  2=輸入錯誤/檔案缺失  3=一致性檢查失敗
"""
from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

SCHEMA_NAME = "intersubmod.read_lineage_assignments"
SCHEMA_VERSION = "1.0.0"

OUTPUT_FIELDS = (
    "dataset",
    "chrom",
    "molecule_id",
    "qname_sha256",
    "hp_family",
    "phase_set",
    "unit_id",
    "block_id",
    "block_index",
    "region_id",
    "pattern_vector",
    "k",
    "n_fixed_ra_in_block",
    "is_full_cov",
    "tree_supported",
)

FIXED_CALLS = {"R", "A"}
VALID_CALLS = {"R", "A", "O", "D", "S", "L", "X"}


# ── helpers（與 exact_ps_partition_to_mlhp.py 同源）────────────────────
def read_tsv_gz(path: Path, required: tuple[str, ...]) -> list[dict[str, str]]:
    with gzip.open(path, "rt", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        missing = [c for c in required if c not in (reader.fieldnames or [])]
        if missing:
            raise ValueError(f"{path}: missing columns {missing}")
        return list(reader)


def parse_positions(value: str, *, label: str) -> tuple[int, ...]:
    if not value:
        raise ValueError(f"empty positions1 for {label}")
    out = tuple(int(v) for v in value.split(","))
    if list(out) != sorted(out) or len(set(out)) != len(out):
        raise ValueError(f"positions1 not strictly increasing for {label}")
    return out


def sha256_path(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ── 主邏輯 ────────────────────────────────────────────────────────────
def build(
    chrom_dir: Path,
    *,
    sample: str,
    chrom: str,
    min_read: int,
    partition_subdir: str,
) -> tuple[list[dict[str, object]], Counter, dict]:
    partition_dir = chrom_dir / partition_subdir
    block_path = partition_dir / "blocks.tsv.gz"
    if not block_path.is_file():
        raise FileNotFoundError(f"blocks not found: {block_path}")

    blocks = read_tsv_gz(
        block_path,
        ("dataset", "chrom", "unit_id", "hp_family", "phase_set", "block_id", "block_index", "k", "positions1"),
    )

    metrics: Counter = Counter()
    block_by_key: dict[tuple[str, str], dict[str, object]] = {}
    for row in blocks:
        if row["dataset"] != sample or row["chrom"] != chrom:
            raise ValueError(f"{block_path}: dataset/chrom mismatch")
        if row["hp_family"] not in {"1", "2"} or not row["phase_set"]:
            raise ValueError(f"{block_path}: non-primary HP or missing PS")
        positions = parse_positions(row["positions1"], label=row["block_id"])
        if int(row["k"]) != len(positions):
            raise ValueError(f"{block_path}: k/positions mismatch for {row['block_id']}")
        key = (row["unit_id"], row["block_index"])
        if key in block_by_key:
            raise ValueError(f"{block_path}: duplicate block key {key}")
        block_by_key[key] = {**row, "positions": positions}
        metrics["blocks_total"] += 1

    # route: (hp_family, phase_set, position) -> block key
    route: dict[tuple[str, str, int], tuple[str, str]] = {}
    for key, block in block_by_key.items():
        for position in block["positions"]:
            route_key = (str(block["hp_family"]), str(block["phase_set"]), position)
            if route_key in route:
                raise ValueError(f"{block_path}: route-site belongs to multiple blocks: {route_key}")
            route[route_key] = key

    # molecule calls（比 mlhp adapter 多讀 qname_sha256）
    candidates = sorted((chrom_dir / "extraction").glob("*.molecule_sparse_calls.tsv.gz"))
    if len(candidates) != 1:
        raise ValueError(f"{chrom_dir}: expected exactly one molecule_sparse_calls.tsv.gz, found {len(candidates)}")
    molecule_path = candidates[0]
    molecule_rows = read_tsv_gz(
        molecule_path,
        ("dataset", "chrom", "molecule_id", "qname_sha256", "hp_family", "phase_set", "positions1", "call_codes"),
    )

    raw: list[dict[str, object]] = []
    patterns: dict[tuple[str, str], Counter] = defaultdict(Counter)
    seen: set[str] = set()

    for row in molecule_rows:
        metrics["molecule_rows_total"] += 1
        if row["dataset"] != sample or row["chrom"] != chrom:
            raise ValueError(f"{molecule_path}: dataset/chrom mismatch")
        molecule_id = row["molecule_id"]
        if not molecule_id or molecule_id in seen:
            raise ValueError(f"{molecule_path}: empty or duplicate molecule_id")
        seen.add(molecule_id)
        hp = row["hp_family"]
        phase_set = row["phase_set"]
        if hp not in {"1", "2"} or not phase_set:
            metrics["molecule_rows_nonprimary_or_missing_ps"] += 1
            continue
        observed = parse_positions(row["positions1"], label=molecule_id)
        codes = row["call_codes"]
        if len(observed) != len(codes) or set(codes) - VALID_CALLS:
            raise ValueError(f"{molecule_path}: invalid molecule call vector")

        projected: dict[tuple[str, str], dict[int, str]] = defaultdict(dict)
        for position, code in zip(observed, codes):
            key = route.get((hp, phase_set, position))
            if key is not None:
                projected[key][position] = code if code in FIXED_CALLS else "X"

        for key, calls in projected.items():
            block = block_by_key[key]
            block_positions = block["positions"]
            vector = "".join(calls.get(p, "X") for p in block_positions)
            if set(vector) == {"X"}:
                metrics["projected_molecule_blocks_without_fixed_ra"] += 1
                continue
            patterns[key][vector] += 1
            metrics["projected_molecule_block_incidences"] += 1
            raw.append(
                {
                    "dataset": sample,
                    "chrom": chrom,
                    "molecule_id": molecule_id,
                    "qname_sha256": row["qname_sha256"],
                    "hp_family": hp,
                    "phase_set": phase_set,
                    "unit_id": block["unit_id"],
                    "block_id": block["block_id"],
                    "block_index": block["block_index"],
                    "region_id": f"{chrom}|PS={phase_set}|HP={hp}|{block['block_id']}",
                    "pattern_vector": vector,
                    "k": len(block_positions),
                    "n_fixed_ra_in_block": sum(1 for c in vector if c in FIXED_CALLS),
                    "is_full_cov": "X" not in vector,
                    "_key": key,
                }
            )

    # tree_supported：與 mlhp adapter :481-490 同判準
    #   block 需 k>=2，且該 pattern 的 weight >= min_read
    for entry in raw:
        key = entry["_key"]
        k_ok = entry["k"] >= 2
        weight = patterns[key][entry["pattern_vector"]]
        entry["tree_supported"] = bool(k_ok and weight >= min_read)
        del entry["_key"]
        if entry["tree_supported"]:
            metrics["tree_supported_incidences"] += 1
        if entry["is_full_cov"]:
            metrics["full_cov_incidences"] += 1

    provenance = {
        "schema_name": SCHEMA_NAME,
        "schema_version": SCHEMA_VERSION,
        "sample": sample,
        "chrom": chrom,
        "min_read": min_read,
        "logic_source": "exact_ps_partition_to_mlhp.py:317-472 (same routing/projection)",
        "inputs": {
            "blocks": {"path": str(block_path), "sha256": sha256_path(block_path), "size_bytes": block_path.stat().st_size},
            "molecule_calls": {
                "path": str(molecule_path),
                "sha256": sha256_path(molecule_path),
                "size_bytes": molecule_path.stat().st_size,
            },
        },
        "metrics": dict(metrics),
    }
    return raw, metrics, provenance


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chrom-dir", required=True, type=Path, help="含 extraction/ 與 <partition-subdir>/ 的染色體目錄")
    ap.add_argument("--sample", required=True)
    ap.add_argument("--chrom", required=True)
    ap.add_argument("--min-read", type=int, default=3, help="與 mlhp adapter 一致，預設 3")
    ap.add_argument("--partition-subdir", default="python_partition")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--receipt", type=Path, help="預設為 <output>.receipt.json")
    args = ap.parse_args()

    if not args.chrom_dir.is_dir():
        print(f"NOT A DIRECTORY: {args.chrom_dir}", file=sys.stderr)
        return 2

    try:
        rows, metrics, provenance = build(
            args.chrom_dir,
            sample=args.sample,
            chrom=args.chrom,
            min_read=args.min_read,
            partition_subdir=args.partition_subdir,
        )
    except (ValueError, FileNotFoundError) as exc:
        print(f"BUILD FAILED: {exc}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(args.output, "wt", newline="") as fh:
        # lineterminator 必須顯式指定為 "\n"：csv 模組預設 "\r\n"，
        # 會讓下游以 rstrip("\n") 讀 header 的工具留下 "\r" 而欄名比對失敗。
        writer = csv.DictWriter(
            fh, fieldnames=list(OUTPUT_FIELDS), delimiter="\t", extrasaction="raise", lineterminator="\n"
        )
        writer.writeheader()
        for r in rows:
            writer.writerow({k: (str(r[k]).lower() if isinstance(r[k], bool) else r[k]) for k in OUTPUT_FIELDS})

    provenance["output"] = {
        "path": str(args.output),
        "sha256": sha256_path(args.output),
        "size_bytes": args.output.stat().st_size,
        "rows": len(rows),
    }
    receipt_path = args.receipt or args.output.with_suffix(args.output.suffix + ".receipt.json")
    receipt_path.write_text(json.dumps(provenance, ensure_ascii=False, indent=1, sort_keys=True) + "\n", encoding="utf-8")

    print(f"rows written              : {len(rows)}")
    print(f"molecule_rows_total       : {metrics['molecule_rows_total']}")
    print(f"blocks_total              : {metrics['blocks_total']}")
    print(f"projected_incidences      : {metrics['projected_molecule_block_incidences']}")
    print(f"  full_cov                : {metrics['full_cov_incidences']}")
    print(f"  tree_supported          : {metrics['tree_supported_incidences']}")
    print(f"blocks_without_fixed_ra   : {metrics['projected_molecule_blocks_without_fixed_ra']}")
    print(f"nonprimary_or_missing_ps  : {metrics['molecule_rows_nonprimary_or_missing_ps']}")
    print(f"output                    : {args.output}")
    print(f"receipt                   : {receipt_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
