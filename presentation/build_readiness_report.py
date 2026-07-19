#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Render an audit-only readiness JSON as standalone HTML.

This presentation script does not open BAM/VCF/sidecar artifacts and does not
perform scientific aggregation. It only maps already formatted audit fields.
"""

from __future__ import annotations

import argparse
import html
import json
from pathlib import Path
from typing import Any, Dict, Iterable, List


def esc(value: Any) -> str:
    return html.escape(str(value), quote=True)


def status_class(value: str) -> str:
    upper = value.upper()
    if "PASS" in upper or "PRESENT" in upper or "READABLE" in upper:
        return "ok"
    if "BLOCK" in upper or "ABSENT" in upper or "NO_GO" in upper or "FAILED" in upper:
        return "bad"
    if "PARTIAL" in upper or "PENDING" in upper or "UNKNOWN" in upper or "COMPONENT" in upper:
        return "warn"
    return "neutral"


def badge(value: str) -> str:
    return f'<span class="badge {status_class(value)}">{esc(value)}</span>'


def path_code(value: str) -> str:
    return f'<code class="path">{esc(value)}</code>'


def render_input_rows(rows: Iterable[Dict[str, Any]]) -> str:
    rendered: List[str] = []
    for row in rows:
        rendered.append(
            "<tr>"
            f"<td><strong>{esc(row['role'])}</strong></td>"
            f"<td>{esc(row['size_display'])}</td>"
            f"<td>{badge(row['state'])}</td>"
            f"<td>{path_code(row['path'])}</td>"
            "</tr>"
        )
    return "".join(rendered)


def render_stage_rows(rows: Iterable[Dict[str, Any]]) -> str:
    rendered: List[str] = []
    for row in rows:
        rendered.append(
            "<tr>"
            f"<td class=\"stage-num\">{int(row['order']):02d}</td>"
            f"<td><strong>{esc(row['stage'])}</strong><small>{esc(row['implementation'])}</small></td>"
            f"<td>{badge(row['status'])}</td>"
            f"<td>{esc(row['timing'])}</td>"
            f"<td>{esc(row['bottleneck'])}</td>"
            "</tr>"
        )
    return "".join(rendered)


def render_measurements(rows: Iterable[Dict[str, Any]]) -> str:
    rendered: List[str] = []
    for row in rows:
        rendered.append(
            '<article class="measurement">'
            f"<div>{badge(row['classification'])}</div>"
            f"<h3>{esc(row['stage'])}</h3>"
            f"<p class=\"measure-value\">{esc(row['wall_display'])}</p>"
            f"<p>{esc(row['result'])}</p>"
            f"<small>{esc(row['scope'])}</small>"
            "</article>"
        )
    return "".join(rendered)


def render_history(rows: Iterable[Dict[str, Any]]) -> str:
    rendered: List[str] = []
    for row in rows:
        width = float(row["bar_width_percent"])
        width = min(100.0, max(0.4, width))
        rendered.append(
            '<div class="history-row">'
            '<div class="history-label">'
            f"<strong>{esc(row['stage'])}</strong>"
            f"<span>{esc(row['scope'])}</span>"
            "</div>"
            '<div class="history-track" aria-hidden="true">'
            f'<div class="history-bar {status_class(row["classification"])}" style="width:{width:.1f}%"></div>'
            "</div>"
            '<div class="history-value">'
            f"<strong>{esc(row['wall_display'])}</strong>"
            f"<span>{esc(row['classification'])}</span>"
            "</div>"
            f"<p>{esc(row['result'])}；{esc(row['limitation'])}</p>"
            "</div>"
        )
    return "".join(rendered)


def render_bottlenecks(rows: Iterable[Dict[str, Any]]) -> str:
    rendered: List[str] = []
    for row in rows:
        rendered.append(
            '<article class="bottleneck">'
            f'<span class="rank">{int(row["rank"]):02d}</span>'
            "<div>"
            f"<div>{badge(row['severity'])}</div>"
            f"<h3>{esc(row['title'])}</h3>"
            f"<p>{esc(row['detail'])}</p>"
            f"<p class=\"unblock\"><strong>解鎖：</strong>{esc(row['unblock'])}</p>"
            "</div>"
            "</article>"
        )
    return "".join(rendered)


def render_commands(rows: Iterable[Dict[str, Any]]) -> str:
    rendered: List[str] = []
    for row in rows:
        rendered.append(
            "<details>"
            f"<summary>{esc(row['purpose'])} · exit {esc(row['exit_code'])}</summary>"
            '<div class="detail-body">'
            f"<p><strong>Input</strong> {path_code(row['input'])}</p>"
            f"<pre><code>{esc(row['command'])}</code></pre>"
            f"<p><strong>Output</strong> {path_code(row['output'])}</p>"
            f"<pre><code>{esc(row['output_snippet'])}</code></pre>"
            "</div>"
            "</details>"
        )
    return "".join(rendered)


def render_sources(rows: Iterable[Dict[str, Any]]) -> str:
    return "".join(
        f"<li><strong>{esc(row['label'])}</strong>{path_code(row['path'])}</li>" for row in rows
    )


def render_baseline_rows(rows: Iterable[Dict[str, Any]]) -> str:
    rendered: List[str] = []
    for row in rows:
        rendered.append(
            "<tr>"
            f"<td><strong>{esc(row['area'])}</strong><small>{esc(row['classification'])}</small></td>"
            f"<td>{path_code(row['source'])}</td>"
            f"<td>{esc(row['recorded_at'])}</td>"
            f"<td>{esc(row['time'])}</td>"
            f"<td>{badge(row['digest_status'])}</td>"
            f"<td>{esc(row['verdict'])}</td>"
            "</tr>"
        )
    return "".join(rendered)


def build_html(data: Dict[str, Any]) -> str:
    required = {
        "schema_name",
        "schema_version",
        "report_id",
        "verdict",
        "task",
        "inputs",
        "current_measurements",
        "historical_context",
        "pipeline_stages",
        "bottlenecks",
        "time_answer",
        "commands",
        "sources",
    }
    missing = sorted(required.difference(data))
    if missing:
        raise ValueError(f"missing required audit fields: {', '.join(missing)}")
    if data["schema_name"] != "longlineage.hcc1395_fullspeed_readiness_timing_audit":
        raise ValueError("unexpected audit schema_name")
    if data["verdict"]["state"] != "NO_GO":
        raise ValueError("this renderer is fail-closed for readiness audits and requires verdict=NO_GO")

    verdict = data["verdict"]
    task = data["task"]
    counts = data["scope_counts"]
    host = data["host"]
    profile = data["planned_fullspeed_profile"]
    time_answer = data["time_answer"]
    display = data.get("display", {})
    page_title = display.get(
        "page_title",
        "HCC1395 全速執行就緒度與時間稽核 · LongLineage",
    )
    hero_title = display.get("hero_title", "HCC1395 全速執行就緒度與時間稽核")
    brand_subtitle = display.get("brand_subtitle", "HCC1395 full-speed audit")
    scope_note = display.get(
        "scope_note",
        "Task B · HCC1395 chr1–22 sample-complete；對 7-dataset production corpus 為 partial。",
    )
    answer_heading = display.get(
        "answer_heading",
        "現在的完整時間不是「很久」，而是「沒有可成功的路徑」",
    )
    history_heading = display.get(
        "history_heading",
        "歷史時間只回答容量脈絡，不回答新 C++ 總時間",
    )
    input_heading = display.get("input_heading", "四項核心輸入被 mount 阻擋")
    input_lead = display.get(
        "input_lead",
        "VCF/sidecar authority 在 big7 可核對；292.06 GB raw BAM、BAI 與 reference 位於目前未掛載的 big8。",
    )
    baseline_rows = data.get("baseline_artifacts", [])
    baseline_nav = '        <a href="#baselines">基準與結果</a>\n' if baseline_rows else ""
    baseline_section = ""
    if baseline_rows:
        baseline_section = f"""      <section id="baselines">
        <div class="section-kicker">04B · Baseline and result authority</div>
        <h2>{esc(display.get('baseline_heading', 'sSNV 共現與區域樹基準不能混為同一證據層'))}</h2>
        <p class="section-lead">{esc(display.get('baseline_lead', '每列同時顯示來源、時間、digest 與可比性；execution PASS 不自動等於 scientific parity。'))}</p>
        <div class="table-wrap">
          <table>
            <thead><tr><th>Area</th><th>Source</th><th>Recorded</th><th>Time</th><th>Digest</th><th>Current verdict</th></tr></thead>
            <tbody>{render_baseline_rows(baseline_rows)}</tbody>
          </table>
        </div>
      </section>

