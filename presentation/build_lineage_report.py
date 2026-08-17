#!/usr/bin/env python3
"""S6 — 依「實際存在多少數據」產出 standalone HTML 觀察報告。

降級契約：任何輸入缺件 → 該面板標記不可用並說明原因，其餘照常產出，絕不失敗。
數字誠信：只呈現 artifact 內已有的值，不自行計算科學結論。

用法:
    python3 pipeline/lineage/build_report.py --out-root <run_root> --sample HCC1395 \
        [--ism-summary <significance_summary.csv>] [--output report.html]
"""
from __future__ import annotations

import argparse
import csv
import gzip
import html
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path


def read_tsv_gz(path: Path, limit: int | None = None):
    with gzip.open(path, "rt", newline="") as fh:
        for i, row in enumerate(csv.DictReader(fh, delimiter="\t")):
            if limit is not None and i >= limit:
                break
            yield row


def panel(pid: str, title: str, available: bool, reason: str = "", body: str = "") -> str:
    if available:
        return f'<section class="p"><h2>{html.escape(title)}</h2>{body}</section>'
    return (
        f'<section class="p na"><h2>{html.escape(title)}</h2>'
        f'<p class="reason">此面板不可用 — {html.escape(reason)}</p></section>'
    )


def bar_table(counter: Counter, headers: tuple[str, str], top: int = 20) -> str:
    if not counter:
        return "<p>（無資料）</p>"
    mx = max(counter.values())
    rows = []
    for k, v in counter.most_common(top):
        w = int(v / mx * 100)
        rows.append(
            f"<tr><td><code>{html.escape(str(k))}</code></td><td class='n'>{v:,}</td>"
            f"<td class='b'><span style='width:{w}%'></span></td></tr>"
        )
    return (
        f"<table><tr><th>{headers[0]}</th><th>{headers[1]}</th><th></th></tr>" + "".join(rows) + "</table>"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-root", required=True, type=Path)
    ap.add_argument("--sample", required=True)
    ap.add_argument("--ism-summary", type=Path)
    ap.add_argument("--output", type=Path)
    ap.add_argument("--max-rows", type=int, default=2_000_000)
    args = ap.parse_args()

    root = args.out_root
    out = args.output or (root / f"{args.sample}.lineage_report.html")
    avail: dict[str, dict] = {}
    panels: list[str] = []

    # ── 1. lineage paths ──────────────────────────────────────────
    ppath = root / "paths" / f"{args.sample}.unit_lineage_paths.tsv.gz"
    path_counter, depth_counter, hidden = Counter(), Counter(), 0
    mut_examples: list[tuple[str, str, str]] = []
    n_paths = 0
    if ppath.is_file():
        for r in read_tsv_gz(ppath):
            n_paths += 1
            path_counter[r["lineage_path"]] += 1
            depth_counter[int(r["depth"])] += 1
            if r["is_hidden"] == "true":
                hidden += 1
            if len(mut_examples) < 12 and r["mutation_order"] != "." and r["best_tree_unique"] == "true":
                mut_examples.append((r["region_id"], r["lineage_path"], r["mutation_order"]))
        avail["lineage_paths"] = {"available": True, "path": str(ppath), "rows": n_paths}
        body = (
            f"<p>共 <b>{n_paths:,}</b> 個 vertex，其中 hidden node（推斷的中間演化態）"
            f"<b>{hidden:,}</b>（{hidden/max(n_paths,1)*100:.1f}%）。</p>"
            "<h3>階層標籤分佈</h3>" + bar_table(path_counter, ("lineage_path", "vertex 數"))
            + "<h3>樹深度分佈</h3>"
            + bar_table(Counter({f"depth {k}": v for k, v in depth_counter.items()}), ("深度", "vertex 數"), 12)
            + "<h3>突變順序範例（唯一解拓撲）</h3><table><tr><th>region_id</th><th>階層</th><th>突變順序</th></tr>"
            + "".join(
                f"<tr><td><code>{html.escape(a)}</code></td><td><code>{html.escape(b)}</code></td>"
                f"<td><code>{html.escape(c)}</code></td></tr>"
                for a, b, c in mut_examples
            )
            + "</table>"
        )
        panels.append(panel("lineage_paths", "1 · 演化階層與突變順序", True, body=body))
    else:
        avail["lineage_paths"] = {"available": False, "reason": f"缺少 {ppath}"}
        panels.append(panel("lineage_paths", "1 · 演化階層與突變順序", False, f"缺少 {ppath}"))

    # ── 2. read 指派 ──────────────────────────────────────────────
    apath = root / "assign" / f"{args.sample}.all.read_lineage_assignments.tsv.gz"
    n_assign = 0
    full_cov = tree_sup = 0
    per_chrom = Counter()
    kdist = Counter()
    if apath.is_file():
        for r in read_tsv_gz(apath, args.max_rows):
            n_assign += 1
            per_chrom[r["chrom"]] += 1
            kdist[r["k"]] += 1
            if r["is_full_cov"] == "true":
                full_cov += 1
            if r["tree_supported"] == "true":
                tree_sup += 1
        avail["assignments"] = {"available": True, "path": str(apath), "rows": n_assign}
        body = (
            f"<div class='kpi'><div><b>{n_assign:,}</b><span>read×block 指派</span></div>"
            f"<div><b>{full_cov:,}</b><span>完整覆蓋 ({full_cov/max(n_assign,1)*100:.1f}%)</span></div>"
            f"<div><b>{tree_sup:,}</b><span>進入建樹 ({tree_sup/max(n_assign,1)*100:.1f}%)</span></div></div>"
            "<h3>各染色體</h3>" + bar_table(per_chrom, ("染色體", "指派數"), 25)
            + "<h3>block 位點數 (k)</h3>" + bar_table(kdist, ("k", "指派數"), 13)
        )
        panels.append(panel("assignments", "2 · read → lineage 指派", True, body=body))
    else:
        avail["assignments"] = {"available": False, "reason": f"缺少 {apath}"}
        panels.append(panel("assignments", "2 · read → lineage 指派", False, f"缺少 {apath}"))

    # ── 3. tagged BAM ─────────────────────────────────────────────
    bam_dir = root / "bam"
    receipts = sorted(bam_dir.glob(f"{args.sample}.*.tag_bam.receipt.json")) if bam_dir.is_dir() else []
    if receipts:
        agg = Counter()
        total_bytes = 0
        for rp in receipts:
            try:
                d = json.loads(rp.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                continue
            for k, v in (d.get("stats") or {}).items():
                agg[k] += int(v)
            b = bam_dir / (rp.name.replace(".tag_bam.receipt.json", ".lineage_tagged.bam"))
            if b.is_file():
                total_bytes += b.stat().st_size
        ls_tot = sum(agg[k] for k in agg if k.startswith("ls_"))
        body = (
            f"<div class='kpi'><div><b>{agg['reads_total']:,}</b><span>alignment 總數</span></div>"
            f"<div><b>{agg['hp_written']:,}</b><span>寫入 HP</span></div>"
            f"<div><b>{agg['lineage_written']:,}</b><span>寫入 lineage</span></div>"
            f"<div><b>{agg['lv_written']:,}</b><span>寫入階層標籤</span></div>"
            f"<div><b>{total_bytes/2**30:.1f} GB</b><span>{len(receipts)} 個 BAM</span></div></div>"
            "<h3>標籤可信度 ls 分佈</h3>"
            + bar_table(Counter({k[3:]: agg[k] for k in agg if k.startswith("ls_")}), ("ls", "read 數"), 6)
            + "<p class='note'>U=唯一解｜M=拓撲並列(tie)｜P=部分覆蓋｜A=拓撲未定。"
            f"非 U 者佔 <b>{(ls_tot-agg.get('ls_U',0))/max(ls_tot,1)*100:.1f}%</b>，"
            "其 <code>lv</code> 僅為代表值，不得視為唯一結論。</p>"
        )
        avail["tagged_bam"] = {"available": True, "receipts": len(receipts)}
        panels.append(panel("tagged_bam", "3 · 帶標籤的 BAM（IGV 可讀）", True, body=body))
    else:
        avail["tagged_bam"] = {"available": False, "reason": f"{bam_dir} 下無 tag_bam receipt"}
        panels.append(panel("tagged_bam", "3 · 帶標籤的 BAM（IGV 可讀）", False, f"{bam_dir} 下無 tag_bam receipt"))

    # ── 4. ISM 甲基 × 標籤 ────────────────────────────────────────
    if args.ism_summary and args.ism_summary.is_file():
        with args.ism_summary.open(newline="") as fh:
            rows = list(csv.DictReader(fh))
        avail["ism"] = {"available": True, "path": str(args.ism_summary), "rows": len(rows)}
        cols = list(rows[0].keys()) if rows else []
        has_lineage = "LineageNGroups" in cols

        def sig(v, thr=0.05):
            try:
                return float(v) <= thr
            except (TypeError, ValueError):
                return False

        # 每個軸各自統計：可檢定的 region 數、其中顯著的數目
        axes = [
            ("HP", "HPMergedP", "HPMergedDelta"),
            ("ALT", "AlleleP", "AlleleDelta"),
            ("Cluster", "ClusterPermanovaP", "ClusterPermanovaF"),
        ]
        if has_lineage:
            axes.append(("lineage", "LineagePermanovaP", "LineagePermanovaF"))

        axis_rows = []
        for name, pcol, scol in axes:
            if pcol not in cols:
                continue
            testable = [r for r in rows if (r.get(pcol) or "").strip() not in ("", "nan")]
            hits = [r for r in testable if sig(r.get(pcol))]
            axis_rows.append(
                f"<tr><td><code>{html.escape(name)}</code></td>"
                f"<td class='n'>{len(testable):,}</td>"
                f"<td class='n'>{len(hits):,}</td>"
                f"<td class='n'>{len(hits)/len(testable)*100:.1f}%</td></tr>"
                if testable
                else f"<tr><td><code>{html.escape(name)}</code></td><td class='n'>0</td>"
                     f"<td class='n'>0</td><td class='n'>—</td></tr>"
            )

        # 逐位點：哪個軸解釋了這個位點的甲基差異
        detail = []
        if has_lineage:
            for r in rows:
                if (r.get("LineageNGroups") or "0") in ("0", ""):
                    continue
                winners = [n for n, pc, _ in axes if sig(r.get(pc))]
                detail.append(
                    "<tr>"
                    f"<td><code>{html.escape(r.get('Chr',''))}:{html.escape(r.get('Pos',''))}"
                    f" {html.escape(r.get('Ref',''))}&gt;{html.escape(r.get('Alt',''))}</code></td>"
                    f"<td><code>{html.escape(r.get('LineageAxis',''))}</code></td>"
                    f"<td class='n'>{html.escape(r.get('LineageNGroups',''))}</td>"
                    f"<td class='n'>{html.escape(r.get('LineageNReads',''))}</td>"
                    f"<td class='n'>{html.escape(r.get('LineagePermanovaF',''))}</td>"
                    f"<td class='n'>{html.escape(r.get('LineagePermanovaP',''))}</td>"
                    "<td>"
                    + (" + ".join(html.escape(w) for w in winners) if winners
                       else "<span class='note'>無顯著軸</span>")
                    + "</td>"
                    "</tr>"
                )

        body = (
            f"<p>載入 <b>{len(rows):,}</b> 個 region，<b>{len(cols)}</b> 個欄位。</p>"
            "<h3>各軸的可檢定數與顯著數（p ≤ 0.05）</h3>"
            "<table><tr><th>軸</th><th>可檢定 region</th><th>顯著</th><th>比例</th></tr>"
            + "".join(axis_rows) + "</table>"
        )
        if has_lineage and detail:
            body += (
                f"<h3>逐位點：lineage 軸可檢定的 {len(detail)} 個位點</h3>"
                "<table><tr><th>位點</th><th>軸</th><th>群數</th><th>read</th>"
                "<th>F</th><th>p</th><th>顯著的軸</th></tr>"
                + "".join(detail) + "</table>"
                "<p class='note'>「顯著的軸」列出該位點上 p ≤ 0.05 的所有軸，"
                "用來判斷甲基差異究竟由哪個分組解釋。空白的 F/p 代表未計算（群數不足），不是 0。</p>"
            )
        elif has_lineage:
            body += "<p class='note'>本批次沒有任何 region 的 lineage 軸達到可檢定的群數。</p>"
        else:
            body += ("<p class='note'>此 summary 由不支援 lineage 軸的 ISM 版本產生；"
                     "重跑 <code>--group-by-tag ...,lv</code> 後本面板會顯示逐位點的軸歸因。</p>")
        panels.append(panel("ism", "4 · 甲基 × 標籤關聯", True, body=body))
    else:
        reason = "未提供 --ism-summary（ISM 尚未支援讀取 lc/lu/lv 標籤）"
        avail["ism"] = {"available": False, "reason": reason}
        panels.append(panel("ism", "4 · 甲基 × 標籤關聯", False, reason))

    # ── 組裝 ──────────────────────────────────────────────────────
    n_ok = sum(1 for v in avail.values() if v.get("available"))
    css = """
body{margin:0;background:#fbfbfa;color:#1f2328;font:16px/1.65 -apple-system,"Noto Sans TC",sans-serif}
@media(prefers-color-scheme:dark){body{background:#14171a;color:#e6e8ea}
 .p{background:#1b1f23;border-color:#2b3237}th{background:#232a30}code{background:#1e2429}
 .kpi div{background:#232a30}}
.w{max-width:1000px;margin:0 auto;padding:28px 20px 80px}
h1{font-size:1.6rem;margin:0 0 6px}h2{font-size:1.15rem;margin:0 0 12px}h3{font-size:.98rem;margin:20px 0 8px}
.sub{color:#5b6570;font-size:.9rem}
.p{background:#fff;border:1px solid #e2e5e9;border-radius:10px;padding:18px 20px;margin:18px 0}
.p.na{opacity:.75;border-left:4px solid #a15c00}
.reason{color:#a15c00;font-size:.9rem;margin:0}
table{border-collapse:collapse;width:100%;font-size:.87rem;margin:8px 0}
th,td{padding:5px 9px;text-align:left;border-bottom:1px solid #e2e5e9}
th{background:#eef2f6}td.n{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
td.b{width:45%}td.b span{display:block;height:9px;background:#0b6fa4;border-radius:5px}
code{background:#f4f6f8;padding:1px 5px;border-radius:4px;font-size:.85em}
.kpi{display:flex;flex-wrap:wrap;gap:12px;margin:10px 0}
.kpi div{background:#eef2f6;border-radius:8px;padding:10px 16px;min-width:120px}
.kpi b{display:block;font-size:1.35rem;color:#0b6fa4}
.kpi span{font-size:.78rem;color:#5b6570}
.note{font-size:.85rem;color:#5b6570}
footer{margin-top:40px;padding-top:16px;border-top:1px solid #e2e5e9;color:#5b6570;font-size:.82rem}
"""
    doc = (
        "<!DOCTYPE html><html lang='zh-Hant'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        f"<title>{html.escape(args.sample)} · lineage 觀察報告</title><style>{css}</style></head><body><div class='w'>"
        f"<h1>{html.escape(args.sample)} · lineage 觀察報告</h1>"
        f"<p class='sub'>資料來源 <code>{html.escape(str(root))}</code>　·　"
        f"可用面板 {n_ok}/{len(avail)}</p>"
        + "".join(panels)
        + "<footer>所有數值直接取自 artifact，未經本頁重新計算。"
        "不可用面板已明列原因，不靜默省略。</footer></div></body></html>"
    )
    out.write_text(doc, encoding="utf-8")
    (out.with_suffix(".availability.json")).write_text(
        json.dumps(avail, ensure_ascii=False, indent=1, sort_keys=True) + "\n", encoding="utf-8"
    )

    print(f"panels available : {n_ok}/{len(avail)}")
    for k, v in sorted(avail.items()):
        print(f"  {k:14s} {'OK' if v.get('available') else 'NA — ' + v.get('reason','')}")
    print(f"output           : {out} ({out.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
