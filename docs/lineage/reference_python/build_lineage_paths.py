#!/usr/bin/env python3
"""S1 — 從 topology.jsonl 的 representative_best_edges 算出每個 vertex 的階層路徑。

輸出 HP1-1-1 這類標籤，讓 read 的演化位置能直接寫進 BAM tag。

規則（見 docs/HIERARCHICAL_TAG_SPEC.md）
---------------------------------------
lineage_path = HP{hp_family}                      若 vertex 是 ROOT
             = HP{hp_family}-{d1}-...-{dk}        否則，d_i = 該層兄弟中的序號(1-based)

同層兄弟排序：先 acquired_position 升冪，再 child_vertex 升冪（保證可重現）。
hidden node（H_ 前綴）**納入** depth 計數 —— 它是真實的演化中間態。

用法
----
    python3 pipeline/lineage/build_lineage_paths.py \
        --topology <topology.jsonl> --chrom chr1 \
        --output <unit_lineage_paths.tsv.gz>

離開碼: 0=OK  2=輸入錯誤  3=樹結構不合法
"""
from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path

SCHEMA_NAME = "intersubmod.unit_lineage_paths"
SCHEMA_VERSION = "1.0.0"

FIELDS = (
    "region_id",
    "chrom",
    "hp_family",
    "phase_set",
    "unit_id",
    "block_id",
    "vertex",
    "vertex_label",
    "is_hidden",
    "depth",
    "lineage_path",
    "parent_vertex",
    "acquired_position",
    "mutation_order",
    "edge_score_fraction",
    "n_children",
    "best_tree_unique",
    "family_status",
)


def build_paths_for_unit(rec: dict) -> list[dict[str, object]] | None:
    """回傳該 unit 所有 vertex 的階層路徑；樹不合法回 None。"""
    edges = rec.get("representative_best_edges") or []
    vertices = rec.get("representative_best_vertices") or []
    if not vertices:
        return None

    label_by_vertex = {v["vertex"]: v["label"] for v in vertices}
    children: dict[int, list[dict]] = defaultdict(list)
    parent_of: dict[int, int] = {}
    edge_of: dict[int, dict] = {}

    for e in edges:
        cv, pv = e["child_vertex"], e["parent_vertex"]
        if cv in parent_of:  # 每個 vertex 只能有一個 parent（樹性質）
            return None
        parent_of[cv] = pv
        edge_of[cv] = e
        children[pv].append(e)

    roots = [v["vertex"] for v in vertices if v["vertex"] not in parent_of]
    if len(roots) != 1:
        return None
    root = roots[0]

    # 同層兄弟排序：acquired_position 升冪，再 child_vertex 升冪
    for pv in children:
        children[pv].sort(key=lambda e: (e.get("acquired_position", 0), e["child_vertex"]))

    hp = str(rec.get("hp_family", ""))
    out: list[dict[str, object]] = []

    def walk(vertex: int, path: list[int], mut: list[int]) -> None:
        label = label_by_vertex.get(vertex, "")
        e = edge_of.get(vertex)
        out.append(
            {
                "region_id": rec.get("region_id", ""),
                "chrom": rec.get("chrom", ""),
                "hp_family": hp,
                "phase_set": rec.get("phase_set", ""),
                "unit_id": rec.get("unit_id", ""),
                "block_id": rec.get("block_id", ""),
                "vertex": vertex,
                "vertex_label": label,
                "is_hidden": label.startswith("H_"),
                "depth": len(path),
                "lineage_path": f"HP{hp}" + ("".join(f"-{d}" for d in path) if path else ""),
                "parent_vertex": parent_of.get(vertex, "."),
                "acquired_position": e["acquired_position"] if e else ".",
                "mutation_order": ">".join(str(p) for p in mut) if mut else ".",
                "edge_score_fraction": e.get("edge_score_fraction", ".") if e else ".",
                "n_children": len(children.get(vertex, [])),
                "best_tree_unique": bool(rec.get("best_tree_unique")),
                "family_status": rec.get("family_status", ""),
            }
        )
        for idx, ce in enumerate(children.get(vertex, []), start=1):
            walk(ce["child_vertex"], path + [idx], mut + [ce["acquired_position"]])

    walk(root, [], [])
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--topology", required=True, type=Path)
    ap.add_argument("--chrom", help="只處理此染色體；省略則全部")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--unique-only", action="store_true", help="只輸出 best_tree_unique 的 unit")
    args = ap.parse_args()

    if not args.topology.is_file():
        print(f"NOT FOUND: {args.topology}", file=sys.stderr)
        return 2

    rows: list[dict[str, object]] = []
    stats: dict[str, int] = defaultdict(int)

    with args.topology.open() as fh:
        for line in fh:
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                stats["malformed_lines"] += 1
                continue
            if args.chrom and rec.get("chrom") != args.chrom:
                continue
            stats["units_seen"] += 1
            if args.unique_only and not rec.get("best_tree_unique"):
                stats["skipped_not_unique"] += 1
                continue
            if not rec.get("representative_best_vertices"):
                stats["skipped_no_vertices"] += 1
                continue
            built = build_paths_for_unit(rec)
            if built is None:
                stats["invalid_tree"] += 1
                continue
            stats["units_with_paths"] += 1
            for r in built:
                stats["vertices_total"] += 1
                if r["is_hidden"]:
                    stats["vertices_hidden"] += 1
                stats[f"depth_{min(int(r['depth']), 9)}"] += 1
            rows.extend(built)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(args.output, "wt", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(FIELDS), delimiter="\t", extrasaction="raise", lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow({k: (str(r[k]).lower() if isinstance(r[k], bool) else r[k]) for k in FIELDS})

    h = hashlib.sha256()
    with args.output.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    receipt = {
        "schema_name": SCHEMA_NAME,
        "schema_version": SCHEMA_VERSION,
        "chrom": args.chrom or "ALL",
        "unique_only": args.unique_only,
        "input": {"path": str(args.topology), "size_bytes": args.topology.stat().st_size},
        "output": {"path": str(args.output), "sha256": h.hexdigest(), "rows": len(rows)},
        "stats": dict(stats),
    }
    rp = args.output.with_suffix(args.output.suffix + ".receipt.json")
    rp.write_text(json.dumps(receipt, ensure_ascii=False, indent=1, sort_keys=True) + "\n", encoding="utf-8")

    print(f"units_seen        : {stats['units_seen']}")
    print(f"units_with_paths  : {stats['units_with_paths']}")
    print(f"vertices_total    : {stats['vertices_total']}")
    print(f"  hidden (H_)     : {stats['vertices_hidden']}")
    print(f"invalid_tree      : {stats['invalid_tree']}")
    print("depth 分佈        : " + ", ".join(f"{d}={stats[f'depth_{d}']}" for d in range(6) if stats.get(f"depth_{d}")))
    print(f"rows              : {len(rows)}")
    print(f"output            : {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
