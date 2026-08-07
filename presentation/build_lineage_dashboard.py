#!/usr/bin/env python3
"""lineage 觀察儀表板 — standalone HTML，fail-closed。

設計依據 docs/lineage/DASHBOARD_SPEC.md 與 DASHBOARD_COLOR_AND_LEGIBILITY.md。
反捏造做法沿用 build_exact_ps_layered_workstation.py:171-250 的 require/AuthorityError 骨架：
缺任何必要輸入就 exit 2，絕不半渲染。

用法:
    python3 presentation/build_lineage_dashboard.py --out-root <RUN> --sample HCC1395 \
        [--ism-summary <significance_summary.csv>] [--ism-root <ISM 輸出根>] \
        --output <report.html>
"""
from __future__ import annotations

import argparse
import csv
import glob
import gzip
import hashlib
import html
import json
import os
import sys
from collections import Counter, defaultdict
from pathlib import Path

# ── fail-closed 原語 ────────────────────────────────────────────────────
class AuthorityError(RuntimeError):
    pass


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise AuthorityError(msg)


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def esc(v) -> str:
    return html.escape(str(v), quote=True)


def num(v) -> str:
    return f"{v:,}" if isinstance(v, (int,)) else str(v)


def pct(a: float, b: float, digits: int = 1) -> str:
    return "—" if not b else f"{a / b * 100:.{digits}f}%"


def rate(hits: int, n: int) -> str:
    """D2：n<30 依 NCHS 標準抑制百分比，只報原始計數。"""
    if n <= 0:
        return "<span class='suppressed' title='分母為 0'>無分母</span>"
    if n < 30:
        return (f"<b>{hits} / {n}</b> <span class='suppressed' "
                f"title='n&lt;30，依 NCHS Data Presentation Standards 不以百分比呈現'>%抑制</span>")
    return f"<b>{pct(hits, n)}</b> <span class='denom'>({hits:,}/{n:,})</span>"


# ── 色票（DASHBOARD_COLOR_AND_LEGIBILITY §2）────────────────────────────
LS_COLOR = {"P": "#b9b8ad", "M": "#b66e20", "A": "#a94336", "U": "#176b58"}
LS_LABEL = {
    "P": "P · 部分覆蓋（子樹斷言）",
    "M": "M · 同分並列（tie）",
    "A": "A · 拓撲未定（abstain）",
    "U": "U · 唯一解",
}
LS_ORDER = ["P", "M", "A", "U"]  # 由最不確定到最確定
DEPTH_COLOR = ["#c9c8bd", "#9ab8ac", "#6ba18d", "#3f8a70", "#217055", "#0d5540"]

CHROM_LEN = {  # GRCh38, chr1-22
    "chr1": 248956422, "chr2": 242193529, "chr3": 198295559, "chr4": 190214555,
    "chr5": 181538259, "chr6": 170805979, "chr7": 159345973, "chr8": 145138636,
    "chr9": 138394717, "chr10": 133797422, "chr11": 135086622, "chr12": 133275309,
    "chr13": 114364328, "chr14": 107043718, "chr15": 101991189, "chr16": 90338345,
    "chr17": 83257441, "chr18": 80373285, "chr19": 58617616, "chr20": 64444167,
    "chr21": 46709983, "chr22": 50818468,
}
CHROMS = list(CHROM_LEN)


# ── 讀取層 ──────────────────────────────────────────────────────────────
def read_tsv_gz(path: Path):
    with gzip.open(path, "rt", newline="") as fh:
        yield from csv.DictReader(fh, delimiter="\t")


