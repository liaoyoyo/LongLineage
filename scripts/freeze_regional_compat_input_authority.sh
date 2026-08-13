#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: freeze_regional_compat_input_authority.sh \
  --repo-root ABS_PATH \
  --input-paths ABS_PATH \
  --hash-root ABS_PATH \
  --output ABS_PATH

Builds the path-free seven-dataset regional input authority. Six raw-BAM
digests must already exist as sha256sum receipts under --hash-root; HCC1395 is
bound to the previously frozen full-content dataset-gate authority. All other
physical inputs are hashed and compared with their source authorities before
the new authority is atomically published. Private physical paths are read
from an external, untracked closed-shape map and are never stored in Git.
USAGE
}

repo_root=""
input_paths=""
hash_root=""
output=""
while (($# > 0)); do
    case "$1" in
        --repo-root) repo_root=${2:?}; shift 2 ;;
        --input-paths) input_paths=${2:?}; shift 2 ;;
        --hash-root) hash_root=${2:?}; shift 2 ;;
        --output) output=${2:?}; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for required in repo_root input_paths hash_root output; do
    if [[ -z ${!required} ]]; then
        echo "ERROR: --${required//_/-} is required" >&2
        exit 2
    fi
    if [[ ${!required:0:1} != / ]]; then
        echo "ERROR: --${required//_/-} must be absolute" >&2
        exit 2
    fi
done
if [[ ! -d $repo_root || ! -f $input_paths || -L $input_paths ||
      ! -d $hash_root || -e $output || -e ${output}.partial ]]; then
    echo "ERROR: repository/input path map/hash root must exist and output/partial must be new" >&2
    exit 3
fi

repo_root=$(realpath "$repo_root")
input_paths=$(realpath "$input_paths")
input_paths_sha256_before=$(sha256sum "$input_paths" | awk '{print $1}')
production_authority="$repo_root/oracle/production_input_authority.json"
hcc_authority="$repo_root/oracle/hcc1395_dataset_gate_input_authority.json"
for authority in "$production_authority" "$hcc_authority"; do
    [[ -f $authority ]] || { echo "ERROR: missing source authority: $authority" >&2; exit 3; }
done
production_authority_sha256_before=$(sha256sum "$production_authority" | awk '{print $1}')
hcc_authority_sha256_before=$(sha256sum "$hcc_authority" | awk '{print $1}')
jq -e '
  .schema_name == "longlineage.production_input_authority" and
  .schema_version == "1.0.0" and
  .profile_id == "RAW_ALL_PRODUCTION_V2_7_DATASET" and
  ([.dataset_order[]] == ["HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954"]) and
  ([.datasets[].dataset_id] == ["HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954"]) and
  ([.datasets[].dataset_order] == [0, 1, 2, 3, 4, 5, 6])
' "$production_authority" >/dev/null
jq -e '
  .schema_name == "longlineage.dataset_gate_input_authority" and
  .schema_version == "1.0.0" and
  .authority_profile == "HCC1395_DATASET_GATE" and
  .dataset_id == "HCC1395" and .dataset_order == 0 and
  ([.files[].role] == ["raw_bam", "raw_bam_index", "pass_biallelic_ssnv_vcf", "pass_biallelic_ssnv_vcf_index", "latest_hp_ps_sidecar", "latest_hp_ps_sidecar_index", "reference_fasta", "reference_fai"])
' "$hcc_authority" >/dev/null

jq -e '
  .schema_name == "longlineage.regional_compat_input_paths" and
  .schema_version == "1.0.0" and
  ((keys | sort) == (["datasets", "schema_name", "schema_version"] | sort)) and
  ([.datasets[].dataset_id] ==
    ["HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954"]) and
  ([.datasets[].dataset_order] == [0, 1, 2, 3, 4, 5, 6]) and
  all(.datasets[];
    ((keys | sort) == (["dataset_id", "dataset_order", "files"] | sort)) and
    ([.files[].role] ==
      ["raw_bam", "raw_bam_index", "pass_biallelic_ssnv_vcf",
       "pass_biallelic_ssnv_vcf_index", "latest_hp_ps_sidecar",
       "latest_hp_ps_sidecar_index", "reference_fasta", "reference_fai"]) and
    all(.files[];
      ((keys | sort) == (["path", "role"] | sort)) and
      (.path | type == "string" and startswith("/") and
       (ascii_downcase | contains("truth") | not)))))
