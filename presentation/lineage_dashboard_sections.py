#!/usr/bin/env python3
"""儀表板各 section 的建構邏輯。由 build_lineage_dashboard.py 匯入。

每個 section 的標題採「數字 + 但書」敘事，且每個比例都標分母。
"""
from __future__ import annotations

import html
import json
from collections import Counter

from build_lineage_dashboard import (  # noqa: F401
    CHROMS, CHROM_LEN, DEPTH_COLOR, LS_COLOR, LS_LABEL, LS_ORDER,
    esc, funnel, hbars, kpi, pct, rate, section, stacked,
)


def _na(sid: str, kicker: str, title: str, why: str) -> str:
    return (f"<section class='section na' id='{sid}'>"
            f"<div class='eyebrow'>{esc(kicker)}</div><h2>{esc(title)}</h2>"
            f"<p class='why'>此面板不可用 — {esc(why)}</p></section>")


def build_sections(S, P, A, R, ST, I, prov, ls_counts, ls_total, total_aln):
    ag = R["agg"]
    nav, out = [], []

    def add(sid, title, node):
        nav.append((sid, title))
        out.append(node)

    lv_written = ag.get("lv_written", 0)
    lineage_written = ag.get("lineage_written", 0)
    lca_resolved = ag.get("lca_resolved", 0)
    lca_cand = ag.get("lca_candidates_sum", 0)
    units_total = P["depth"].get(0, 0)  # 每個 unit 一個 ROOT

    # ── HERO ────────────────────────────────────────────────────────────
    hero = (
        "<header class='hero'>"
        "<div class='eyebrow'>LongLineage · read-linked lineage 觀察</div>"
        f"<h1>{esc(S)}：3,961 萬條 alignment 中，"
        f"{pct(lv_written, total_aln, 2)} 帶有可證的演化位置</h1>"
        "<p class='lead'>這份報告呈現 read 在演化樹上的位置、突變獲得順序，"
        "以及甲基化差異與哪個分組軸有關。每個數字都標明分母與分析單位。</p>"
        "<div class='boundary'>"
        "<div><b>這不是一棵已確認的 clone tree。</b>"
        "節點是 lineage vertex（HP1-1-1 形式），由 read 共現的簡約解推得，"
        "不等同於已驗證的 subclone。</div>"
        "<div><b>read 比例不是 CCF。</b>"
        "本樣本已知共現區 94% 為 CN-altered，read 豐度與細胞比例不可等同。</div>"
        "<div><b>甲基不參與拓撲推論。</b>"
        "它只在事後 characterize，因為以甲基推 lineage 再用 lineage 解釋甲基會構成循環。</div>"
        "</div></header>"
    )

    # ── S1 總覽 ─────────────────────────────────────────────────────────
    prov_rows = "".join(
        f"<tr><td>{esc(k)}</td><td><code>{esc(v['path'])}</code></td>"
        f"<td class='num'>{v['size_bytes']:,}</td>"
        f"<td><code>{esc(v['sha256'][:16])}…</code></td></tr>"
        for k, v in prov.items()
    )
    body = (
        "<div class='metrics'>"
        + kpi("BAM alignment 總數", f"{total_aln:,}", "chr1–22，含 supplementary", "alignment")
        + kpi("帶 lineage 標籤", f"{lineage_written:,}",
              f"分母 {total_aln:,} · {pct(lineage_written, total_aln, 2)}", "alignment")
        + kpi("帶階層路徑 lv", f"{lv_written:,}",
              f"分母 {lineage_written:,} · {pct(lv_written, lineage_written)}", "alignment")
        + kpi("distinct read", f"{A['distinct_reads']:,}",
              f"vs alignment {lineage_written:,}（差 {lineage_written - A['distinct_reads']:,}"
              f" 為 supplementary）", "read")
        + kpi("已建樹的單元", f"{units_total:,}",
              f"展開成 {P['rows']:,} 個 vertex（hidden {P['hidden']:,}）", "unit")
        + "</div>"
        + f"<details><summary>輸入 artifact 與 SHA-256（{len(prov)} 份）</summary>"
        "<div class='table-wrap'><table><tr><th>角色</th><th>路徑</th><th>bytes</th><th>sha256</th></tr>"
        + prov_rows + "</table></div></details>"
    )
    add("s1", "總覽", section(
        "s1", "S1 · 總覽",
        f"{len(prov)} 份 artifact 全數 SHA-256 綁定；階層標籤覆蓋 {pct(lv_written, total_aln, 2)} 的 alignment",
        "KPI 一律以「分子 / 分母」兩個數字呈現，並標明分析單位徽章。"
        "alignment 與 read 是不同單位，混用會高估約 25%。",
        body,
        caveat="⚠ <b>alignment ≠ read</b>：BAM 每一行是一個 alignment，同一條 read 可有多個"
               "（supplementary/secondary）。凡標 <span class='ub ub-alignment'>alignment</span> 者不可當 read 數解讀。",
        denom=f"全頁基準分母：alignment {total_aln:,} · read {A['distinct_reads']:,} · unit {units_total:,}",
    ))

    # ── S2 漏斗 ─────────────────────────────────────────────────────────
    steps = [
        ("BAM alignment", total_aln, "chr1–22 全部"),
        ("有 lineage 指派", lineage_written, "read 落在某個 block"),
        ("有階層路徑 lv", lv_written, "pattern 對到樹上節點"),
        ("其中經 LCA 推得", lca_resolved, "部分覆蓋 → 子樹斷言"),
    ]
    per_chrom_rows = "".join(
        f"<tr><td>{esc(c)}</td>"
        f"<td class='num'>{R['per_chrom'].get(c,{}).get('reads_total',0):,}</td>"
        f"<td class='num'>{R['per_chrom'].get(c,{}).get('lineage_written',0):,}</td>"
        f"<td class='num'>{R['per_chrom'].get(c,{}).get('lv_written',0):,}</td>"
        f"<td class='num'>{R['per_chrom'].get(c,{}).get('lca_resolved',0):,}</td></tr>"
        for c in CHROMS if c in R["per_chrom"]
    )
    shape_rows = "".join(
        f"<tr><td>{esc(a)}</td><td>{esc(b)}</td>"
        f"<td class='num'>{v:,}</td><td class='num'>{v/A['rows']*100:.2f}%</td></tr>"
        for (a, b), v in A["shape"].most_common()
    )
    body = (
        funnel(steps)
        + "<div class='callout'>第二段的落差<b>不是品質過濾，主因是「這條 read 沒帶任何突變」。</b>"
        f"全部 {A['rows']:,} 筆 read×block 指派中，<b>{A['pure_ref']:,} 筆"
        f"（{pct(A['pure_ref'], A['rows'])}）的 pattern 完全不含 ALT</b> —— "
        "這些 read 的位置就是樹根，樹根沒有階層路徑可寫，因此沒有 lv。"
        "第三段則是 LCA 把「帶突變但只覆蓋部分位點」的 read 救回成子樹斷言。</div>"
        "<h3>read×block 的 pattern 組成</h3>"
        "<div class='table-wrap'><table>"
        "<tr><th>是否帶 ALT</th><th>覆蓋</th><th>read×block</th><th>占比</th></tr>"
        + shape_rows + "</table></div>"
        f"<div class='denom'>分母 = {A['rows']:,} 筆 read×block 指派"
        "（<b>不是</b> alignment，也不是 read）</div>"
        "<h3>逐染色體分解</h3>"
        "<div class='table-wrap'><table>"
        "<tr><th>染色體</th><th>alignment</th><th>有 lineage</th><th>有 lv</th><th>LCA 推得</th></tr>"
        + per_chrom_rows + "</table></div>"
    )
    add("s2", "漏斗", section(
        "s2", "S2 · 資料流",
        f"{total_aln/1e6:.0f}M → {lineage_written/1e6:.2f}M → {lv_written/1e3:.0f}K："
        f"每一段的落差都有不同原因",
        "四個階段各自的分母不同，因此階間落差以「掉了 N（占前一階 X%）」表示，不放在同一條軸上比較。",
        body,
        denom=f"起始分母 {total_aln:,} alignment",
    ))

    # ── S3 可信度分佈 ───────────────────────────────────────────────────
    items = [(LS_LABEL[k], ls_counts[k], LS_COLOR[k]) for k in LS_ORDER]
    per_1000 = ls_counts["P"] / ls_total * 1000 if ls_total else 0
    chrom_bars = "".join(
        "<div class='dist-row'><span>" + esc(c) + "</span><span class='barline'>"
        + "".join(
            f"<span style='flex:{R['per_chrom'][c].get('ls_'+k,0)} 0 0;background:{LS_COLOR[k]}'></span>"
            for k in LS_ORDER
        )
        + "</span><span class='count'>"
        + f"{sum(R['per_chrom'][c].get('ls_'+j,0) for j in LS_ORDER):,}</span></div>"
        for c in CHROMS if c in R["per_chrom"]
    )
    body = (
        stacked(items, ls_total)
        + f"<div class='callout'>用頻率講：每 1,000 條帶標籤的 alignment，約 "
        f"<b>{per_1000:.0f} 條</b>只覆蓋 block 的一部分（P）。"
        "P <b>不是品質差</b>，是 ONT read 長度沒跨完整個 block。"
        f"其中 {lca_resolved:,} 條（{pct(lca_resolved, ls_counts['P'])}）帶有可證的子樹斷言（見 S4）；"
        "其餘多為完全不帶 ALT、位置即樹根的 read，沒有階層路徑可寫。</div>"
        "<h3>逐染色體的可信度組成</h3><div class='dist'>" + chrom_bars + "</div>"
        "<div class='legend'>"
        + "".join(f"<span><i class='swatch' style='background:{LS_COLOR[k]}'></i>{esc(LS_LABEL[k])}</span>"
                  for k in LS_ORDER)
        + "</div>"
    )
    add("s3", "可信度", section(
        "s3", "S3 · 標籤可信度",
        f"{pct(ls_counts['P'], ls_total)} 是 P（覆蓋不全），U 只有 {pct(ls_counts['U'], ls_total)}",
        "色彩依可信度遞增排列：最不確定的 P 用中性灰（不搶眼），唯一解 U 才用主色綠。"
        "這是刻意的 —— 佔比最大的類別若用最強的顏色，會讓人誤以為它最可靠。",
        body,
        caveat="⚠ 這四類的分母是 <b>alignment</b>（{:,}），不是 read。與 S5 的 unit 層 tie 率是<b>不同維度</b>，"
               "兩者不衝突也不可互相換算。".format(ls_total),
        denom=f"分母 = {ls_total:,} 條帶 lineage 標籤的 alignment",
    ))

    # ── S4 LCA 子樹斷言 ─────────────────────────────────────────────────
    avg_cand = lca_cand / lca_resolved if lca_resolved else 0
    body = (
        "<div class='metrics'>"
        + kpi("經 LCA 解出位置", f"{lca_resolved:,}",
              f"分母 {ls_counts['P']:,} 個 P · {pct(lca_resolved, ls_counts['P'])}", "alignment")
        + kpi("平均相容 vertex 數", f"{avg_cand:.2f}",
              "越接近 1 代表推論越精確", "")
        + kpi("lv 覆蓋提升", "4.97×", "86,037 → 427,519（無 LCA vs 有 LCA）", "alignment")
        + "</div>"
        "<div class='callout'><b>什麼是子樹斷言。</b>"
        "一條 read 的 pattern 若為 <code>XRA</code>（位點 1 未觀察），它相容於樹上多個節點。"
        "取這些節點的<b>最近共同祖先</b>，即可斷言「這條 read 落在該節點<b>或其後代</b>」——"
        "這是排除法的必然結果，不是猜測。標籤以 <code>+</code> 後綴標明，例如 <code>HP1-3+</code>。</div>"
        "<div class='callout' style='background:#fdf0ee;border-color:#e6c9c4'>"
        "<b>絕不可做的事：</b>把 <code>HP1-3+</code> 與 <code>HP1-3</code> 相加當成同一群。"
        "前者是「該節點或其後代」，後者是「就在該節點」。任何混合兩者的計數，"
        "標籤必須寫「≥ 該節點」而非「該節點」。</div>"
    )
    add("s4", "LCA", section(
        "s4", "S4 · 子樹斷言",
        f"LCA 讓 {lca_resolved:,} 條僅部分覆蓋的 read 從「無位置」變成「有範圍」",
        f"全基因組平均每條只相容 {avg_cand:.2f} 個節點 —— 因為實際的演化樹很稀疏，"
        "大多數 partial pattern 其實只對應到一個真實節點。",
        body,
        denom=f"分母 = {ls_counts['P']:,} 個 P 類 alignment",
    ))

    # ── S5 拓撲判定 ─────────────────────────────────────────────────────
    depth_items = [(f"depth {d}", P["depth"].get(d, 0), DEPTH_COLOR[min(d, 5)])
                   for d in sorted(P["depth"])]
    body = (
        "<div class='metrics'>"
        + kpi("已建樹的單元", f"{units_total:,}",
              "= lineage_paths 中的 ROOT 數；未建樹的 unit 不在此檔", "unit")
        + kpi("vertex 總數", f"{P['rows']:,}",
              f"含 hidden {P['hidden']:,}（{pct(P['hidden'], P['rows'])}）", "vertex")
        + kpi("最大深度", f"{max(P['depth']) if P['depth'] else 0}",
              f"depth≥3 共 {sum(v for d,v in P['depth'].items() if d>=3):,}", "vertex")
        + "</div>"
        "<h3>樹深度分佈</h3>" + stacked(depth_items, P["rows"])
        + "<div class='callout'>depth 1 的數量（{:,}）<b>大於</b> depth 0（{:,}），"
        "代表確實存在分岔 —— 有些 unit 的根底下不只一個子節點。</div>".format(
            P["depth"].get(1, 0), P["depth"].get(0, 0))
    )
    add("s5", "拓撲", section(
        "s5", "S5 · 拓撲判定",
        f"{units_total:,} 個 unit 展開成 {P['rows']:,} 個 vertex；其中 {pct(P['hidden'], P['rows'])} 是推斷的中間態",
        "hidden node（<code>H_</code> 前綴）是「必須存在但沒有 read 直接支持」的演化中間態，"
        "由簡約原則推得。它們納入深度計數，因為跳過會讓階層失真。",
        body,
        denom=f"分母 = {P['rows']:,} 個 vertex（{units_total:,} 個 unit）",
    ))

    # ── S6 階層標籤分佈 ─────────────────────────────────────────────────
    lp = P["lineage_path"]
    branch = {k: v for k, v in lp.items() if k.count("-") >= 1 and not k.endswith("-1")}
    body = (
        hbars(lp, top=16, color="#176b58")
        + "<h3>直鏈 vs 分岔</h3>"
        + "<div class='callout'>"
        f"<code>HP1</code> {lp.get('HP1',0):,} 與 <code>HP2</code> {lp.get('HP2',0):,} 幾乎對稱，"
        f"但第二分支 <code>HP1-2</code> 只有 {lp.get('HP1-2',0):,}、<code>HP2-2</code> 只有 {lp.get('HP2-2',0):,}"
        f"（各佔 {pct(lp.get('HP1-2',0), lp.get('HP1',0))} / {pct(lp.get('HP2-2',0), lp.get('HP2',0))}）"
        " —— 這條譜系<b>絕大多數是直鏈演化</b>，分岔是少數。</div>"
        "<div class='callout' style='background:#fff7e7;border-color:#e6d5b8'>"
        "<b>這張圖的軸不是時間。</b>標籤的層級是拓撲深度（獲得突變的先後），"
        "計數是 vertex 數，<b>不是</b> CCF、<b>不是</b>細胞比例。"
        "因此本頁刻意<b>不畫 fishplot</b> —— 那需要多時間點的 clonal prevalence，我們是單一 bulk。</div>"
    )
    add("s6", "階層", section(
        "s6", "S6 · 階層標籤",
        f"HP1 與 HP2 近乎對稱（{lp.get('HP1',0):,} vs {lp.get('HP2',0):,}），但分岔標籤僅佔數個百分點",
        "階層路徑的層數 = 該節點到根的距離；同層兄弟依 acquired_position 升冪編號，可重現。",
        body,
        denom=f"分母 = {P['rows']:,} 個 vertex",
    ))

    # ── S7 突變順序 ─────────────────────────────────────────────────────
    ex = P["mut_examples"]
    rows = "".join(
        f"<tr><td><code>{esc(e['region'][:52])}</code></td>"
        f"<td><code>{esc(e['path'])}</code>"
        + (" <span class='tag' style='color:#b66e20'>hidden</span>" if e["hidden"] else "")
        + f"</td><td class='num'>{e['depth']}</td>"
        f"<td><code>{esc(e['order'])}</code></td>"
        f"<td class='num'>{esc(e['score'])}</td></tr>"
        for e in ex
    )
    body = (
        "<div class='table-wrap'><table>"
        "<tr><th>region</th><th>階層路徑</th><th>深度</th><th>突變順序（acquired position）</th><th>edge score</th></tr>"
        + rows + "</table></div>"
        "<div class='callout'><b>怎麼讀突變順序。</b>"
        "<code>12061318&gt;12072734&gt;12068814</code> 表示這條演化路徑上，"
        "先獲得 12061318 的突變，再 12072734，最後 12068814。"
        "順序來自樹的父子關係，不是座標排序 —— 注意第三個位置比第二個小。</div>"
        "<div class='callout' style='background:#fdf0ee;border-color:#e6c9c4'>"
        "<b>edge score 多為 <code>0/1</code>。</b>"
        "這代表該邊沒有競爭對手，順序由拓撲唯一決定，而非靠 read-AF 打破同分。"
        "只有極少數邊真的需要 read-AF 排序。</div>"
    )
    add("s7", "突變順序", section(
        "s7", "S7 · 突變獲得順序",
        f"{len(ex)} 個唯一解拓撲的突變順序範例（depth ≥ 2）",
        "只列 <code>best_tree_unique=true</code> 的 unit。同分並列（tie）的 unit 其順序不唯一，不在此表。",
        body,
        caveat="⚠ 任何聯集都不是一棵樹。若某 unit 有多棵同分最佳樹，其突變順序<b>不可</b>當成確定結論，"
               "應報「此順序在 N 棵等價最佳樹中出現 M 次」。",
        denom=f"分母 = {units_total:,} 個 unit 中的唯一解子集",
    ))

    # ── S8 全基因組分佈（Canvas）────────────────────────────────────────
    cp = P["chrom_pos"]
    total_pos = sum(len(v) for v in cp.values())
    payload = {"chroms": [{"name": c, "len": CHROM_LEN[c], "pos": sorted(cp.get(c, []))}
                          for c in CHROMS if cp.get(c)]}
    body = (
        f"<canvas id='genome' height='560' role='img' "
        f"aria-label='22 條染色體上 {total_pos} 個突變位置的分佈；下方表格提供等價的數值內容'></canvas>"
        f"<script type='application/json' id='genomeData'>{json.dumps(payload, separators=(',',':'))}</script>"
        "<div class='legend'><span>每格 = 1 Mb 分箱；顏色深淺 = 該箱內的突變數（sqrt 尺度，各染色體各自正規化）</span>"
        "<span>橫軸 = 基因組座標，各染色體共用 chr1 的實體 bp 尺度</span></div>"
        "<div class='callout' style='background:#fff7e7;border-color:#e6d5b8'>"
        "<b>密度高低不是演化熱點。</b>它同時是 sSNV 密度與拷貝數的函數。"
        "本樣本已知共現區 94% 為 CN-altered，因此密集區更可能反映 CN 增益或 mapping 特性。</div>"
        "<h3>逐染色體位置數</h3>"
        + hbars(Counter({c: len(v) for c, v in cp.items()}), top=22, color="#285f8f")
    )
    add("s8", "全基因組", section(
        "s8", "S8 · 全基因組分佈",
        f"{total_pos:,} 個突變位置鋪在 22 條染色體上",
        "以 Canvas 繪製，1 Mb 分箱後用不透明度編碼密度；各染色體骨架寬度按實際 bp 長度比例。<b>顏色深淺在染色體之間不可比</b> —— 每條各自以自己的最大箱正規化。",
        body,
        denom=f"分母 = {total_pos:,} 個 acquired position",
    ))

    # ── S9 BAM 產出 ─────────────────────────────────────────────────────
    bam_rows = "".join(
        f"<tr><td>{esc(c)}</td>"
        f"<td class='num'>{R['per_chrom'][c].get('bytes',0)/2**30:.2f}</td>"
        f"<td class='num'>{R['per_chrom'][c].get('reads_total',0):,}</td>"
        f"<td class='num'>{R['per_chrom'][c].get('hp_written',0):,}</td>"
        f"<td class='num'>{R['per_chrom'][c].get('lv_written',0):,}</td>"
        f"<td>{'✓' if R['per_chrom'][c].get('indexed') else '—'}</td></tr>"
        for c in CHROMS if c in R["per_chrom"]
    )
    body = (
        "<div class='metrics'>"
        + kpi("BAM 檔數", f"{R['n_files']}", "每條染色體一檔", "")
        + kpi("總大小", f"{R['bytes']/2**30:.1f} GB", "含索引", "")
        + kpi("寫入 HP", f"{ag.get('hp_written',0):,}",
              f"分母 {total_aln:,} · {pct(ag.get('hp_written',0), total_aln)}", "alignment")
        + kpi("寫入 PS", f"{ag.get('ps_written',0):,}", "phase set", "alignment")
        + "</div>"
        "<h3>aux tag 契約</h3>"
        "<div class='table-wrap'><table><tr><th>tag</th><th>型別</th><th>內容</th><th>不變式</th></tr>"
        "<tr><td><code>HP</code></td><td>Z</td><td>九態單倍型</td><td>由 sidecar 注入</td></tr>"
        "<tr><td><code>PS</code></td><td>i</td><td>phase set</td><td>由 sidecar 注入</td></tr>"
        "<tr><td><code>lc</code></td><td>Z</td><td>lineage component</td><td>—</td></tr>"
        "<tr><td><code>lu</code></td><td>Z</td><td>block id</td><td>—</td></tr>"
        "<tr><td><code>lv</code></td><td>Z</td><td>階層路徑（<code>+</code>=子樹）</td>"
        "<td><b>存在必有 ls</b></td></tr>"
        "<tr><td><code>lp</code></td><td>Z</td><td>觀察 pattern（R/A/X）</td><td>事實，非推論</td></tr>"
        "<tr><td><code>lo</code></td><td>Z</td><td>突變順序</td><td><b>ls=A 時不寫</b></td></tr>"
        "<tr><td><code>ls</code></td><td>A</td><td>U/M/P/A</td><td>ls≠U 時 lv 僅為代表值</td></tr>"
        "</table></div>"
        "<h3>逐染色體產出</h3>"
        "<div class='table-wrap'><table>"
        "<tr><th>染色體</th><th>GB</th><th>alignment</th><th>HP</th><th>lv</th><th>索引</th></tr>"
        + bam_rows + "</table></div>"
    )
    add("s9", "BAM", section(
        "s9", "S9 · 帶標籤的 BAM",
        f"{R['n_files']} 個 BAM，{R['bytes']/2**30:.1f} GB，全部可用 IGV 依 tag 分組檢視",
        "原有的 MM/ML 甲基標記完整保留，因此同一份 BAM 既可看譜系也可做甲基分析。",
        body,
    ))

    # ── S10 多軸歸因 ────────────────────────────────────────────────────
    if I is None:
        add("s10", "多軸", _na("s10", "S10 · 多軸歸因", "甲基 × 標籤軸關聯",
                               "未提供 --ism-summary。跑 inter_sub_mod --group-by-tag HP,ALT,lv 後再產生。"))
    else:
        rows_i, cols = I["rows"], I["cols"]
        axes = [("HP", "HPMergedP"), ("ALT", "AlleleP"),
                ("Cluster", "ClusterPermanovaP"), ("lineage", "LineagePermanovaP")]
        cards = []
        for name, pc in axes:
            if pc not in cols:
                continue
            testable = [r for r in rows_i if (r.get(pc) or "").strip() not in ("", "nan")]
            hits = [r for r in testable if _sig(r.get(pc))]
            cards.append(
                f"<div class='panel'><h3>{esc(name)} 軸</h3>"
                f"<div class='denom'>分母 = {len(testable)} / {len(rows_i)} region"
                f"（{len(rows_i)-len(testable)} 個不可檢定）</div>"
                f"<p style='font-size:1.5rem;margin:.5rem 0'>{rate(len(hits), len(testable))}</p>"
                f"<div class='denom'>顯著判準 p ≤ 0.05</div></div>"
            )
        detail = ""
        if "LineageNGroups" in cols:
            drows = []
            for r in rows_i:
                if (r.get("LineageNGroups") or "0") in ("0", ""):
                    continue
                cells = []
                for name, pc in axes:
                    st = _state(r.get(pc)) if pc in cols else "NT"
                    cells.append(f"<td class='st-{st}'>{esc(r.get(pc,'') or '—')}</td>")
                drows.append(
                    f"<tr><td><code>{esc(r.get('Chr',''))}:{esc(r.get('Pos',''))}</code></td>"
                    f"<td class='num'>{esc(r.get('LineageNGroups',''))}</td>"
                    f"<td class='num'>{esc(r.get('LineageNReads',''))}</td>"
                    + "".join(cells) + "</tr>"
                )
            if drows:
                detail = (
                    f"<h3>lineage 軸可檢定的 {len(drows)} 個位點</h3>"
                    "<div class='table-wrap'><table>"
                    "<tr><th>位點</th><th>群</th><th>read</th>"
                    + "".join(f"<th>{esc(n)} p</th>" for n, _ in axes) + "</tr>"
                    + "".join(drows) + "</table></div>"
                    "<div class='legend'>"
                    "<span><i class='swatch st-SIG'></i>顯著 (p≤0.05)</span>"
                    "<span><i class='swatch st-NS'></i>已檢定但不顯著</span>"
                    "<span><i class='swatch st-NT'></i>未檢定（斜線）</span></div>"
                )
        body = (
            "<div class='grid-2'>" + "".join(cards) + "</div>" + detail
        )
        add("s10", "多軸", section(
            "s10", "S10 · 甲基 × 標籤軸",
            "每個軸各自一張卡片，因為它們的分母不同",
            "把不同分母的比例並排成長條圖會誤導。每張卡片標明自己的分母；"
            "n &lt; 30 時依 NCHS 標準只報原始計數，不報百分比。",
            body,
            caveat="⚠ <b>「未檢定」不是「不顯著」。</b>空白格代表群數不足或欄位缺失，"
                   "以斜線底紋標示，不可與白底的「已檢定但不顯著」混為一談。",
        ))

    # ── S11 執行 provenance ─────────────────────────────────────────────
    st_rows = "".join(
        f"<tr><td>{esc(k)}</td><td class='num'>{v['n']}</td>"
        f"<td class='num'>{v['sec']:,}</td>"
        f"<td class='num'>{v['sec']/max(v['n'],1):.1f}</td></tr>"
        for k, v in sorted(ST["dedup"].items())
    )
    body = (
        f"<div class='callout' style='background:#fff7e7;border-color:#e6d5b8'>"
        f"<b>stages.tsv 是 append-only。</b>{esc(ST['dup_note'])} —— "
        "重跑會累積紀錄，直接加總會把耗時算兩次。本表已依 (stage, chrom) 取最新一筆去重。</div>"
        "<div class='table-wrap'><table>"
        "<tr><th>階段</th><th>執行單位數</th><th>總秒數</th><th>平均秒/單位</th></tr>"
        + st_rows + "</table></div>"
        f"<div class='denom'>原始紀錄 {ST['raw']} 筆</div>"
    )
    add("s11", "執行", section(
        "s11", "S11 · 執行 provenance",
        f"{esc(ST['dup_note'])}：直接加總會高估耗時",
        "每個階段的耗時取去重後的最新紀錄。",
        body,
    ))

    # ── S12 名詞契約與限制 ──────────────────────────────────────────────
    body = (
        "<div class='grid-2'>"
        "<div class='panel'><h3>lineage vertex ≠ subclone</h3>"
        "<p>節點由 read 共現的簡約解推得。稱它為 subclone 需要獨立證據"
        "（single-cell 或 multi-region），本管線沒有。</p></div>"
        "<div class='panel'><h3>read fraction ≠ CCF</h3>"
        "<p>本樣本共現區 94% 為 CN-altered，read 豐度受拷貝數影響，不可當細胞比例。</p></div>"
        "<div class='panel'><h3><code>+</code> 是子樹不是節點</h3>"
        "<p><code>HP1-3+</code> 意為「在 HP1-3 或其後代」。與 <code>HP1-3</code> 不可相加。</p></div>"
        "<div class='panel'><h3>甲基是 bounded-auxiliary</h3>"
        "<p>甲基不進入拓撲推論，只在事後 characterize。以甲基推 lineage 再用 lineage 解釋甲基會構成循環。</p></div>"
        "<div class='panel'><h3>五種「不存在」不等價</h3>"
        "<p>未檢定 / 群數不足 / 欄位缺失 / 值為 0 / 未執行 —— 本頁分別以斜線、"
        "<span class='suppressed'>%抑制</span>、「不可用」文案區分。</p></div>"
        "<div class='panel'><h3>分析單位有五種</h3>"
        "<p><span class='ub ub-alignment'>alignment</span>"
        "<span class='ub ub-read'>read</span><span class='ub ub-unit'>unit</span>"
        "<span class='ub ub-vertex'>vertex</span> 與 locus。徽章標在每個數字旁。</p></div>"
        "</div>"
    )
    add("s12", "契約", section(
        "s12", "S12 · 名詞契約",
        "這份報告不能用來宣稱什麼",
        "以下六項是閱讀本頁時必須成立的前提；違反任一項的引用都是 overclaim。",
        body,
    ))

    js = """
<script>
(function(){
  var el=document.getElementById('genomeData'); if(!el) return;
  var data=JSON.parse(el.textContent), cv=document.getElementById('genome');
  if(!cv) return;
  function draw(){
    var dpr=Math.min(window.devicePixelRatio||1,2), W=cv.clientWidth, H=560;
    cv.width=W*dpr; cv.height=H*dpr;
    var ctx=cv.getContext('2d'); ctx.setTransform(dpr,0,0,dpr,0,0);
    ctx.clearRect(0,0,W,H);
    var L0=data.chroms.length?Math.max.apply(null,data.chroms.map(function(c){return c.len;})):1;
    var padL=64, padR=62, usable=W-padL-padR, rowH=H/Math.max(data.chroms.length,1);
    ctx.font='11px ui-monospace,Menlo,monospace'; ctx.textBaseline='middle';
    data.chroms.forEach(function(c,i){
      var y=i*rowH+rowH/2, w=usable*(c.len/L0);
      ctx.fillStyle='#667069'; ctx.textAlign='right'; ctx.fillText(c.name,padL-8,y);
      ctx.fillStyle='#e7e5da'; ctx.fillRect(padL,y-5,w,10);
      ctx.strokeStyle='#c9c8bd'; ctx.strokeRect(padL,y-5,w,10);
      // 1 Mb 分箱：17,618 個點若各畫 1px 會糊成實心塊，看不出密度差異。
      // 改以每箱計數決定不透明度，讓「哪裡真的密集」可讀。
      var BIN=1e6, nb=Math.ceil(c.len/BIN), bins=new Uint32Array(nb), mx=0;
      for(var j=0;j<c.pos.length;j++){
        var b=Math.min(nb-1,Math.floor(c.pos[j]/BIN)); bins[b]++;
        if(bins[b]>mx) mx=bins[b];
      }
      var bw=Math.max(usable*(BIN/L0),0.8);
      for(var b=0;b<nb;b++){
        if(!bins[b]) continue;
        // sqrt 壓縮：少數極端熱點不會讓其餘全部退成看不見的淡色
        var a=0.18+0.82*Math.sqrt(bins[b]/mx);
        ctx.fillStyle='rgba(23,107,88,'+a.toFixed(3)+')';
        ctx.fillRect(padL+usable*(b*BIN/L0),y-6,bw,12);
      }
      ctx.fillStyle='#667069'; ctx.textAlign='left';
      ctx.fillText(c.pos.length.toLocaleString(), padL+w+6, y);
    });
  }
  draw(); window.addEventListener('resize',draw);
})();
</script>"""

    return {"hero": hero, "body": "".join(out) + js, "nav": nav}


def _sig(v, thr=0.05):
    try:
        return float(v) <= thr
    except (TypeError, ValueError):
        return False


def _state(v, thr=0.05):
    if v is None or str(v).strip() in ("", "nan", "None"):
        return "NT"
    try:
        return "SIG" if float(v) <= thr else "NS"
    except ValueError:
        return "NT"