def load_paths(p: Path) -> dict:
    lp, depth, chrom_pos = Counter(), Counter(), defaultdict(list)
    hidden = rows = 0
    mut_examples = []
    for r in read_tsv_gz(p):
        rows += 1
        lp[r["lineage_path"]] += 1
        depth[int(r["depth"])] += 1
        if r["is_hidden"] == "true":
            hidden += 1
        if r["acquired_position"] != ".":
            chrom_pos[r["chrom"]].append(int(r["acquired_position"]))
        if (len(mut_examples) < 24 and r["mutation_order"] != "."
                and r["best_tree_unique"] == "true" and int(r["depth"]) >= 2):
            mut_examples.append({
                "region": r["region_id"], "path": r["lineage_path"],
                "order": r["mutation_order"], "depth": int(r["depth"]),
                "score": r["edge_score_fraction"], "hidden": r["is_hidden"] == "true",
            })
    return {"rows": rows, "hidden": hidden, "lineage_path": lp, "depth": depth,
            "chrom_pos": dict(chrom_pos), "mut_examples": mut_examples}


def load_assign(p: Path, cap: int) -> dict:
    n = full_cov = tree_sup = pure_ref = 0
    per_chrom, kdist, pat, shape = Counter(), Counter(), Counter(), Counter()
    qnames = set()
    for i, r in enumerate(read_tsv_gz(p)):
        if i >= cap:
            break
        n += 1
        per_chrom[r["chrom"]] += 1
        kdist[int(r["k"])] += 1
        qnames.add(r["qname_sha256"])
        pv = r["pattern_vector"]
        # 純 REF = 該 read 在此 block 的每個已觀察位點都是參考等位 → 它不帶任何突變，
        # 位置就是樹根，因此不會有階層路徑 lv。這是 lv 缺席的主因，不是品質問題。
        if "A" not in pv:
            pure_ref += 1
        shape[("ALT" if "A" in pv else "純REF", "部分覆蓋" if "X" in pv else "完整覆蓋")] += 1
        if r["is_full_cov"] == "true":
            full_cov += 1
        else:
            pat[pv] += 1
        if r["tree_supported"] == "true":
            tree_sup += 1
    return {"rows": n, "full_cov": full_cov, "tree_supported": tree_sup,
            "per_chrom": per_chrom, "k": kdist, "distinct_reads": len(qnames),
            "partial_patterns": pat, "pure_ref": pure_ref, "shape": shape,
            "capped": n >= cap}


def load_receipts(d: Path, sample: str) -> dict:
    agg, per_chrom, total_bytes = Counter(), {}, 0
    files = sorted(d.glob(f"{sample}.*.tag_bam.receipt.json"))
    for rp in files:
        rec = json.loads(rp.read_text(encoding="utf-8"))
        st = rec.get("stats") or {}
        chrom = rp.name.split(".")[1]
        per_chrom[chrom] = {k: int(v) for k, v in st.items()}
        for k, v in st.items():
            agg[k] += int(v)
        bam = d / rp.name.replace(".tag_bam.receipt.json", ".lineage_tagged.bam")
        if bam.is_file():
            sz = bam.stat().st_size
            per_chrom[chrom]["bytes"] = sz
            total_bytes += sz
            per_chrom[chrom]["indexed"] = (d / (bam.name + ".bai")).is_file()
    return {"agg": agg, "per_chrom": per_chrom, "bytes": total_bytes, "n_files": len(files)}


def load_stages(p: Path) -> dict:
    """S13：stages.tsv 是 append-only，同一 (stage,chrom) 可能多筆 → 取最新，否則耗時算兩次。"""
    if not p.is_file():
        return {"raw": 0, "dedup": {}, "dup_note": ""}
    raw = list(csv.DictReader(p.open(newline=""), delimiter="\t"))
    latest = {}
    for r in raw:
        if r.get("status") != "OK":
            continue
        latest[(r["stage"], r["chrom"])] = r  # 後出現者覆蓋 = 最新
    per_stage = defaultdict(lambda: {"n": 0, "sec": 0})
    for (stage, _), r in latest.items():
        per_stage[stage]["n"] += 1
        per_stage[stage]["sec"] += int(r.get("seconds") or 0)
    raw_tag = sum(1 for r in raw if r.get("stage") == "tag_bam")
    dedup_tag = sum(1 for k in latest if k[0] == "tag_bam")
    return {"raw": len(raw), "dedup": dict(per_stage),
            "dup_note": f"tag_bam 原始 {raw_tag} 筆 → 去重後 {dedup_tag} 條染色體"}


