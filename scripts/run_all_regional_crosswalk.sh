#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: run_all_regional_crosswalk.sh \
  --binary ABS_PATH \
  --repo-root ABS_PATH \
  --bundle-root ABS_PATH \
  --python-root ABS_PATH \
  --output-dir ABS_PATH \
  --cohort-output ABS_PATH

Runs the C++ exact region/unit/pattern crosswalk for the frozen seven-dataset
cohort. Every bundle must already be independently validated and FROZEN.
USAGE
}

binary=""
repo_root=""
bundle_root=""
python_root=""
output_dir=""
cohort_output=""
while (($# > 0)); do
    case "$1" in
        --binary) binary=${2:?}; shift 2 ;;
        --repo-root) repo_root=${2:?}; shift 2 ;;
        --bundle-root) bundle_root=${2:?}; shift 2 ;;
        --python-root) python_root=${2:?}; shift 2 ;;
        --output-dir) output_dir=${2:?}; shift 2 ;;
        --cohort-output) cohort_output=${2:?}; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for required in binary repo_root bundle_root python_root output_dir cohort_output; do
    if [[ -z ${!required} ]]; then
        echo "ERROR: --${required//_/-} is required" >&2
        exit 2
    fi
    value=${!required}
    if [[ ${value:0:1} != / ]]; then
        echo "ERROR: all paths must be absolute: $value" >&2
        exit 2
    fi
done
if [[ ! -x $binary || ! -d $repo_root || ! -d $bundle_root || ! -d $python_root || -e $output_dir ||
      -e $cohort_output || -e ${output_dir}.batch.run.log ]]; then
    echo "ERROR: binary/input roots must exist and output directory/receipt must be new" >&2
    exit 3
fi

binary=$(realpath "$binary")
repo_root=$(realpath "$repo_root")
bundle_root=$(realpath "$bundle_root")
python_root=$(realpath "$python_root")
binary_sha256_before=$(sha256sum "$binary" | awk '{print $1}')
batch_log="${output_dir}.batch.run.log"
batch_started_at=$(date --iso-8601=seconds)
batch_started_epoch=$(date +%s)
mkdir -p "$(dirname "$output_dir")" "$(dirname "$cohort_output")"
record() {
    printf '%s\n' "$*" | tee -a "$batch_log"
}
record "CROSSWALK_BATCH_STARTED_AT=$batch_started_at"
record "INPUT_REPOSITORY_ROOT=$repo_root"
record "INPUT_BUNDLE_ROOT=$bundle_root"
record "INPUT_PYTHON_ROOT=$python_root"
record "COMPAT_EXECUTABLE=$binary"
record "COMPAT_EXECUTABLE_SHA256=$binary_sha256_before"
record "OUTPUT_DIRECTORY=$output_dir"
record "OUTPUT_COHORT_RECEIPT=$cohort_output"

mkdir -p "$output_dir"
datasets=(HCC1395 HCC1395_DORADO COLO829 H1437 H2009 HCC1937 HCC1954)
for dataset_id in "${datasets[@]}"; do
    bundle="$bundle_root/$dataset_id"
    python_manifest="$python_root/samples/$dataset_id/output_manifest.json"
    output="$output_dir/$dataset_id.crosswalk.json"
    if [[ ! -f $bundle/FROZEN || ! -f $python_manifest ]]; then
        echo "ERROR: frozen C++ bundle or Python manifest missing for $dataset_id" >&2
        exit 3
    fi
    record "DATASET_${dataset_id}_BEGIN_AT=$(date --iso-8601=seconds)"
    record "DATASET_${dataset_id}_CPP_BUNDLE=$bundle"
    record "DATASET_${dataset_id}_PYTHON_MANIFEST=$python_manifest"
    record "DATASET_${dataset_id}_COMMAND=$binary crosswalk --repo-root $repo_root --bundle $bundle --python-manifest $python_manifest --output $output"
    "$binary" crosswalk \
        --repo-root "$repo_root" \
        --bundle "$bundle" \
        --python-manifest "$python_manifest" \
        --output "$output"
    jq -e '
      .all_exact == true and
      .crosswalks.regions.mismatches == 0 and
      .crosswalks.units.mismatches == 0 and
      .crosswalks.patterns.mismatches == 0
    ' "$output" >/dev/null
    record "DATASET_${dataset_id}_EXACT_AT=$(date --iso-8601=seconds)"
    record "DATASET_${dataset_id}_OUTPUT=$output"
    record "DATASET_${dataset_id}_OUTPUT_SHA256=$(sha256sum "$output" | awk '{print $1}')"
done

record "ALL_DATASET_CROSSWALKS_EXACT=7"
record "COHORT_COMMAND=$binary cohort --bundle-root $bundle_root --crosswalk-dir $output_dir --output $cohort_output"
"$binary" cohort \
    --bundle-root "$bundle_root" \
    --crosswalk-dir "$output_dir" \
    --output "$cohort_output"
jq -e '
  .state == "VALIDATED_FROZEN_BUNDLES_WITH_EXACT_CROSSWALK_RECEIPTS_AGGREGATED" and
  .dataset_count == 7 and
  .all_sources_validated_frozen == true and
  .all_crosswalk_receipts_exact == true and
  .crosswalk_receipts_independently_frozen == false
' "$cohort_output" >/dev/null
if [[ $(sha256sum "$binary" | awk '{print $1}') != "$binary_sha256_before" ]]; then
    echo "ERROR: compatibility executable changed during crosswalk/cohort aggregation" >&2
    exit 4
fi
batch_finished_at=$(date --iso-8601=seconds)
batch_finished_epoch=$(date +%s)
record "OUTPUT_COHORT_SHA256=$(sha256sum "$cohort_output" | awk '{print $1}')"
record "CROSSWALK_BATCH_FINISHED_AT=$batch_finished_at"
record "CROSSWALK_ELAPSED_WALL_SECONDS=$((batch_finished_epoch - batch_started_epoch))"
record "EXECUTABLE_STABLE=1"
record "OUTPUT_BATCH_LOG=$batch_log"
