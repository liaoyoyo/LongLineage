#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: run_regional_compat.sh \
  --binary ABS_PATH \
  --validator ABS_PATH \
  --repo-root ABS_PATH \
  --manifest ABS_PATH \
  --dataset-id ID \
  --run-id ID \
  --output-dir ABS_PATH \
  [--workers N] [--max-load N]

Runs one governed C++ PYTHON_V2_DESCRIPTIVE_REGIONAL dataset, including full
physical SHA-256 input verification, and then invokes the independent
fail-closed validator. Producer and validator logs are written beside the new
bundle. Truth-bearing inputs and persisted tagged BAMs are rejected upstream.
USAGE
}

binary=""
validator=""
repo_root=""
manifest=""
dataset_id=""
run_id=""
output_dir=""
workers="24"
max_load="60"

while (($# > 0)); do
    case "$1" in
        --binary) binary=${2:?}; shift 2 ;;
        --validator) validator=${2:?}; shift 2 ;;
        --repo-root) repo_root=${2:?}; shift 2 ;;
        --manifest) manifest=${2:?}; shift 2 ;;
        --dataset-id) dataset_id=${2:?}; shift 2 ;;
        --run-id) run_id=${2:?}; shift 2 ;;
        --output-dir) output_dir=${2:?}; shift 2 ;;
        --workers) workers=${2:?}; shift 2 ;;
        --max-load) max_load=${2:?}; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for required in binary validator repo_root manifest dataset_id run_id output_dir; do
    if [[ -z ${!required} ]]; then
        echo "ERROR: --${required//_/-} is required" >&2
        exit 2
    fi
done
for path_value in "$binary" "$validator" "$repo_root" "$manifest" "$output_dir"; do
    if [[ ${path_value:0:1} != / ]]; then
        echo "ERROR: all paths must be absolute: $path_value" >&2
        exit 2
    fi
done
if [[ ! -x $binary || ! -x $validator || ! -d $repo_root || ! -f $manifest ]]; then
    echo "ERROR: binary, validator, repository or manifest precondition failed" >&2
    exit 3
fi
binary=$(realpath "$binary")
validator=$(realpath "$validator")
repo_root=$(realpath "$repo_root")
manifest=$(realpath "$manifest")
if [[ -e $output_dir ]]; then
    echo "ERROR: output directory already exists: $output_dir" >&2
    exit 3
fi
if [[ ! $workers =~ ^[1-9][0-9]*$ ]] || ((workers > 64)); then
    echo "ERROR: workers must be an integer from 1 through 64" >&2
    exit 2
