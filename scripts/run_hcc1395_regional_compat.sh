#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: run_hcc1395_regional_compat.sh \
  --binary ABS_PATH \
  --validator ABS_PATH \
  --repo-root ABS_PATH \
  --manifest ABS_PATH \
  --run-id ID \
  --output-dir ABS_PATH \
  [--workers N]

Backward-compatible HCC1395 wrapper around run_regional_compat.sh. New runs
also require repository authority verification and full physical SHA replay.
USAGE
}

binary=""
validator=""
repo_root=""
manifest=""
run_id=""
output_dir=""
workers="24"

while (($# > 0)); do
    case "$1" in
        --binary) binary=${2:?}; shift 2 ;;
        --validator) validator=${2:?}; shift 2 ;;
        --repo-root) repo_root=${2:?}; shift 2 ;;
        --manifest) manifest=${2:?}; shift 2 ;;
        --run-id) run_id=${2:?}; shift 2 ;;
        --output-dir) output_dir=${2:?}; shift 2 ;;
        --workers) workers=${2:?}; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for required in binary validator repo_root manifest run_id output_dir; do
    if [[ -z ${!required} ]]; then
        echo "ERROR: --${required//_/-} is required" >&2
        exit 2
    fi
done
if [[ ${binary:0:1} != / || ${validator:0:1} != / || ${repo_root:0:1} != / ||
      ${manifest:0:1} != / || ${output_dir:0:1} != / ]]; then
    echo "ERROR: binary, validator, repo-root, manifest and output-dir must be absolute" >&2
    exit 2
fi
if [[ ! -x $binary || ! -x $validator || ! -d $repo_root || ! -f $manifest ]]; then
    echo "ERROR: executable or manifest precondition failed" >&2
    exit 3
fi
if [[ -e $output_dir ]]; then
    echo "ERROR: output directory already exists: $output_dir" >&2
    exit 3
fi
if [[ ! $workers =~ ^[1-9][0-9]*$ ]] || ((workers > 64)); then
    echo "ERROR: workers must be an integer from 1 through 64" >&2
    exit 2
fi

exec "$(dirname "$0")/run_regional_compat.sh" \
    --binary "$binary" \
    --validator "$validator" \
    --repo-root "$repo_root" \
    --manifest "$manifest" \
    --dataset-id HCC1395 \
    --run-id "$run_id" \
    --output-dir "$output_dir" \
    --workers "$workers"