def load_ism(p: Path | None) -> dict | None:
    if p is None or not p.is_file():
        return None
    rows = list(csv.DictReader(p.open(newline="")))
    return {"rows": rows, "cols": list(rows[0].keys()) if rows else []}


# ── HTML 元件 ───────────────────────────────────────────────────────────
def kpi(label: str, value: str, note: str, unit: str = "") -> str:
    badge = f"<span class='ub ub-{unit}'>{esc(unit)}</span>" if unit else ""
    return (f"<div class='metric'><span class='label'>{esc(label)} {badge}</span>"
            f"<span class='value'>{value}</span><span class='note'>{note}</span></div>")


def stacked(items, total: int, height: int = 26) -> str:
    """100% 堆疊條。items = [(label, count, color)]"""
    if not total:
        return "<p class='denom'>（無資料）</p>"
    segs = "".join(
        f"<span style='flex:{c} 0 0;background:{col}' "
        f"title='{esc(lab)}: {c:,} ({c/total*100:.2f}%)'></span>"
        for lab, c, col in items if c > 0
    )
    rows = "".join(
        f"<div class='dist-row'><span><i class='swatch' style='background:{col}'></i>{esc(lab)}</span>"
        f"<span class='count'>{c:,}</span><span class='pct'>{c/total*100:.2f}%</span></div>"
        for lab, c, col in items
    )
    return f"<div class='stack'>{segs}</div><div class='dist'>{rows}</div>"


def hbars(counter: Counter, top: int = 18, color: str = "#176b58", label_fmt=str) -> str:
    if not counter:
        return "<p class='denom'>（無資料）</p>"
    mx = max(counter.values())
    out = []
    for k, v in counter.most_common(top):
        out.append(
            f"<div class='dist-row'><span>{esc(label_fmt(k))}</span>"
            f"<span class='barline'>"
            f"<span style='flex:0 0 {v/mx*100:.2f}%;background:{color}'></span></span>"
            f"<span class='count'>{v:,}</span></div>"
        )
    more = len(counter) - top
    tail = f"<p class='denom'>另有 {more:,} 個未列出（依計數排序取前 {top}）</p>" if more > 0 else ""
    return f"<div class='dist'>{''.join(out)}</div>{tail}"


def funnel(steps) -> str:
    """steps = [(label, value, note)]；階間顯示落差與原因。"""
    if not steps:
        return ""
    cells = []
    for i, (lab, val, note) in enumerate(steps):
        drop = ""
        if i > 0:
            prev = steps[i - 1][1]
            d = prev - val
            if prev:
                drop = (f"<div class='drop'>▼ {d:,}"
                        f"<span class='denom'>（{d/prev*100:.1f}% of {prev:,}）</span></div>")
        cells.append(
            f"<div class='funnel-step'><span class='label'>{esc(lab)}</span>"
            f"<b>{val:,}</b><span class='note'>{note}</span>{drop}</div>"
        )
    return f"<div class='funnel'>{''.join(cells)}</div>"


def section(sid: str, kicker: str, title: str, lead: str, body: str,
            caveat: str = "", denom: str = "") -> str:
    d = f"<div class='denom'>{denom}</div>" if denom else ""
    c = f"<div class='claim'>{caveat}</div>" if caveat else ""
    return (f"<section class='section' id='{sid}'>"
            f"<div class='section-head'><div>"
            f"<div class='eyebrow'>{esc(kicker)}</div><h2>{title}</h2>"
            f"<p>{lead}</p>{d}</div></div>{c}{body}</section>")


