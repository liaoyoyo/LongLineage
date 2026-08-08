#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# 統一 driver — 單樣本從 partition 到 tagged BAM
#
# 用法:
#   bash scripts/run_sample.sh --sample SAMPLE --out-root DIR \
#        --partition-root DIR --topology FILE --sidecar FILE --in-bam FILE \
#        [--chroms "chr1 chr2 ..."] [--threads 8] [--skip-bam] [--skip-existing]
#        [--split-by-chrom]
#
# BAM 輸出：**預設合併成單一 <SAMPLE>.lineage_tagged.bam**，因為那才是 IGV /
# 一般下游工具期望的形式（22 個檔要載 22 條 track）。逐染色體的中間檔在合併後
# 刪除。若下游是逐染色體平行處理，加 --split-by-chrom 保留分檔、不合併。
#
# 依專案慣例，所有站點資料路徑一律由 CLI 傳入，不在本檔寫死
# （scripts/ci/check_repo_hygiene.sh 會阻擋寫死的絕對路徑）。
# 四個資料路徑可改用環境變數提供：
#   LL_PARTITION_ROOT / LL_TOPOLOGY / LL_SIDECAR / LL_IN_BAM
#
# 前提: /usr/bin/cmake --build <build> 已建好 longlineage-{lineage-paths,read-assign,tag-bam}

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# LongLineage build tree; override with LONGLINEAGE_BUILD when using another config.
BUILD_DIR="${LONGLINEAGE_BUILD:-${REPO_ROOT}/build_lineage_migrate}"
BIN="${BUILD_DIR}/bin"
export LD_LIBRARY_PATH=/usr/local/lib:${LD_LIBRARY_PATH:-}

SAMPLE=""; OUT_ROOT=""; THREADS=8; SKIP_BAM=0; SKIP_EXISTING=0
SPLIT_BY_CHROM=0     # 預設合併成單一 BAM；加 --split-by-chrom 才保留逐染色體
PARTITION_ROOT="${LL_PARTITION_ROOT:-}"
TOPOLOGY="${LL_TOPOLOGY:-}"
SIDECAR="${LL_SIDECAR:-}"
RAW_BAM="${LL_IN_BAM:-}"
CHROMS="chr1 chr2 chr3 chr4 chr5 chr6 chr7 chr8 chr9 chr10 chr11 chr12 chr13 chr14 chr15 chr16 chr17 chr18 chr19 chr20 chr21 chr22"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sample) SAMPLE="$2"; shift 2;;
        --out-root) OUT_ROOT="$2"; shift 2;;
        --partition-root) PARTITION_ROOT="$2"; shift 2;;
        --topology) TOPOLOGY="$2"; shift 2;;
        --sidecar) SIDECAR="$2"; shift 2;;
        --chroms) CHROMS="$2"; shift 2;;
        --threads) THREADS="$2"; shift 2;;
        --skip-bam) SKIP_BAM=1; shift;;
        --split-by-chrom) SPLIT_BY_CHROM=1; shift;;
        --in-bam) RAW_BAM="$2"; shift 2;;
        --skip-existing) SKIP_EXISTING=1; shift;;
        *) echo "unknown: $1" >&2; exit 2;;
    esac
done
[[ -n "$SAMPLE" && -n "$OUT_ROOT" ]] || { echo "need --sample and --out-root" >&2; exit 2; }

missing_arg() { echo "need $1 (or env $2)" >&2; exit 2; }
[[ -n "$PARTITION_ROOT" ]] || missing_arg --partition-root LL_PARTITION_ROOT
[[ -n "$TOPOLOGY" ]]       || missing_arg --topology       LL_TOPOLOGY
[[ -n "$SIDECAR" ]]        || missing_arg --sidecar        LL_SIDECAR
[[ -n "$RAW_BAM" ]]        || missing_arg --in-bam         LL_IN_BAM