' "$input_paths" >/dev/null

datasets=(HCC1395 HCC1395_DORADO COLO829 H1437 H2009 HCC1937 HCC1954)
roles=(raw_bam raw_bam_index pass_biallelic_ssnv_vcf pass_biallelic_ssnv_vcf_index \
       latest_hp_ps_sidecar latest_hp_ps_sidecar_index reference_fasta reference_fai)

input_path() {
    local dataset=$1
    local role=$2
    jq -er --arg dataset "$dataset" --arg role "$role" '
      first(.datasets[] | select(.dataset_id == $dataset) |
            .files[] | select(.role == $role) | .path)
    ' "$input_paths"
}

require_canonical_regular() {
    local path=$1
    [[ -f $path && ! -L $path ]] || {
        echo "ERROR: authority input is not a regular non-symlink file: $path" >&2
        return 3
    }
    local canonical
    canonical=$(realpath "$path")
    [[ $canonical == "$path" ]] || {
        echo "ERROR: authority input traverses a symbolic-link alias: $path -> $canonical" >&2
        return 3
    }
}

source_lock() {
    local dataset=$1
    local role=$2
    if [[ $dataset == HCC1395 ]]; then
        jq -r --arg role "$role" '.files[] | select(.role == $role) | [.size_bytes, .sha256] | @tsv' \
            "$hcc_authority"
        return
    fi
    case "$role" in
        pass_biallelic_ssnv_vcf|pass_biallelic_ssnv_vcf_index|latest_hp_ps_sidecar|latest_hp_ps_sidecar_index)
            jq -r --arg dataset "$dataset" --arg role "$role" \
                '.datasets[] | select(.dataset_id == $dataset) | .[$role] | [.size_bytes, .sha256] | @tsv' \
                "$production_authority"
            ;;
        reference_fasta|reference_fai)
            jq -r --arg role "$role" '.files[] | select(.role == $role) | [.size_bytes, .sha256] | @tsv' \
                "$hcc_authority"
            ;;
        *) return 2 ;;
    esac
}

emit_row() {
    local dataset=$1
    local order=$2
    local role=$3
    local size=$4
    local digest=$5
    [[ $size =~ ^[1-9][0-9]*$ && $digest =~ ^[0-9a-f]{64}$ ]] || {
        echo "ERROR: malformed authority row for $dataset/$role" >&2
        return 3
    }
    printf '%s\t%s\t%s\t%s\t%s\n' "$dataset" "$order" "$role" "$size" "$digest"
}

reference_fasta_path=$(input_path HCC1395 reference_fasta)
reference_fai_path=$(input_path HCC1395 reference_fai)
require_canonical_regular "$reference_fasta_path"
require_canonical_regular "$reference_fai_path"
reference_fasta_size=$(stat -Lc %s "$reference_fasta_path")
reference_fai_size=$(stat -Lc %s "$reference_fai_path")
reference_fasta_sha=$(sha256sum "$reference_fasta_path" | awk '{print $1}')
reference_fai_sha=$(sha256sum "$reference_fai_path" | awk '{print $1}')
IFS=$'\t' read -r expected_reference_fasta_size expected_reference_fasta_sha \
    < <(source_lock HCC1395 reference_fasta)
IFS=$'\t' read -r expected_reference_fai_size expected_reference_fai_sha \
    < <(source_lock HCC1395 reference_fai)
