#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run offline browser smoke checks for a standalone presentation HTML file."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Dict, List

from playwright.sync_api import sync_playwright


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inspect_page(page: Any, html_uri: str, screenshot: Path) -> Dict[str, Any]:
    console_errors: List[str] = []
    page_errors: List[str] = []
    requests: List[str] = []
    page.on("console", lambda message: console_errors.append(message.text) if message.type == "error" else None)
    page.on("pageerror", lambda error: page_errors.append(str(error)))
    page.on("request", lambda request: requests.append(request.url))
    response = page.goto(html_uri, wait_until="networkidle")
    page.screenshot(path=str(screenshot), full_page=True)
    metrics = page.evaluate(
        """() => ({
          title: document.title,
          lang: document.documentElement.lang,
          partial: document.documentElement.dataset.partial,
          h1Count: document.querySelectorAll('h1').length,
          sectionCount: document.querySelectorAll('main section').length,
          tableCount: document.querySelectorAll('table').length,
          navLinkCount: document.querySelectorAll('nav a').length,
          ribbonVisible: !!document.querySelector('.partial-ribbon') &&
            getComputedStyle(document.querySelector('.partial-ribbon')).display !== 'none',
          scrollWidth: document.documentElement.scrollWidth,
          viewportWidth: window.innerWidth,
          scrollHeight: document.documentElement.scrollHeight,
          overflowElements: Array.from(document.querySelectorAll('body *'))
            .map((element) => {
              const rect = element.getBoundingClientRect();
              return {
                tag: element.tagName,
                className: typeof element.className === 'string' ? element.className : '',
                left: Math.round(rect.left),
                right: Math.round(rect.right),
                width: Math.round(rect.width)
              };
            })
            .filter((item) => item.left < -1 || item.right > window.innerWidth + 1)
            .slice(0, 20)
        })"""
    )
    external_requests = [
        url for url in requests if url.startswith(("http://", "https://"))
    ]
    checks = {
        "navigation_ok": response is None or response.ok,
        "lang_zh_hant": metrics["lang"] == "zh-Hant",
        "partial_flag": metrics["partial"] == "true",
        "single_h1": metrics["h1Count"] == 1,
        "sections_present": metrics["sectionCount"] >= 9,
        "tables_present": metrics["tableCount"] >= 2,
        "navigation_present": metrics["navLinkCount"] >= 9,
        "partial_ribbon_visible": metrics["ribbonVisible"],
        "no_horizontal_overflow": metrics["scrollWidth"] <= metrics["viewportWidth"] + 1,
        "no_console_errors": not console_errors,
        "no_page_errors": not page_errors,
        "no_external_requests": not external_requests,
    }
    return {
        "metrics": metrics,
        "checks": checks,
        "console_errors": console_errors,
        "page_errors": page_errors,
        "external_requests": external_requests,
        "screenshot": {
            "path": str(screenshot.resolve()),
            "size_bytes": screenshot.stat().st_size,
            "sha256": sha256(screenshot),
        },
        "pass": all(checks.values()),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--html", required=True, type=Path)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args()

    html_path = args.html.resolve()
    if not html_path.is_file():
        raise FileNotFoundError(html_path)
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    args.receipt.parent.mkdir(parents=True, exist_ok=True)

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        desktop_page = browser.new_page(viewport={"width": 1440, "height": 1100}, device_scale_factor=1)
        desktop = inspect_page(
            desktop_page,
            html_path.as_uri(),
            args.evidence_dir / "desktop.png",
        )
        desktop_page.emulate_media(media="print")
        pdf_path = args.evidence_dir / "print.pdf"
        desktop_page.pdf(path=str(pdf_path), format="A4", print_background=True)
        mobile_page = browser.new_page(
            viewport={"width": 390, "height": 844},
            device_scale_factor=1,
        )
        mobile = inspect_page(
            mobile_page,
            html_path.as_uri(),
            args.evidence_dir / "mobile.png",
        )
        browser.close()

    receipt = {
        "schema_name": "longlineage.static_report_browser_qa",
        "schema_version": "1.0.0",
        "scope": "PRESENTATION_ONLY_NOT_P8_RELEASE_EVIDENCE",
        "html": {
            "path": str(html_path),
            "size_bytes": html_path.stat().st_size,
            "sha256": sha256(html_path),
        },
        "desktop": desktop,
        "mobile": mobile,
        "print_pdf": {
            "path": str(pdf_path.resolve()),
            "size_bytes": pdf_path.stat().st_size,
            "sha256": sha256(pdf_path),
        },
        "pass": desktop["pass"] and mobile["pass"] and pdf_path.stat().st_size > 0,
    }
    args.receipt.write_text(
        json.dumps(receipt, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(receipt, ensure_ascii=False, sort_keys=True))
    return 0 if receipt["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