# 🔴 --partition-root 必須指向 production partition，不可用 pilot
#    （unit_id 雜湊不同，對不上 topology；見 KNOWN_ISSUES E10a）
[[ -d "$PARTITION_ROOT" ]] || { echo "MISSING dir: $PARTITION_ROOT" >&2; exit 2; }
for f in "$TOPOLOGY" "$SIDECAR" "$RAW_BAM"; do
    [[ -f "$f" ]] || { echo "MISSING: $f" >&2; exit 2; }
done

mkdir -p "$OUT_ROOT"/{paths,assign,bam,logs}
STAGE_LOG="$OUT_ROOT/logs/stages.tsv"
[[ -f "$STAGE_LOG" ]] || printf 'utc\tstage\tchrom\tstatus\tseconds\tdetail\n' > "$STAGE_LOG"

log_stage() {
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -u +%FT%TZ)" "$1" "$2" "$3" "$4" "${5:-}" >> "$STAGE_LOG"
}

echo "sample     : $SAMPLE"
echo "out-root   : $OUT_ROOT"
echo "chroms     : $(echo $CHROMS | wc -w)"
echo "threads    : $THREADS"
echo

# ── STAGE 1: lineage_paths（全染色體一次）─────────────────────────
echo "[1/4] lineage_paths"
t0=$SECONDS
"$BIN/longlineage-lineage-paths" --topology "$TOPOLOGY" \
    --output "$OUT_ROOT/paths/${SAMPLE}.unit_lineage_paths.tsv.gz" \
    --receipt "$OUT_ROOT/paths/${SAMPLE}.unit_lineage_paths.receipt.json" \
    > "$OUT_ROOT/logs/lineage_paths.log" 2>&1
log_stage lineage_paths ALL OK $((SECONDS-t0))
tail -3 "$OUT_ROOT/logs/lineage_paths.log" | sed 's/^/      /'

# ── STAGE 2: read_assign（逐染色體）───────────────────────────────
echo "[2/4] read_assign"
for c in $CHROMS; do
    d="$PARTITION_ROOT/$c"
    [[ -d "$d" ]] || { echo "      $c SKIP (no partition)"; log_stage read_assign "$c" SKIP 0 "no partition dir"; continue; }
    t0=$SECONDS
    if "$BIN/longlineage-read-assign" --chrom-dir "$d" --sample "$SAMPLE" --chrom "$c" \
        --output "$OUT_ROOT/assign/${SAMPLE}.${c}.read_lineage_assignments.tsv.gz" \
        --receipt "$OUT_ROOT/assign/${SAMPLE}.${c}.read_assign.receipt.json" \
        > "$OUT_ROOT/logs/read_assign.${c}.log" 2>&1; then
        rows=$(grep -m1 'rows written' "$OUT_ROOT/logs/read_assign.${c}.log" | tr -dc '0-9')
        echo "      $c OK  rows=$rows  ($((SECONDS-t0))s)"
        log_stage read_assign "$c" OK $((SECONDS-t0)) "rows=$rows"
    else
        echo "      $c FAIL"; log_stage read_assign "$c" FAIL $((SECONDS-t0)); exit 1
    fi
done

# 合併成單一 assignments（tag_bam 吃一份）
echo "      merging ..."
MERGED="$OUT_ROOT/assign/${SAMPLE}.all.read_lineage_assignments.tsv.gz"
first=1
for c in $CHROMS; do
    f="$OUT_ROOT/assign/${SAMPLE}.${c}.read_lineage_assignments.tsv.gz"
    [[ -f "$f" ]] || continue
    if [[ $first -eq 1 ]]; then zcat "$f"; first=0; else zcat "$f" | tail -n +2; fi
done | gzip -c > "$MERGED"
echo "      merged: $(zcat "$MERGED" | wc -l) rows"

# ── STAGE 3: tag_bam ──────────────────────────────────────────────
if [[ $SKIP_BAM -eq 1 ]]; then
    echo "[3/4] tag_bam SKIPPED (--skip-bam)"
    exit 0
