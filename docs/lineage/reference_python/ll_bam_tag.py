#!/usr/bin/env python3
"""ll-bam-tag — 把 HP/PS 與 lineage 標籤一次寫進 BAM（方案 C）。

背景
----
lineage 分析所用的 HP 來自 2026-07 sidecar（九態 somatic HP），
而唯一落地的 tagged BAM 是 2026-03 版、HP 值不同（見 KNOWN_ISSUES D8）。
方案 C：讀 **raw BAM**（有 MM/ML、無 HP/PS），一次注入 sidecar 的 HP/PS
與 assignments 的 lineage 標籤，使三者同源。

寫入的 aux tag（SAM 規格：含小寫字母的 tag 保留給 local use）
--------------------------------------------------------------
  HP:Z  九態 HP（1/2/1-1/2-1/3）        ← sidecar
  PS:i  phase set                        ← sidecar
  lc:Z  unit_id（lineage component）      ← assignments
  lu:Z  block_id                          ← assignments
  lv:Z  pattern_vector（該 read 的觀察 R/A/X 向量）
  ls:A  U=唯一 / M=tie / P=partial / A=abstain

不變式（防 overclaim）
  * lv 存在 ⟹ ls 必存在
  * ls != 'U' 時 lv 僅為觀察值，不得被下游當唯一拓撲結論
  * 一條 read 落在多個 block 時，lc/lu/lv 以逗號串接，ls 取最保守者

用法
----
    python3 pipeline/lineage/ll_bam_tag.py \
        --in-bam <raw.bam> --sidecar <read_tags.tsv.gz> \
        --assignments <read_lineage_assignments.tsv.gz> \
        --topology <topology.jsonl> \
        --region chr1:1-2000000 \
        --out-bam <out.bam>

離開碼: 0=OK  2=輸入錯誤  3=一致性失敗
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path

try:
    import pysam
except ImportError:
    print("需要 pysam：pip install pysam", file=sys.stderr)
    raise SystemExit(2)

# ls 保守度排序：越前面越保守
LS_SEVERITY = {"A": 0, "P": 1, "M": 2, "U": 3}


def sha256_hex(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def load_assignments(path: Path, chrom: str | None) -> dict[str, list[dict[str, str]]]:
    """qname_sha256 -> [assignment, ...]"""
    out: dict[str, list[dict[str, str]]] = defaultdict(list)
    with gzip.open(path, "rt", newline="") as fh:
        hdr = fh.readline().rstrip("\r\n").split("\t")
        need = ("chrom", "qname_sha256", "unit_id", "block_id", "region_id", "pattern_vector", "is_full_cov", "tree_supported")
        missing = [c for c in need if c not in hdr]
        if missing:
            raise ValueError(f"{path}: missing columns {missing}")
        ix = {k: hdr.index(k) for k in need}
        for line in fh:
            f = line.rstrip("\r\n").split("\t")
            if chrom and f[ix["chrom"]] != chrom:
                continue
            out[f[ix["qname_sha256"]]].append(
                {
                    "unit_id": f[ix["unit_id"]],
                    "block_id": f[ix["block_id"]],
                    "region_id": f[ix["region_id"]],
                    "pattern_vector": f[ix["pattern_vector"]],
                    "is_full_cov": f[ix["is_full_cov"]] == "true",
                    "tree_supported": f[ix["tree_supported"]] == "true",
                }
            )
    return out


def load_topology(path: Path | None, chrom: str | None) -> dict[str, dict[str, object]]:
    """region_id -> {best_tree_unique, family_status}"""
    if path is None:
        return {}
    out: dict[str, dict[str, object]] = {}
    with path.open() as fh:
        for line in fh:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if chrom and d.get("chrom") != chrom:
                continue
            rid = d.get("region_id")
            if rid:
                out[rid] = {
                    "best_tree_unique": bool(d.get("best_tree_unique")),
                    "family_status": d.get("family_status", ""),
                }
    return out


def load_sidecar(path: Path, chrom: str, start: int | None, end: int | None) -> dict[str, tuple[str, str]]:
    """(qname) -> (HP, PS)；用 tabix 只取所需區間"""
    out: dict[str, tuple[str, str]] = {}
    tbx = pysam.TabixFile(str(path))
    region = chrom if start is None else f"{chrom}:{max(start, 1)}-{end}"
    try:
        rows = tbx.fetch(region=region)
    except ValueError:
        return out
    for line in rows:
        f = line.rstrip("\n").split("\t")
        if len(f) < 9:
            continue
        # #CHROM START0 END0 QNAME FLAG MAPQ CIGAR_B2 HP PS
        out[f[3]] = (f[7], f[8])
    tbx.close()
    return out


def decide_ls(entry: dict[str, object], topo: dict[str, dict[str, object]]) -> str:
    if not entry["is_full_cov"]:
        return "P"
    t = topo.get(entry["region_id"])
    if t is None:
        # 沒有 topology 資訊：只要通過 tree_supported 就給 M（保守），否則 A
        return "M" if entry["tree_supported"] else "A"
    if t["family_status"] != "FAMILY_COMPLETE":
        return "A"
    return "U" if t["best_tree_unique"] else "M"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in-bam", required=True, type=Path)
    ap.add_argument("--sidecar", required=True, type=Path, help="HP/PS sidecar（BGZF+Tabix）")
    ap.add_argument("--assignments", required=True, type=Path)
    ap.add_argument("--topology", type=Path, help="topology.jsonl，用於 ls 的 U/M/A 判定")
    ap.add_argument("--region", required=True, help="chr 或 chr:start-end。磁碟保護：不接受全基因組")
    ap.add_argument("--out-bam", required=True, type=Path)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--require-all-reads", action="store_true", help="任何 read 查無標籤即失敗")
    args = ap.parse_args()

    for p in (args.in_bam, args.sidecar, args.assignments):
        if not p.is_file():
            print(f"NOT FOUND: {p}", file=sys.stderr)
            return 2

    if ":" in args.region:
        chrom, span = args.region.split(":", 1)
        s, e = span.split("-", 1)
        start, end = int(s), int(e)
    else:
        chrom, start, end = args.region, None, None

    print(f"region      : {args.region}", file=sys.stderr)
    assign = load_assignments(args.assignments, chrom)
    print(f"assignments : {len(assign)} distinct qname_sha256", file=sys.stderr)
    topo = load_topology(args.topology, chrom)
    print(f"topology    : {len(topo)} region_id", file=sys.stderr)
    side = load_sidecar(args.sidecar, chrom, start, end)
    print(f"sidecar     : {len(side)} qname", file=sys.stderr)

    stats = defaultdict(int)
    args.out_bam.parent.mkdir(parents=True, exist_ok=True)

    with pysam.AlignmentFile(str(args.in_bam), "rb", threads=args.threads) as fin:
        with pysam.AlignmentFile(str(args.out_bam), "wb", template=fin, threads=args.threads) as fout:
            for aln in fin.fetch(chrom, start - 1 if start else None, end if end else None):
                stats["reads_total"] += 1
                qname = aln.query_name
                # ① sidecar 注入 HP/PS
                hp_ps = side.get(qname)
                if hp_ps:
                    hp, ps = hp_ps
                    if hp and hp != ".":
                        aln.set_tag("HP", hp, value_type="Z")
                        stats["hp_written"] += 1
                    if ps and ps != ".":
                        try:
                            aln.set_tag("PS", int(ps), value_type="i")
                            stats["ps_written"] += 1
                        except ValueError:
                            stats["ps_malformed"] += 1
                else:
                    stats["no_sidecar_row"] += 1

                # ② lineage 標籤
                entries = assign.get(sha256_hex(qname))
                if entries:
                    lc = ",".join(dict.fromkeys(e["unit_id"] for e in entries))
                    lu = ",".join(dict.fromkeys(e["block_id"] for e in entries))
                    lv = ",".join(e["pattern_vector"] for e in entries)
                    ls = min((decide_ls(e, topo) for e in entries), key=lambda c: LS_SEVERITY[c])
                    aln.set_tag("lc", lc, value_type="Z")
                    aln.set_tag("lu", lu, value_type="Z")
                    aln.set_tag("lv", lv, value_type="Z")
                    aln.set_tag("ls", ls, value_type="A")
                    stats["lineage_written"] += 1
                    stats[f"ls_{ls}"] += 1
                    if len(entries) > 1:
                        stats["multi_block_reads"] += 1
                else:
                    stats["no_lineage"] += 1
                    if args.require_all_reads:
                        print(f"FAIL --require-all-reads: {qname} 查無標籤", file=sys.stderr)
                        return 3

                fout.write(aln)

    print()
    for k in sorted(stats):
        print(f"{k:24s} {stats[k]}")
    print(f"\nout-bam: {args.out_bam} ({args.out_bam.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