if [[ $reference_fasta_size != "$expected_reference_fasta_size" ||
      $reference_fasta_sha != "$expected_reference_fasta_sha" ||
      $reference_fai_size != "$expected_reference_fai_size" ||
      $reference_fai_sha != "$expected_reference_fai_sha" ]]; then
    echo "ERROR: reference FASTA/FAI differs from source authority" >&2
    exit 3
fi

emit_rows() {
    local dataset order role path expected_size expected_sha actual_size actual_sha receipt recorded_path
    for order in "${!datasets[@]}"; do
        dataset=${datasets[$order]}
        for role in "${roles[@]}"; do
            path=$(input_path "$dataset" "$role")
            require_canonical_regular "$path"
            actual_size=$(stat -Lc %s "$path")
            if [[ $role == reference_fasta ]]; then
                emit_row "$dataset" "$order" "$role" "$reference_fasta_size" "$reference_fasta_sha"
                continue
            fi
            if [[ $role == reference_fai ]]; then
                emit_row "$dataset" "$order" "$role" "$reference_fai_size" "$reference_fai_sha"
                continue
            fi
            if [[ $role == raw_bam ]]; then
                if [[ $dataset == HCC1395 ]]; then
                    IFS=$'\t' read -r expected_size expected_sha < <(source_lock "$dataset" "$role")
                    [[ $actual_size == "$expected_size" ]] || {
                        echo "ERROR: HCC1395 raw BAM size drift" >&2
                        return 3
                    }
                    emit_row "$dataset" "$order" "$role" "$actual_size" "$expected_sha"
                    continue
                fi
                receipt="$hash_root/$dataset.raw_bam.sha256.txt"
                [[ -f $receipt && $(wc -l <"$receipt") -eq 1 ]] || {
                    echo "ERROR: missing or malformed raw BAM hash receipt: $receipt" >&2
                    return 3
                }
                read -r actual_sha recorded_path <"$receipt"
                [[ $recorded_path == "$path" && $actual_sha =~ ^[0-9a-f]{64}$ ]] || {
                    echo "ERROR: raw BAM hash receipt path/digest mismatch: $dataset" >&2
                    return 3
                }
                emit_row "$dataset" "$order" "$role" "$actual_size" "$actual_sha"
                continue
            fi

            if [[ $role == raw_bam_index ]]; then
                actual_sha=$(sha256sum "$path" | awk '{print $1}')
                if [[ $dataset == HCC1395 ]]; then
                    IFS=$'\t' read -r expected_size expected_sha < <(source_lock "$dataset" "$role")
                    [[ $actual_size == "$expected_size" && $actual_sha == "$expected_sha" ]] || {
                        echo "ERROR: HCC1395 BAM index differs from source authority" >&2
                        return 3
                    }
                fi
                emit_row "$dataset" "$order" "$role" "$actual_size" "$actual_sha"
                continue
            fi

            IFS=$'\t' read -r expected_size expected_sha < <(source_lock "$dataset" "$role")
            actual_sha=$(sha256sum "$path" | awk '{print $1}')
            [[ $actual_size == "$expected_size" && $actual_sha == "$expected_sha" ]] || {
                echo "ERROR: source authority drift for $dataset/$role" >&2
                return 3
            }
            emit_row "$dataset" "$order" "$role" "$actual_size" "$actual_sha"
        done
    done
}

base_sha256=$production_authority_sha256_before
hcc_sha256=$hcc_authority_sha256_before
mkdir -p "$(dirname "$output")"
partial="${output}.partial"
emit_rows | jq -Rn \
    --arg base_sha256 "$base_sha256" \
    --arg hcc_sha256 "$hcc_sha256" '
    [inputs | select(length > 0) | split("\t") | {
      dataset_id: .[0], dataset_order: (.[1] | tonumber), role: .[2],
      size_bytes: (.[3] | tonumber), sha256: .[4]
    }] as $rows |
    {
      schema_name: "longlineage.regional_compat_all_datasets_input_authority",
      schema_version: "1.0.0",
      profile_id: "PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET",
      base_production_input_authority_sha256: $base_sha256,
      hcc1395_dataset_gate_input_authority_sha256: $hcc_sha256,
      constraints: {
        latest_tag_join: "EXACT_PROJECTION_NO_FALLBACK",
        truth_fields: 0,
        persisted_tagged_bam_allowed: false,
        all_physical_sha256_verified: true,
        private_source_paths_stored: false
      },
      datasets: [range(0; 7) as $order | {
        dataset_id: first($rows[] | select(.dataset_order == $order) | .dataset_id),
        dataset_order: $order,
        files: [$rows[] | select(.dataset_order == $order) |
          {role, size_bytes, sha256}]
      }]
    }