fi
echo "[3/4] tag_bam"
for c in $CHROMS; do
    t0=$SECONDS
    out="$OUT_ROOT/bam/${SAMPLE}.${c}.lineage_tagged.bam"
    rcp="$OUT_ROOT/bam/${SAMPLE}.${c}.tag_bam.receipt.json"
    if [[ $SKIP_EXISTING -eq 1 && -f "$out" && -f "$rcp" ]]; then
        echo "      $c SKIP (already done)"; continue
    fi
    if "$BIN/longlineage-tag-bam" --in-bam "$RAW_BAM" --sidecar "$SIDECAR" \
        --assignments "$MERGED" \
        --lineage-paths "$OUT_ROOT/paths/${SAMPLE}.unit_lineage_paths.tsv.gz" \
        --topology "$TOPOLOGY" --region "$c" --threads "$THREADS" \
        --out-bam "$out" --receipt "$OUT_ROOT/bam/${SAMPLE}.${c}.tag_bam.receipt.json" \
        > "$OUT_ROOT/logs/tag_bam.${c}.log" 2>&1; then
        sz=$(stat -c%s "$out")
        echo "      $c OK  $(numfmt --to=iec "$sz")  ($((SECONDS-t0))s)"
        log_stage tag_bam "$c" OK $((SECONDS-t0)) "bytes=$sz"
    else
        echo "      $c FAIL"; log_stage tag_bam "$c" FAIL $((SECONDS-t0)); exit 1
    fi
done

# ── STAGE 4: 合併成單一 BAM ───────────────────────────────────────
# 逐染色體是為了平行化的**中間產物**，不是給人用的形式。IGV 與多數下游
# 期望單一座標排序 BAM，所以預設合併；22 個檔的 header 完全一致（同一份
# @SQ 195 條），merge 是無損的。
if [[ $SPLIT_BY_CHROM -eq 1 ]]; then
    echo
    echo "[4/4] 保留逐染色體分檔（--split-by-chrom）"
else
    echo
    echo "[4/4] merge → 單一 BAM"
    t0=$SECONDS
    MERGED_BAM="$OUT_ROOT/bam/${SAMPLE}.lineage_tagged.bam"
    parts=()
    for c in $CHROMS; do
        f="$OUT_ROOT/bam/${SAMPLE}.${c}.lineage_tagged.bam"
        [[ -f "$f" ]] && parts+=("$f")
    done
    if [[ ${#parts[@]} -eq 0 ]]; then
        echo "      沒有可合併的分檔，略過"
    elif [[ ${#parts[@]} -eq 1 ]]; then
        mv "${parts[0]}" "$MERGED_BAM"
        [[ -f "${parts[0]}.bai" ]] && mv "${parts[0]}.bai" "$MERGED_BAM.bai"
        echo "      只有 1 個分檔，直接改名"
    else
        if samtools merge -@ "$THREADS" -f -o "$MERGED_BAM" "${parts[@]}" \
             > "$OUT_ROOT/logs/merge.log" 2>&1 \
           && samtools index -@ "$THREADS" "$MERGED_BAM" >> "$OUT_ROOT/logs/merge.log" 2>&1; then
            # 只有在 merge + index 都成功後才刪中間檔
            for f in "${parts[@]}"; do rm -f "$f" "$f.bai"; done
            sz=$(stat -c%s "$MERGED_BAM")
            echo "      OK  ${#parts[@]} 檔 → $(numfmt --to=iec "$sz")  ($((SECONDS-t0))s)"
            log_stage merge ALL OK $((SECONDS-t0)) "parts=${#parts[@]} bytes=$sz"
        else
            echo "      FAIL —— 分檔保留未刪，可用 --split-by-chrom 重跑或手動 merge"
            log_stage merge ALL FAIL $((SECONDS-t0))
            exit 1
        fi
    fi
    echo
    echo "  IGV 用法：載入 $MERGED_BAM 後"
    echo "    Group alignments by → tag → 輸入 lv（譜系）/ ls（可信度）/ HP（單倍型）"
    echo "    ⚠ HP 是字串 HP:Z（九態 1/1-1/2/2-1/3），IGV 內建 haplotype 模式只認 HP:i，"
    echo "      所以要走 tag 分組而非內建模式。"
fi

echo
echo "done. stage log: $STAGE_LOG"