"""
    html_document = f"""<!doctype html>
<html lang="zh-Hant" data-partial="true" data-report-id="{esc(data['report_id'])}">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light">
  <title>{esc(page_title)}</title>
  <style>
    :root {{
      --paper:#f3f0e8; --surface:#fffdf8; --ink:#17202a; --muted:#5f6b76;
      --navy:#17324d; --navy2:#244b6f; --line:#d8d2c5; --red:#b42318;
      --red-bg:#fef3f2; --amber:#b54708; --amber-bg:#fffaeb; --green:#027a48;
      --green-bg:#ecfdf3; --blue:#175cd3; --blue-bg:#eff8ff;
      --shadow:0 12px 32px rgba(23,50,77,.10);
    }}
    * {{ box-sizing:border-box; }}
    html {{ max-width:100%; scroll-behavior:smooth; overflow-x:clip; }}
    body {{
      margin:0; color:var(--ink); background:var(--paper);
      font-family:Inter, ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
      line-height:1.62; overflow-x:clip;
    }}
    a {{ color:inherit; }}
    code, pre {{ font-family:"SFMono-Regular", Consolas, monospace; }}
    .partial-ribbon {{
      background:var(--red); color:white; text-align:center; padding:.5rem 1rem;
      font-weight:800; letter-spacing:.06em; font-size:.78rem;
    }}
    .layout {{ width:100%; max-width:1480px; margin:0 auto; display:grid; grid-template-columns:250px 1fr; }}
    aside {{
      position:sticky; top:0; height:100vh; padding:2rem 1.3rem; border-right:1px solid var(--line);
      background:#ebe6dc;
    }}
    .brand {{ font-weight:900; color:var(--navy); letter-spacing:.03em; }}
    .brand small {{ display:block; color:var(--muted); font-weight:650; letter-spacing:0; }}
    nav {{ min-width:0; max-width:100%; margin-top:2rem; display:grid; gap:.35rem; }}
    nav a {{ text-decoration:none; padding:.52rem .65rem; border-left:3px solid transparent; color:#44515d; }}
    nav a:hover, nav a:focus-visible {{ color:var(--navy); border-color:var(--red); background:rgba(255,255,255,.55); }}
    aside .scope-note {{ margin-top:2rem; font-size:.82rem; color:var(--muted); }}
    main {{ min-width:0; padding:2.2rem clamp(1.2rem,4vw,4.5rem) 5rem; }}
    .hero {{
      background:var(--navy); color:white; padding:clamp(2rem,5vw,4.5rem);
      border-top:8px solid var(--red); box-shadow:var(--shadow);
    }}
    .eyebrow {{ text-transform:uppercase; letter-spacing:.13em; font-weight:800; font-size:.78rem; color:#b9d4eb; }}
    h1 {{ max-width:980px; margin:.55rem 0 1rem; font-size:clamp(2.1rem,5vw,4.6rem); line-height:1.02; letter-spacing:-.045em; }}
    .hero-lead {{ max-width:850px; font-size:clamp(1.02rem,2vw,1.25rem); color:#e8f0f7; }}
    .verdict-line {{ display:flex; flex-wrap:wrap; gap:.65rem; align-items:center; margin-top:1.6rem; }}
    .verdict {{ background:white; color:var(--red); font-weight:950; font-size:1.45rem; padding:.42rem .8rem; }}
    .hero-meta {{ display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:1px; margin-top:2.2rem; background:#55738e; }}
    .hero-meta div {{ background:var(--navy2); padding:1rem; }}
    .hero-meta span {{ display:block; color:#b9d4eb; font-size:.78rem; text-transform:uppercase; letter-spacing:.06em; }}
    .hero-meta strong {{ font-size:1.05rem; }}
    section {{ min-width:0; max-width:100%; scroll-margin-top:1rem; padding:4.2rem 0 0; }}
    .section-kicker {{ color:var(--red); font-weight:900; letter-spacing:.12em; text-transform:uppercase; font-size:.75rem; }}
    h2 {{ margin:.25rem 0 .8rem; color:var(--navy); font-size:clamp(1.6rem,3vw,2.6rem); letter-spacing:-.025em; line-height:1.14; }}
    h3 {{ margin:.3rem 0 .35rem; line-height:1.25; }}
    .section-lead {{ max-width:900px; color:var(--muted); font-size:1.03rem; }}
    .answer-grid {{ display:grid; grid-template-columns:1.4fr 1fr; gap:1rem; margin-top:1.4rem; }}
    .answer-primary {{ background:var(--red-bg); border-left:6px solid var(--red); padding:1.4rem; }}
    .answer-primary strong {{ display:block; color:var(--red); font-size:1.45rem; }}
    .answer-secondary {{ background:var(--surface); border:1px solid var(--line); padding:1.4rem; }}
    .note {{ padding:1rem 1.15rem; border:1px solid #e6b8b4; background:#fff8f7; margin-top:1rem; }}
    .measure-grid {{ display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:1rem; margin-top:1.35rem; }}
    .measurement {{ background:var(--surface); border:1px solid var(--line); border-top:4px solid var(--navy); padding:1.15rem; }}
    .measurement .measure-value {{ color:var(--navy); font-size:1.65rem; font-weight:900; margin:.25rem 0; }}
    .measurement small, td small {{ display:block; color:var(--muted); margin-top:.45rem; }}
    .badge {{ display:inline-block; padding:.18rem .45rem; border-radius:3px; font-size:.68rem; font-weight:850; letter-spacing:.035em; overflow-wrap:anywhere; }}
    .badge.ok {{ color:var(--green); background:var(--green-bg); }}
    .badge.bad {{ color:var(--red); background:var(--red-bg); }}
    .badge.warn {{ color:var(--amber); background:var(--amber-bg); }}
    .badge.neutral {{ color:var(--blue); background:var(--blue-bg); }}
    .table-wrap {{ width:100%; min-width:0; max-width:100%; overflow-x:auto; contain:inline-size; margin-top:1.3rem; border:1px solid var(--line); background:var(--surface); }}
    table {{ width:100%; border-collapse:collapse; min-width:820px; }}
    th {{ text-align:left; background:#e8edf1; color:var(--navy); font-size:.74rem; letter-spacing:.05em; text-transform:uppercase; }}
    th, td {{ padding:.82rem .9rem; border-bottom:1px solid var(--line); vertical-align:top; }}
    tbody tr:last-child td {{ border-bottom:0; }}
    tbody tr:hover {{ background:#fbf8f1; }}
    .stage-num {{ font-weight:900; color:#8b98a3; width:3rem; }}
    .path {{ display:block; white-space:normal; overflow-wrap:anywhere; color:#415266; font-size:.76rem; background:#f0ede6; padding:.35rem .45rem; margin-top:.2rem; }}
    .history {{ margin-top:1.5rem; display:grid; gap:.8rem; }}
    .history-row {{ display:grid; grid-template-columns:230px 1fr 210px; gap:1rem; align-items:center; background:var(--surface); border:1px solid var(--line); padding:1rem; }}
    .history-row p {{ grid-column:1/-1; margin:0; color:var(--muted); font-size:.88rem; }}
    .history-label span, .history-value span {{ display:block; color:var(--muted); font-size:.78rem; }}
    .history-track {{ height:14px; background:#e6e1d7; overflow:hidden; }}
    .history-bar {{ height:100%; background:var(--navy2); min-width:4px; }}
    .history-bar.bad {{ background:var(--red); }}
    .history-bar.warn {{ background:var(--amber); }}
    .history-value {{ text-align:right; }}
    .bottleneck-list {{ display:grid; gap:.7rem; margin-top:1.4rem; }}
    .bottleneck {{ display:grid; grid-template-columns:54px 1fr; gap:1rem; background:var(--surface); border:1px solid var(--line); padding:1.15rem; }}
    .rank {{ color:#a7afb7; font-size:1.5rem; font-weight:950; }}
    .bottleneck p {{ margin:.3rem 0; }}
    .unblock {{ color:#3c4d5c; }}
    .profile {{ display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); border:1px solid var(--line); margin-top:1.3rem; background:var(--surface); }}
    .profile div {{ padding:1rem; border-right:1px solid var(--line); }}
    .profile div:last-child {{ border:0; }}
    .profile span {{ color:var(--muted); display:block; font-size:.75rem; text-transform:uppercase; }}
    .profile strong {{ color:var(--navy); font-size:1.2rem; }}
    details {{ background:var(--surface); border:1px solid var(--line); margin:.65rem 0; }}
    summary {{ cursor:pointer; padding:.9rem 1rem; font-weight:800; color:var(--navy); }}
    .detail-body {{ padding:0 1rem 1rem; }}
    pre {{ white-space:pre-wrap; overflow-wrap:anywhere; background:#162535; color:#f4f7fa; padding:1rem; overflow:auto; }}
    .sources {{ padding-left:1.3rem; }}
    .sources li {{ margin:.8rem 0; }}
    .footer {{ margin-top:4rem; padding-top:1.5rem; border-top:1px solid var(--line); color:var(--muted); font-size:.83rem; }}
    @media (max-width:980px) {{
      .layout {{ display:block; }}
      aside {{ position:static; height:auto; border-right:0; border-bottom:1px solid var(--line); padding:1rem; }}
      nav {{ display:flex; overflow-x:auto; margin-top:.8rem; }}
      nav a {{ white-space:nowrap; border-left:0; border-bottom:3px solid transparent; }}
      aside .scope-note {{ display:none; }}
      .measure-grid {{ grid-template-columns:repeat(2,minmax(0,1fr)); }}
      .history-row {{ grid-template-columns:1fr; }}
      .history-row p {{ grid-column:auto; }}
      .history-value {{ text-align:left; }}
      .profile {{ grid-template-columns:repeat(2,minmax(0,1fr)); }}
    }}
    @media (max-width:620px) {{
      main {{ padding:1rem 1rem 3rem; }}
      .hero {{ padding:1.5rem; }}
      h1 {{ font-size:2.35rem; }}
      nav {{ display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); overflow:visible; }}
      nav a {{ white-space:normal; text-align:center; padding:.45rem .25rem; }}
      .hero-meta, .answer-grid, .measure-grid {{ grid-template-columns:1fr; }}
      .profile {{ grid-template-columns:1fr; }}
      .profile div {{ border-right:0; border-bottom:1px solid var(--line); }}
      .bottleneck {{ grid-template-columns:40px 1fr; }}
    }}
    @media print {{
      body {{ background:white; font-size:10pt; }}
      .partial-ribbon {{ print-color-adjust:exact; -webkit-print-color-adjust:exact; }}
      .layout {{ display:block; max-width:none; }}
      aside {{ display:none; }}
      main {{ padding:0; }}
      .hero {{ box-shadow:none; break-after:avoid; print-color-adjust:exact; -webkit-print-color-adjust:exact; }}
      section {{ padding-top:1.6rem; break-inside:auto; }}
      .measurement, .bottleneck, .history-row, table {{ break-inside:avoid; }}
      details {{ break-inside:avoid; }}
      details > * {{ display:block; }}
    }}
  </style>
</head>
<body>
  <div class="partial-ribbon">PARTIAL / READINESS AUDIT — 非 production run、非 P8 release evidence</div>
  <div class="layout">
    <aside>
      <div class="brand">LongLineage<small>{esc(brand_subtitle)}</small></div>
      <nav aria-label="報告章節">
        <a href="#answer">時間答案</a>
        <a href="#measured">本輪實測</a>
        <a href="#pipeline">全流程狀態</a>
        <a href="#history">歷史脈絡</a>
{baseline_nav}        <a href="#inputs">輸入</a>
        <a href="#bottlenecks">瓶頸</a>
        <a href="#profile">全速設定</a>
        <a href="#commands">命令證據</a>
        <a href="#sources">來源</a>
      </nav>
      <p class="scope-note">{esc(scope_note)}</p>
    </aside>
    <main>
      <header class="hero">
        <div class="eyebrow">Evidence → Gate → Timing · {esc(data['created_at'])}</div>
        <h1>{esc(hero_title)}</h1>
        <p class="hero-lead">{esc(verdict['summary'])}</p>
        <div class="verdict-line">
          <span class="verdict">NO-GO</span>
          {badge('影響 HIGH')}
          {badge('信心 HIGH')}
          {badge('科學輸出 0')}
        </div>
        <div class="hero-meta">
          <div><span>Scope</span><strong>HCC1395 · chr1–22 · {counts['autosomal_chr1_22_sites']:,} sites</strong></div>
          <div><span>Host</span><strong>{host['logical_cpus']} CPUs · {host['memory_available_display']} available</strong></div>
          <div><span>Storage</span><strong>big7 {host['big7_free_display']} free · big8 {host['big8_mount_state']}</strong></div>
        </div>
      </header>

      <section id="answer">
        <div class="section-kicker">01 · Direct answer</div>
        <h2>{esc(answer_heading)}</h2>
        <div class="answer-grid">
          <div class="answer-primary">
            <strong>Production total：不可量測</strong>
            {esc(time_answer['production_total'])}
          </div>
          <div class="answer-secondary">
            <strong>Production HTML：不可量測</strong>
            <p>{esc(time_answer['production_html'])}</p>
          </div>
        </div>
        <div class="note"><strong>不能相加：</strong>下方歷史時間來自不同程式、資料集範圍、成功狀態與演算法。{esc(time_answer['legacy_reference'])}</div>
      </section>

      <section id="measured">
        <div class="section-kicker">02 · Current measurements</div>
        <h2>本輪真正量到的只有 foundation 與 fail-closed 邊界</h2>
        <p class="section-lead">這些數字驗證 repository 能建置、測試與正確拒絕未就緒 production；它們不含 HCC1395 BAM 科學運算。</p>
        <div class="measure-grid">{render_measurements(data['current_measurements'])}</div>
      </section>

      <section id="pipeline">
        <div class="section-kicker">03 · Stage-by-stage</div>
        <h2>P0–P8 實際可執行度與時間缺口</h2>
        <div class="table-wrap">
          <table>
            <thead><tr><th>#</th><th>Stage / implementation</th><th>Status</th><th>Timing</th><th>Current bottleneck</th></tr></thead>
            <tbody>{render_stage_rows(data['pipeline_stages'])}</tbody>
          </table>
        </div>
      </section>

      <section id="history">
        <div class="section-kicker">04 · Non-comparable context</div>
        <h2>{esc(history_heading)}</h2>
        <p class="section-lead">長條以歷史最長 wall time 正規化；0.4% 是顯示下限。每列獨立，禁止相加或外推。</p>
        <div class="history">{render_history(data['historical_context'])}</div>
      </section>

{baseline_section}      <section id="inputs">
        <div class="section-kicker">05 · Input readiness</div>
        <h2>{esc(input_heading)}</h2>
        <p class="section-lead">{esc(input_lead)}</p>
        <div class="table-wrap">
          <table>
            <thead><tr><th>Role</th><th>Recorded size</th><th>Current state</th><th>Path</th></tr></thead>
            <tbody>{render_input_rows(data['inputs'])}</tbody>
          </table>
        </div>
      </section>

      <section id="bottlenecks">
        <div class="section-kicker">06 · Bottleneck ranking</div>
        <h2>先解鎖正確性與可執行性，再談 CPU 加速</h2>
        <div class="bottleneck-list">{render_bottlenecks(data['bottlenecks'])}</div>
      </section>

      <section id="profile">
        <div class="section-kicker">07 · Planned full-speed profile</div>
        <h2>解鎖後的單樣本 profiling 設定</h2>
        <p class="section-lead">{badge(profile['state'])} 這是 proposed PARTIAL benchmark profile，不可取代 exact seven-dataset production authority。</p>
        <div class="profile">
          <div><span>Compute</span><strong>{profile['compute_workers']} workers</strong></div>
          <div><span>Writers</span><strong>{profile['writer_threads']} threads</strong></div>
          <div><span>Coordinator</span><strong>{profile['coordinator_slots']} slots</strong></div>
          <div><span>Total ceiling</span><strong>{profile['total_declared_threads']} / 48</strong></div>
        </div>
        <div class="note"><strong>Staging</strong>{path_code(profile['staging_root'])}<strong>Final</strong>{path_code(profile['final_root'])}<strong>Storage policy</strong> {esc(profile['storage_reservation'])}（{esc(profile['storage_reservation_classification'])}）</div>
      </section>

      <section id="commands">
        <div class="section-kicker">08 · Reproducible evidence</div>
        <h2>本輪輸入、命令、輸出與實際片段</h2>
        {render_commands(data['commands'])}
      </section>

      <section id="sources">
        <div class="section-kicker">09 · Provenance</div>
        <h2>可追溯來源</h2>
        <ul class="sources">{render_sources(data['sources'])}</ul>
      </section>

      <footer class="footer">
        <strong>{esc(data['report_id'])}</strong> · schema {esc(data['schema_version'])}<br>
        {esc(data['claim_ceiling']['forbidden'])}<br>
        本頁由 Python presentation-only renderer 產生；所有科學與時間欄位皆來自凍結 audit JSON，renderer 不讀 BAM/VCF、不聚合 scientific tables。
      </footer>
    </main>
  </div>
</body>
</html>
"""
    return html_document


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    data = json.loads(args.evidence.read_text(encoding="utf-8"))
    rendered = build_html(data)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    print(
        json.dumps(
            {
                "status": "PASS",
                "scope": "AUDIT_PRESENTATION_ONLY",
                "evidence": str(args.evidence.resolve()),
                "output": str(args.output.resolve()),
                "bytes": len(rendered.encode("utf-8")),
            },
            ensure_ascii=False,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
