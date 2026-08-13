#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: run_all_regional_compat.sh \
  --binary ABS_PATH \
  --validator ABS_PATH \
  --repo-root ABS_PATH \
  --manifest ABS_PATH \
  --output-root ABS_PATH \
  --run-prefix ID \
  [--workers N] [--max-load N] [--resume]

Runs the exact seven-dataset authority sequentially. Each dataset is frozen by
the independent validator before the next dataset starts. This wrapper never
publishes a cohort-level completed claim on partial execution. --resume skips
only sample directories that already contain a FROZEN marker; an incomplete
existing sample is rejected and preserved for audit.
USAGE
}

binary=""
validator=""
repo_root=""
manifest=""
output_root=""
run_prefix=""
workers="24"
max_load="60"
resume="false"

while (($# > 0)); do
    case "$1" in
        --binary) binary=${2:?}; shift 2 ;;
        --validator) validator=${2:?}; shift 2 ;;
        --repo-root) repo_root=${2:?}; shift 2 ;;
        --manifest) manifest=${2:?}; shift 2 ;;
        --output-root) output_root=${2:?}; shift 2 ;;
        --run-prefix) run_prefix=${2:?}; shift 2 ;;
        --workers) workers=${2:?}; shift 2 ;;
        --max-load) max_load=${2:?}; shift 2 ;;
        --resume) resume="true"; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for required in binary validator repo_root manifest output_root run_prefix; do
    if [[ -z ${!required} ]]; then
        echo "ERROR: --${required//_/-} is required" >&2
        exit 2
    fi
done
for path_value in "$binary" "$validator" "$repo_root" "$manifest" "$output_root"; do
    if [[ ${path_value:0:1} != / ]]; then
        echo "ERROR: producer, validator, repository, manifest and output-root paths must be absolute" >&2
        exit 3
    fi
done
if [[ ! -x $binary || ! -x $validator || ! -d $repo_root || ! -f $manifest ]]; then
    echo "ERROR: producer, validator, repository or source manifest precondition failed" >&2
    exit 3
fi
binary=$(realpath "$binary")
validator=$(realpath "$validator")
repo_root=$(realpath "$repo_root")
manifest=$(realpath "$manifest")
manifest_run_id=$(jq -er '.run_id | select(type == "string" and length > 0)' "$manifest")
manifest_output_root=$(jq -er '.output_root | select(type == "string" and startswith("/"))' "$manifest")
manifest_workers=$(jq -er '.runtime.compute_workers | select(type == "number")' "$manifest")
manifest_sha256_before=$(sha256sum "$manifest" | awk '{print $1}')
manifest_identity_before=$(stat -Lc '%d:%i:%s:%Y:%Z' "$manifest")
producer_sha256_before=$(sha256sum "$binary" | awk '{print $1}')
validator_sha256_before=$(sha256sum "$validator" | awk '{print $1}')
if [[ $run_prefix != "$manifest_run_id" || $output_root != "$manifest_output_root" ||
      $workers != "$manifest_workers" ]]; then
    echo "ERROR: run-prefix/output-root/workers differ from the source manifest contract" >&2
    echo "EXPECTED_RUN_PREFIX=$manifest_run_id" >&2
    echo "EXPECTED_OUTPUT_ROOT=$manifest_output_root" >&2
    echo "EXPECTED_WORKERS=$manifest_workers" >&2
    exit 3
fi
if [[ -e $output_root && $resume != true ]]; then
    echo "ERROR: output-root already exists; use --resume only for an audited partial run" >&2
    exit 3
fi
if [[ -e $output_root && ! -d $output_root ]]; then
    echo "ERROR: existing output-root is not a directory" >&2
    exit 3
fi

batch_log="${output_root}.batch.run.log"
if [[ -e $batch_log && $resume != true ]]; then
    echo "ERROR: batch log already exists: $batch_log" >&2
    exit 3
fi
mkdir -p "$(dirname "$output_root")"
record() {
    printf '%s\n' "$*" | tee -a "$batch_log"
}
batch_started_at=$(date --iso-8601=seconds)
batch_started_epoch=$(date +%s)
record "BATCH_STARTED_AT=$batch_started_at"
record "INPUT_REPOSITORY_ROOT=$repo_root"
record "INPUT_MANIFEST=$manifest"
record "INPUT_MANIFEST_SHA256=$manifest_sha256_before"
record "PRODUCER_EXECUTABLE=$binary"
record "PRODUCER_EXECUTABLE_SHA256=$producer_sha256_before"
record "VALIDATOR_EXECUTABLE=$validator"
record "VALIDATOR_EXECUTABLE_SHA256=$validator_sha256_before"
record "INPUT_DATASET_ORDER=HCC1395,HCC1395_DORADO,COLO829,H1437,H2009,HCC1937,HCC1954"
record "RESOURCE_WORKERS_PER_DATASET=$workers"
record "RESOURCE_EXECUTION_MODE=SEQUENTIAL_DATASETS"
record "OUTPUT_ROOT=$output_root"

mkdir -p "$output_root"
datasets=(HCC1395 HCC1395_DORADO COLO829 H1437 H2009 HCC1937 HCC1954)
for dataset_id in "${datasets[@]}"; do
    sample_output="$output_root/$dataset_id"
    if [[ -e $sample_output ]]; then
        if [[ $resume == true && -f $sample_output/FROZEN ]]; then
            record "DATASET_${dataset_id}_RESUME_SKIP_FROZEN=$sample_output/FROZEN"
            continue
        fi
        echo "ERROR: existing non-frozen sample output is preserved: $sample_output" >&2
        exit 3
    fi
    record "DATASET_${dataset_id}_BEGIN_AT=$(date --iso-8601=seconds)"
    "$(dirname "$0")/run_regional_compat.sh" \
        --binary "$binary" \
        --validator "$validator" \
        --repo-root "$repo_root" \
        --manifest "$manifest" \
        --dataset-id "$dataset_id" \
        --run-id "${run_prefix}-${dataset_id}" \
        --output-dir "$sample_output" \
        --workers "$workers" \
        --max-load "$max_load"
    record "DATASET_${dataset_id}_FROZEN_AT=$(date --iso-8601=seconds)"
    record "DATASET_${dataset_id}_FROZEN_PATH=$sample_output/FROZEN"
done

for dataset_id in "${datasets[@]}"; do
    test -f "$output_root/$dataset_id/FROZEN"
done
if [[ $(sha256sum "$binary" | awk '{print $1}') != "$producer_sha256_before" ||
      $(sha256sum "$validator" | awk '{print $1}') != "$validator_sha256_before" ]]; then
    echo "ERROR: producer or validator executable changed during the batch" >&2
    exit 4
fi
if [[ $(sha256sum "$manifest" | awk '{print $1}') != "$manifest_sha256_before" ||
      $(stat -Lc '%d:%i:%s:%Y:%Z' "$manifest") != "$manifest_identity_before" ]]; then
    echo "ERROR: source manifest bytes or filesystem identity changed during the batch" >&2
    exit 4
fi
batch_finished_at=$(date --iso-8601=seconds)
batch_finished_epoch=$(date +%s)
record "BATCH_FINISHED_AT=$batch_finished_at"
record "COHORT_ELAPSED_WALL_SECONDS=$((batch_finished_epoch - batch_started_epoch))"
record "ALL_DATASET_BUNDLES_FROZEN=7"
record "EXECUTABLES_STABLE=1"
record "SOURCE_MANIFEST_STABLE=1"
record "OUTPUT_BATCH_LOG=$batch_log"