fi
if [[ ! $max_load =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "ERROR: max-load must be a nonnegative number" >&2
    exit 2
fi

manifest_run_id=$(jq -er '.run_id | select(type == "string" and length > 0)' "$manifest")
manifest_output_root=$(jq -er '.output_root | select(type == "string" and startswith("/"))' "$manifest")
manifest_workers=$(jq -er '.runtime.compute_workers | select(type == "number")' "$manifest")
expected_run_id="${manifest_run_id}-${dataset_id}"
expected_output_dir="${manifest_output_root}/${dataset_id}"
if [[ $run_id != "$expected_run_id" || $output_dir != "$expected_output_dir" ||
      $workers != "$manifest_workers" ]]; then
    echo "ERROR: run-id/output-dir/workers differ from the deterministic manifest mapping" >&2
    echo "EXPECTED_RUN_ID=$expected_run_id" >&2
    echo "EXPECTED_OUTPUT_DIR=$expected_output_dir" >&2
    echo "EXPECTED_WORKERS=$manifest_workers" >&2
    exit 3
fi

read -r load_one _ </proc/loadavg
if ! awk -v observed="$load_one" -v ceiling="$max_load" 'BEGIN { exit !(observed <= ceiling) }'; then
    echo "ERROR: RESOURCE_GATE load1=$load_one exceeds max_load=$max_load; no science launched" >&2
    exit 4
fi

mkdir -p "$(dirname "$output_dir")"
run_log="${output_dir}.run.log"
producer_stdout="${output_dir}.producer.stdout.log"
producer_stderr="${output_dir}.producer.stderr.log"
producer_time="${output_dir}.producer.time.txt"
validator_stdout="${output_dir}.validator.stdout.json"
validator_stderr="${output_dir}.validator.stderr.log"
validator_time="${output_dir}.validator.time.txt"
for log_path in "$run_log" "$producer_stdout" "$producer_stderr" "$producer_time" \
    "$validator_stdout" "$validator_stderr" "$validator_time"; do
    if [[ -e $log_path ]]; then
        echo "ERROR: sibling log path already exists: $log_path" >&2
        exit 3
    fi
done

record() {
    printf '%s\n' "$*" | tee -a "$run_log"
}

run_started_at=$(date --iso-8601=seconds)
run_started_epoch=$(date +%s)
record "RUN_STARTED_AT=$run_started_at"
record "INPUT_REPOSITORY_ROOT=$repo_root"
record "INPUT_MANIFEST=$manifest"
record "INPUT_DATASET_ID=$dataset_id"
record "RESOURCE_LOAD1=$load_one"
record "PRODUCER_COMMAND=/usr/bin/time -v -o $producer_time $binary run --repo-root $repo_root --manifest $manifest --dataset-id $dataset_id --run-id $run_id --output-dir $output_dir --workers $workers"

set +e
/usr/bin/time -v -o "$producer_time" \
    "$binary" run \
    --repo-root "$repo_root" \
    --manifest "$manifest" \
    --dataset-id "$dataset_id" \
    --run-id "$run_id" \
    --output-dir "$output_dir" \
    --workers "$workers" \
    > >(tee "$producer_stdout") \
    2> >(tee "$producer_stderr" >&2)
producer_status=$?
wait
set -e
record "PRODUCER_EXIT_CODE=$producer_status"
if ((producer_status != 0)); then
    record "RUN_STATE=PRODUCER_FAILED"
    exit "$producer_status"
fi

record "VALIDATOR_COMMAND=/usr/bin/time -v -o $validator_time $validator --repo-root $repo_root --bundle $output_dir"
set +e
/usr/bin/time -v -o "$validator_time" \
    "$validator" --repo-root "$repo_root" --bundle "$output_dir" \
    > >(tee "$validator_stdout") \
    2> >(tee "$validator_stderr" >&2)
validator_status=$?
wait
set -e
record "VALIDATOR_EXIT_CODE=$validator_status"
if ((validator_status != 0)); then
    record "RUN_STATE=VALIDATOR_FAILED"
    exit "$validator_status"
fi

test -f "$output_dir/FROZEN"
run_finished_at=$(date --iso-8601=seconds)
run_finished_epoch=$(date +%s)
record "RUN_FINISHED_AT=$run_finished_at"
record "END_TO_END_WALL_SECONDS=$((run_finished_epoch - run_started_epoch))"
record "RUN_STATE=VALIDATED_FROZEN"
record "OUTPUT_DIRECTORY=$output_dir"
record "OUTPUT_SUMMARY=$output_dir/summary.json"
record "OUTPUT_VALIDATION_RECEIPT=$output_dir/validation_receipt.json"
record "OUTPUT_FROZEN=$output_dir/FROZEN"
record "OUTPUT_RUN_LOG=$run_log"
record "OUTPUT_PRODUCER_STDOUT=$producer_stdout"
record "OUTPUT_PRODUCER_STDERR=$producer_stderr"
record "OUTPUT_PRODUCER_TIME=$producer_time"
record "OUTPUT_VALIDATOR_STDOUT=$validator_stdout"
record "OUTPUT_VALIDATOR_STDERR=$validator_stderr"
record "OUTPUT_VALIDATOR_TIME=$validator_time"