' >"$partial"

jq -e '
  .schema_name == "longlineage.regional_compat_all_datasets_input_authority" and
  .schema_version == "1.0.0" and
  .profile_id == "PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET" and
  ((keys | sort) == (["base_production_input_authority_sha256", "constraints", "datasets", "hcc1395_dataset_gate_input_authority_sha256", "profile_id", "schema_name", "schema_version"] | sort)) and
  (.base_production_input_authority_sha256 | test("^[0-9a-f]{64}$")) and
  (.hcc1395_dataset_gate_input_authority_sha256 | test("^[0-9a-f]{64}$")) and
  ((.constraints | keys | sort) == (["all_physical_sha256_verified", "latest_tag_join", "persisted_tagged_bam_allowed", "private_source_paths_stored", "truth_fields"] | sort)) and
  .constraints.latest_tag_join == "EXACT_PROJECTION_NO_FALLBACK" and
  .constraints.truth_fields == 0 and
  .constraints.persisted_tagged_bam_allowed == false and
  .constraints.all_physical_sha256_verified == true and
  .constraints.private_source_paths_stored == false and
  ([.datasets[].dataset_id] == ["HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954"]) and
  ([.datasets[].dataset_order] == [0, 1, 2, 3, 4, 5, 6]) and
  all(.datasets[];
    ((keys | sort) == (["dataset_id", "dataset_order", "files"] | sort)) and
    ([.files[].role] == ["raw_bam", "raw_bam_index", "pass_biallelic_ssnv_vcf", "pass_biallelic_ssnv_vcf_index", "latest_hp_ps_sidecar", "latest_hp_ps_sidecar_index", "reference_fasta", "reference_fai"]) and
    all(.files[];
      ((keys | sort) == (["role", "sha256", "size_bytes"] | sort)) and
      (.size_bytes | type == "number" and . > 0) and
      (.sha256 | test("^[0-9a-f]{64}$")))) and
  ([paths(scalars) as $path | getpath($path) | strings | ascii_downcase | contains("/big")] | any) == false
' "$partial" >/dev/null

if [[ $(sha256sum "$input_paths" | awk '{print $1}') != "$input_paths_sha256_before" ||
      $(sha256sum "$production_authority" | awk '{print $1}') != "$production_authority_sha256_before" ||
      $(sha256sum "$hcc_authority" | awk '{print $1}') != "$hcc_authority_sha256_before" ]]; then
    echo "ERROR: the private path map or a source authority changed while the regional authority was built" >&2
    exit 4
fi

mv "$partial" "$output"
echo "INPUT_REPOSITORY_ROOT=$repo_root"
echo "INPUT_PATH_MAP_SHA256=$input_paths_sha256_before"
echo "INPUT_HASH_ROOT=$hash_root"
echo "INPUT_PRODUCTION_AUTHORITY=$production_authority"
echo "INPUT_HCC1395_AUTHORITY=$hcc_authority"
echo "OUTPUT_REGIONAL_AUTHORITY=$output"
echo "OUTPUT_REGIONAL_AUTHORITY_SHA256=$(sha256sum "$output" | awk '{print $1}')"
echo "OUTPUT_DATASET_COUNT=$(jq '.datasets | length' "$output")"
echo "OUTPUT_FILE_ROWS=$(jq '[.datasets[].files[]] | length' "$output")"
