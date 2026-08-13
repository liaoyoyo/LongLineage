#!/usr/bin/env node
/** Deliver a canonical portable report with a narrow full-bleed scrollbar fix. */

import { randomUUID } from "node:crypto";
import { mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { pathToFileURL } from "node:url";

function parseArguments(argv) {
    const values = {};
    for (let index = 0; index < argv.length; index += 2) {
        const key = argv[index];
        const value = argv[index + 1];
        if (!key?.startsWith("--") || !value || value.startsWith("--")) {
            throw new Error("Usage: deliver_portable_report.mjs --plugin-root DIR --input FILE --output FILE");
        }
        values[key.slice(2)] = value;
    }
    for (const key of ["plugin-root", "input", "output"]) {
        if (!values[key]) throw new Error(`missing --${key}`);
    }
    return values;
}

function applyPortablePresentationFixes(html) {
    const marker = "</head>";
    const occurrences = html.split(marker).length - 1;
    if (occurrences !== 1) throw new Error("portable HTML must contain exactly one </head>");
    const rootMarker = '<html lang="en" data-data-analytics-portable-artifact="true">';
    if (html.split(rootMarker).length - 1 !== 1) {
        throw new Error("portable HTML language root is not canonical");
    }
    const style = [
        '<style data-longlineage-portable-width-fix="true">',
        ".analytics-top-bar,.portable-page-header{",
        "width:100%!important;margin-right:0!important;margin-left:0!important",
        "}",
        ".partial-ribbon{padding:8px 16px;background:#6b310d;color:#fff7ed;",
        "font:700 12px/1.4 ui-sans-serif,system-ui,sans-serif;letter-spacing:.04em;",
        "text-align:center;position:relative;z-index:20}",
        "@media print{.partial-ribbon{print-color-adjust:exact;-webkit-print-color-adjust:exact}}",
        "</style>",
    ].join("");
    const bodyPattern = /<body([^>]*)>/;
    const bodyMatches = html.match(/<body[^>]*>/g) ?? [];
    if (bodyMatches.length !== 1) {
        throw new Error("portable HTML body is not canonical");
    }
    return html
        .replace(
            rootMarker,
            '<html lang="zh-Hant" data-partial="true" data-data-analytics-portable-artifact="true">',
        )
        .replace(marker, `${style}\n${marker}`)
        .replace(
            bodyPattern,
            '$&<div class="partial-ribbon" role="status">PARTIAL · DESCRIPTIVE COMPATIBILITY · NON-PRODUCTION · NOT P8 RELEASE EVIDENCE</div>',
        );
}

async function main() {
    const args = parseArguments(process.argv.slice(2));
    const pluginRoot = resolve(args["plugin-root"]);
    const inputPath = resolve(args.input);
    const outputPath = resolve(args.output);
    const scriptsRoot = resolve(pluginRoot, "skills/build-report/scripts");
    const buildModule = await import(pathToFileURL(resolve(scriptsRoot, "build_portable_artifact.mjs")));
    const extractModule = await import(pathToFileURL(resolve(scriptsRoot, "extract_portable_chart_svgs.mjs")));
    const verifyModule = await import(pathToFileURL(resolve(scriptsRoot, "verify_portable_artifact.mjs")));

    const artifact = JSON.parse(readFileSync(inputPath, "utf8"));
    mkdirSync(dirname(outputPath), { recursive: true });
    const temporaryPath = `${outputPath}.tmp-${process.pid}-${randomUUID()}.html`;
    try {
        let html = buildModule.buildPortableArtifact(artifact);
        writeFileSync(temporaryPath, html, "utf8");
        const staticCharts = await extractModule.extractPortableChartSvgs({ htmlPath: temporaryPath });
        html = applyPortablePresentationFixes(
            buildModule.buildPortableArtifact(artifact, { staticCharts }),
        );
        writeFileSync(temporaryPath, html, "utf8");
        const verification = await verifyModule.verifyPortableArtifact({
            artifactPath: inputPath,
            htmlPath: temporaryPath,
        });
        renameSync(temporaryPath, outputPath);
        process.stdout.write(`${JSON.stringify({
            ok: true,
            html: outputPath,
            language: "zh-Hant",
            widthFix: "scrollbar-safe-full-bleed",
            verification,
        })}\n`);
    } finally {
        rmSync(temporaryPath, { force: true });
    }
}

main().catch((error) => {
    const verification = error?.verificationResult;
    process.stderr.write(`${JSON.stringify({
        ok: false,
        code: verification?.code ?? error?.code ?? "delivery_failed",
        error: verification?.error ?? error?.message ?? String(error),
    })}\n`);
    process.exitCode = 1;
});