CSS = """
:root{--paper:#f3f0e6;--panel:#fffdf7;--ink:#17231e;--muted:#667069;--line:#c9c8bd;
--soft:#e7e5da;--accent:#176b58;--accent2:#285f8f;--amber:#b66e20;--danger:#a94336;
--purple:#6b5592;--shadow:0 10px 32px rgba(30,42,35,.08)}
*{box-sizing:border-box}
html{scroll-behavior:smooth;background:var(--paper)}
body{margin:0;color:var(--ink);background:var(--paper);line-height:1.55;
font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Noto Sans TC","PingFang TC",sans-serif}
.authority{position:sticky;top:0;z-index:30;display:flex;gap:.9rem;flex-wrap:wrap;align-items:center;
justify-content:center;padding:.5rem 1rem;background:#123c34;color:#fff;font-size:.78rem}
.authority b{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-weight:700}
.shell{width:min(1360px,calc(100% - 32px));margin:auto}
.hero{padding:3rem 0 1.6rem;border-bottom:1px solid var(--line)}
.eyebrow{font-size:.74rem;font-weight:800;letter-spacing:.12em;text-transform:uppercase;color:var(--accent)}
h1{font-family:Georgia,"Noto Serif TC",serif;font-size:clamp(2rem,4.4vw,4.2rem);line-height:1;
letter-spacing:-.04em;margin:.4rem 0 .9rem;max-width:1050px}
h2{font-family:Georgia,"Noto Serif TC",serif;font-size:clamp(1.4rem,2.6vw,2.15rem);line-height:1.12;margin:.15rem 0 .5rem}
h3{font-size:.95rem;margin:1.4rem 0 .45rem}
p{margin:.3rem 0 .8rem}
.lead{font-size:clamp(.98rem,1.6vw,1.18rem);max-width:900px;color:#3e4a44}
.boundary{display:grid;gap:.5rem;margin-top:1.1rem;padding:.95rem 1.05rem;border-left:5px solid var(--amber);
background:#fff7e7;max-width:1050px;font-size:.88rem}
.boundary b{color:#7a4614}
.claim{padding:.7rem .9rem;border-left:4px solid var(--danger);background:#fdf0ee;
font-size:.84rem;margin:0 0 1rem}
.nav{display:flex;gap:.35rem;flex-wrap:wrap;padding:.9rem 0}
.nav a{display:inline-flex;align-items:center;min-height:34px;padding:.3rem .6rem;border:1px solid var(--line);
background:var(--panel);text-decoration:none;color:var(--ink);font-size:.8rem}
.nav a:hover{background:var(--ink);color:#fff;border-color:var(--ink)}
.section{padding:2.1rem 0;border-bottom:1px solid var(--line)}
.section-head p{max-width:820px;color:var(--muted);margin:0}
.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:1px;
background:var(--line);border:1px solid var(--line);margin:1rem 0}
.metric{background:var(--panel);padding:1rem}
.metric .label{display:block;color:var(--muted);font-size:.75rem;min-height:2.5em}
.metric .value{display:block;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;
font-size:clamp(1.3rem,2.2vw,2.1rem);font-weight:800;line-height:1.08;margin:.3rem 0}
.metric .note{display:block;font-size:.73rem;color:var(--muted)}
.denom{font-size:.76rem;color:var(--muted);margin-top:.3rem}
.stack{display:flex;height:26px;overflow:hidden;border:1px solid rgba(0,0,0,.15);margin:.9rem 0 .7rem;background:var(--soft)}
.stack span{display:block;min-width:1px;border-right:1px solid rgba(255,255,255,.3)}
.dist{display:grid;gap:.35rem}
.dist-row{display:grid;grid-template-columns:minmax(120px,.8fr) minmax(0,3fr) minmax(70px,auto);
gap:.6rem;align-items:center;font-size:.82rem}
.dist-row .barline{height:8px;background:var(--soft);display:flex;overflow:hidden}
.dist-row .barline>span{height:100%;min-width:0}
.swatch{width:.7rem;height:.7rem;display:inline-block;margin-right:.4rem;vertical-align:-.03rem;
border:1px solid rgba(0,0,0,.15)}
.count,.pct{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;text-align:right}
.pct{color:var(--muted)}
.funnel{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1.4rem;margin:1rem 0}
.funnel-step{position:relative;padding:.95rem;background:var(--panel);border:1px solid var(--line)}
.funnel-step:not(:last-child)::after{content:"→";position:absolute;right:-1.15rem;top:38%;
font-size:1.35rem;color:var(--muted)}
.funnel-step .label{display:block;font-size:.74rem;color:var(--muted)}
.funnel-step b{display:block;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:1.4rem;margin:.2rem 0}
.funnel-step .note{display:block;font-size:.72rem;color:var(--muted)}
.funnel-step .drop{margin-top:.4rem;font-size:.74rem;color:var(--danger)}
.grid-2{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:1rem}
.panel{background:var(--panel);border:1px solid var(--line);padding:1rem;box-shadow:var(--shadow);min-width:0}
.panel h3{margin-top:0}
.table-wrap{overflow:auto;border:1px solid var(--line);background:var(--panel);max-height:520px}
table{border-collapse:collapse;width:100%}
th,td{padding:.55rem .6rem;border-bottom:1px solid var(--line);text-align:left;font-size:.8rem;vertical-align:top}
th{position:sticky;top:0;background:#e9e7dd;font-size:.7rem;letter-spacing:.04em;text-transform:uppercase;z-index:2}
td.num{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;text-align:right;white-space:nowrap}
.tag{display:inline-block;padding:.1rem .4rem;border:1px solid currentColor;font-size:.68rem;
font-weight:800;letter-spacing:.03em;text-transform:uppercase;margin:.05rem .1rem}
.ub{display:inline-block;padding:.02rem .3rem;font-size:.62rem;font-weight:800;letter-spacing:.04em;
text-transform:uppercase;border:1px solid var(--muted);color:var(--muted);border-radius:2px;vertical-align:.06rem}
.ub-alignment{border-color:var(--accent2);color:var(--accent2)}
.ub-read{border-color:var(--accent);color:var(--accent)}
.ub-unit{border-color:var(--purple);color:var(--purple)}
.ub-vertex{border-color:var(--amber);color:var(--amber)}
.suppressed{color:#8a6d3b;background:#fff7e7;border:1px dashed var(--amber);padding:0 .3rem;font-size:.72rem}
.callout{padding:.9rem 1rem;border:1px solid var(--line);background:#edf5f1;font-size:.85rem;margin:.9rem 0}
.na{opacity:.72;border-left:4px solid var(--amber);padding-left:.9rem}
.na .why{color:var(--amber);font-size:.85rem}
canvas{display:block;width:100%;background:var(--panel);border:1px solid var(--line)}
.st-SIG{background:#dff0e8;font-weight:800}
.st-NS{background:transparent}
.st-NT{background:repeating-linear-gradient(45deg,#f0efe7,#f0efe7 4px,#e2e0d5 4px,#e2e0d5 8px);
color:var(--muted);font-style:italic}
.legend{display:flex;flex-wrap:wrap;gap:.4rem .9rem;margin-top:.7rem;padding-top:.6rem;
border-top:1px solid var(--line);font-size:.74rem;color:var(--muted)}
.legend span{display:inline-flex;align-items:center;gap:.3rem}
footer{padding:2rem 0 4rem;color:var(--muted);font-size:.8rem}
details>summary{cursor:pointer;font-size:.85rem;color:var(--accent);padding:.3rem 0}
@media print{.authority{position:static}.nav{display:none}}
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-root", required=True, type=Path)
    ap.add_argument("--sample", required=True)
    ap.add_argument("--ism-summary", type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--max-assign-rows", type=int, default=1_200_000)
    args = ap.parse_args()

    root: Path = args.out_root
    S = args.sample
    try:
        paths_p = root / "paths" / f"{S}.unit_lineage_paths.tsv.gz"
        assign_p = root / "assign" / f"{S}.all.read_lineage_assignments.tsv.gz"
        require(paths_p.is_file(), f"缺少 lineage paths: {paths_p}")
        require(assign_p.is_file(), f"缺少 assignments: {assign_p}")

        P = load_paths(paths_p)
        A = load_assign(assign_p, args.max_assign_rows)
        R = load_receipts(root / "bam", S)
        ST = load_stages(root / "logs" / "stages.tsv")
        I = load_ism(args.ism_summary)

        require(P["rows"] > 0, "lineage paths 為空")
        require(A["rows"] > 0, "assignments 為空")

        prov = {
            "paths": {"path": str(paths_p), "sha256": sha256_file(paths_p),
                      "size_bytes": paths_p.stat().st_size},
            "assignments": {"path": str(assign_p), "sha256": sha256_file(assign_p),
                            "size_bytes": assign_p.stat().st_size},
        }
        if args.ism_summary and args.ism_summary.is_file():
            prov["ism_summary"] = {"path": str(args.ism_summary),
                                   "sha256": sha256_file(args.ism_summary),
                                   "size_bytes": args.ism_summary.stat().st_size}
    except AuthorityError as e:
        print(f"FAIL CLOSED: {e}", file=sys.stderr)
        return 2

    ag = R["agg"]
    total_aln = ag.get("reads_total", 0)
    ls_counts = {k: ag.get(f"ls_{k}", 0) for k in LS_ORDER}
    ls_total = sum(ls_counts.values())

    from lineage_dashboard_sections import build_sections  # noqa: E402
    parts = build_sections(S, P, A, R, ST, I, prov, ls_counts, ls_total, total_aln)

    meta = "".join(
        f'<meta name="longlineage-{esc(k)}-sha256" content="{esc(v["sha256"])}">'
        for k, v in prov.items()
    )
    auth = " · ".join(
        f'{esc(k)} <b>{esc(v["sha256"][:12])}…</b>' for k, v in prov.items()
    )
    nav = "".join(f"<a href='#{sid}'>{esc(t)}</a>" for sid, t in parts["nav"])

    doc = (
        "<!doctype html><html lang='zh-Hant'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='color-scheme' content='light'>"
        f"<meta name='longlineage-sample' content='{esc(S)}'>{meta}"
        f"<title>{esc(S)} · lineage 觀察儀表板</title><style>{CSS}</style></head><body>"
        f"<div class='authority'>{esc(S)} · {auth}</div>"
        f"<div class='shell'>{parts['hero']}<nav class='nav'>{nav}</nav>"
        f"<main>{parts['body']}</main>"
        "<footer>所有數值直接取自上述 SHA-256 綁定的 artifact，本頁未重新計算任何科學數字。"
        "缺件面板標示原因而非省略；n&lt;30 的比例依 NCHS 標準抑制百分比。</footer>"
        "</div></body></html>"
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    tmp = args.output.with_suffix(args.output.suffix + ".tmp")
    tmp.write_text(doc, encoding="utf-8")
    os.replace(tmp, args.output)

    print(f"sections     : {len(parts['nav'])}")
    print(f"alignments   : {total_aln:,}")
    print(f"ls total     : {ls_total:,}  " + " ".join(f"{k}={ls_counts[k]:,}" for k in LS_ORDER))
    print(f"lineage paths: {P['rows']:,} vertices ({P['hidden']:,} hidden)")
    print(f"assignments  : {A['rows']:,} rows / {A['distinct_reads']:,} distinct reads")
    print(f"output       : {args.output} ({args.output.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
