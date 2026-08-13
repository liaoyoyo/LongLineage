#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: build_regional_compat_manifest.sh \
  --repo-root ABS_PATH \
  --input-paths ABS_PATH \
  --run-id ID \
  --staging-base ABS_PATH \
  --output ABS_PATH

Builds a closed-shape PRODUCTION_7_DATASET manifest for the descriptive
regional compatibility endpoint. File sizes and full-content SHA-256 values
come only from the repository's frozen seven-dataset regional authority.
Private physical paths come from an external, untracked closed-shape map.
USAGE
}

repo_root=""
input_paths=""
run_id=""
staging_base=""
output=""
while (($# > 0)); do
    case "$1" in
        --repo-root) repo_root=${2:?}; shift 2 ;;
        --input-paths) input_paths=${2:?}; shift 2 ;;
        --run-id) run_id=${2:?}; shift 2 ;;
        --staging-base) staging_base=${2:?}; shift 2 ;;
        --output) output=${2:?}; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for required in repo_root input_paths run_id staging_base output; do
    if [[ -z ${!required} ]]; then
        echo "ERROR: --${required//_/-} is required" >&2
        exit 2
    fi
done
if [[ ${repo_root:0:1} != / || ${input_paths:0:1} != / ||
      ${staging_base:0:1} != / || ${output:0:1} != / ]]; then
    echo "ERROR: repository, input paths, staging base and output must be absolute paths" >&2
    exit 2
fi
if [[ ! $run_id =~ ^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$ ]]; then
    echo "ERROR: run-id violates the safe identifier contract" >&2
    exit 2
fi
if [[ ! -d $repo_root || ! -f $input_paths || -L $input_paths || -e $output ]]; then
    echo "ERROR: repository/input path map must exist and output must be new" >&2
    exit 3
fi

repo_root=$(realpath "$repo_root")
input_paths=$(realpath "$input_paths")
input_paths_sha256_before=$(sha256sum "$input_paths" | awk '{print $1}')
regional_authority="$repo_root/oracle/regional_compat_all_datasets_input_authority.json"
production_authority="$repo_root/oracle/production_input_authority.json"
hcc_authority="$repo_root/oracle/hcc1395_dataset_gate_input_authority.json"
if [[ ! -f $regional_authority || ! -f $production_authority || ! -f $hcc_authority ]]; then
    echo "ERROR: frozen regional, production and HCC authorities are required" >&2
    exit 3
fi

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

mkdir -p "$staging_base/.staging"
staging_base=$(realpath "$staging_base")
regional_authority_sha256_before=$(sha256sum "$regional_authority" | awk '{print $1}')
production_authority_sha256_before=$(sha256sum "$production_authority" | awk '{print $1}')
hcc_authority_sha256_before=$(sha256sum "$hcc_authority" | awk '{print $1}')

jq -e '
  .schema_name == "longlineage.regional_compat_all_datasets_input_authority" and
  .schema_version == "1.0.0" and
  .profile_id == "PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET" and
  ((keys | sort) == (["base_production_input_authority_sha256", "constraints", "datasets", "hcc1395_dataset_gate_input_authority_sha256", "profile_id", "schema_name", "schema_version"] | sort)) and
  (.constraints | (keys | sort) == (["all_physical_sha256_verified", "latest_tag_join", "persisted_tagged_bam_allowed", "private_source_paths_stored", "truth_fields"] | sort)) and
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
      (.sha256 | test("^[0-9a-f]{64}$"))))
' "$regional_authority" >/dev/null

if [[ $(jq -r '.base_production_input_authority_sha256' "$regional_authority") != "$production_authority_sha256_before" ]]; then
    echo "ERROR: regional authority does not bind the current production authority bytes" >&2
    exit 3
fi
if [[ $(jq -r '.hcc1395_dataset_gate_input_authority_sha256' "$regional_authority") != "$hcc_authority_sha256_before" ]]; then
    echo "ERROR: regional authority does not bind the current HCC dataset authority bytes" >&2
    exit 3
fi

digest() {
    local relative=$1
    sha256sum "$repo_root/$relative" | awk '{print $1}'
}

science_parameters_sha256=$(digest contracts/v1/science_parameters.json)
schema_catalog_sha256=$(digest schema/catalog.json)
status_reason_registry_sha256=$(digest contracts/v1/status_reason_codes.tsv)
type_registry_sha256=$(digest contracts/v1/type_registry.tsv)
transform_registry_sha256=$(digest contracts/v1/transform_registry.tsv)
authority_manifest_sha256=$(digest oracle/authority_manifest.json)
source_to_target_manifest_sha256=$(digest provenance/source_to_target_manifest.json)
production_input_authority_sha256=$(digest oracle/production_input_authority.json)
schema_id_registry_sha256=$(digest schema/id_registry.json)
release_attestation_sha256=$(digest state/release_attestation.json)

mkdir -p "$(dirname "$output")"
temporary="${output}.partial"
if [[ -e $temporary ]]; then
    echo "ERROR: auditable partial output already exists: $temporary" >&2
    exit 3
fi

jq -n \
  --slurpfile authority "$regional_authority" \
  --slurpfile paths "$input_paths" \
  --arg run_id "$run_id" \
  --arg output_root "$staging_base/.staging/$run_id" \
  --arg science_parameters_sha256 "$science_parameters_sha256" \
  --arg schema_catalog_sha256 "$schema_catalog_sha256" \
  --arg status_reason_registry_sha256 "$status_reason_registry_sha256" \
  --arg type_registry_sha256 "$type_registry_sha256" \
  --arg transform_registry_sha256 "$transform_registry_sha256" \
  --arg authority_manifest_sha256 "$authority_manifest_sha256" \
  --arg source_to_target_manifest_sha256 "$source_to_target_manifest_sha256" \
  --arg production_input_authority_sha256 "$production_input_authority_sha256" \
  --arg schema_id_registry_sha256 "$schema_id_registry_sha256" \
  --arg release_attestation_sha256 "$release_attestation_sha256" '
  ($authority[0]) as $locked |
  ($paths[0]) as $path_map |
  {
    schema_name:"longlineage.production_manifest",
    schema_version:"1.0.0",
    authority_profile:"PRODUCTION_7_DATASET",
    run_id:$run_id,
    output_root:$output_root,
    datasets:[
      $path_map.datasets[] as $dataset |
      ($locked.datasets[] | select(.dataset_id == $dataset.dataset_id and
                                    .dataset_order == $dataset.dataset_order)) as $authority_dataset |
      {
        dataset_id:$dataset.dataset_id,
        dataset_order:$dataset.dataset_order,
        files:[
          $dataset.files[] as $path_row |
          ($authority_dataset.files[] | select(.role == $path_row.role)) as $authority_row |
          ($path_row + {size_bytes:$authority_row.size_bytes, sha256:$authority_row.sha256})
        ]
      }
    ],
    runtime:{
      compute_workers:24,
      writer_threads:4,
      coordinator_slots:2,
      buffer_bytes:8589934592,
      max_focal_sites_per_block:4096,
      max_estimated_alignments_per_block:250000,
      halo_bp:5000
    },
    contract_bindings:{
      science_parameters_sha256:$science_parameters_sha256,
      schema_catalog_sha256:$schema_catalog_sha256,
      status_reason_registry_sha256:$status_reason_registry_sha256,
      type_registry_sha256:$type_registry_sha256,
      transform_registry_sha256:$transform_registry_sha256,
      authority_manifest_sha256:$authority_manifest_sha256,
      source_to_target_manifest_sha256:$source_to_target_manifest_sha256,
      production_input_authority_sha256:$production_input_authority_sha256,
      schema_id_registry_sha256:$schema_id_registry_sha256,
      release_attestation_sha256:$release_attestation_sha256
    }
  }
' >"$temporary"

jq -e --arg run_id "$run_id" --arg output_root "$staging_base/.staging/$run_id" '
  ((keys | sort) == (["authority_profile", "contract_bindings", "datasets", "output_root", "run_id", "runtime", "schema_name", "schema_version"] | sort)) and
  .schema_name == "longlineage.production_manifest" and
  .schema_version == "1.0.0" and
  .authority_profile == "PRODUCTION_7_DATASET" and
  .run_id == $run_id and .output_root == $output_root and
  ([.datasets[].dataset_id] == ["HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954"]) and
  ([.datasets[].dataset_order] == [0, 1, 2, 3, 4, 5, 6]) and
  all(.datasets[];
    ((keys | sort) == (["dataset_id", "dataset_order", "files"] | sort)) and
    ([.files[].role] == ["raw_bam", "raw_bam_index", "pass_biallelic_ssnv_vcf", "pass_biallelic_ssnv_vcf_index", "latest_hp_ps_sidecar", "latest_hp_ps_sidecar_index", "reference_fasta", "reference_fai"]) and
    all(.files[];
      ((keys | sort) == (["path", "role", "sha256", "size_bytes"] | sort)) and
      (.path | startswith("/") and (ascii_downcase | contains("truth") | not)) and
      (.size_bytes | type == "number" and . > 0) and
      (.sha256 | test("^[0-9a-f]{64}$")))) and
  (.runtime == {compute_workers:24, writer_threads:4, coordinator_slots:2, buffer_bytes:8589934592, max_focal_sites_per_block:4096, max_estimated_alignments_per_block:250000, halo_bp:5000}) and
  ((.contract_bindings | keys | sort) == (["authority_manifest_sha256", "production_input_authority_sha256", "release_attestation_sha256", "schema_catalog_sha256", "schema_id_registry_sha256", "science_parameters_sha256", "source_to_target_manifest_sha256", "status_reason_registry_sha256", "transform_registry_sha256", "type_registry_sha256"] | sort)) and
  all(.contract_bindings[]; type == "string" and test("^[0-9a-f]{64}$"))
' "$temporary" >/dev/null

if [[ $(sha256sum "$input_paths" | awk '{print $1}') != "$input_paths_sha256_before" ||
      $(sha256sum "$regional_authority" | awk '{print $1}') != "$regional_authority_sha256_before" ||
      $(sha256sum "$production_authority" | awk '{print $1}') != "$production_authority_sha256_before" ||
      $(sha256sum "$hcc_authority" | awk '{print $1}') != "$hcc_authority_sha256_before" ]]; then
    echo "ERROR: the private path map or an input authority changed while the manifest was built" >&2
    exit 4
fi
for binding in \
    "contracts/v1/science_parameters.json:$science_parameters_sha256" \
    "schema/catalog.json:$schema_catalog_sha256" \
    "contracts/v1/status_reason_codes.tsv:$status_reason_registry_sha256" \
    "contracts/v1/type_registry.tsv:$type_registry_sha256" \
    "contracts/v1/transform_registry.tsv:$transform_registry_sha256" \
    "oracle/authority_manifest.json:$authority_manifest_sha256" \
    "provenance/source_to_target_manifest.json:$source_to_target_manifest_sha256" \
    "oracle/production_input_authority.json:$production_input_authority_sha256" \
    "schema/id_registry.json:$schema_id_registry_sha256" \
    "state/release_attestation.json:$release_attestation_sha256"; do
    relative=${binding%%:*}
    expected=${binding##*:}
    if [[ $(digest "$relative") != "$expected" ]]; then
        echo "ERROR: repository contract changed while the manifest was built: $relative" >&2
        exit 4
    fi
done
mv "$temporary" "$output"

echo "INPUT_REPOSITORY_ROOT=$repo_root"
echo "INPUT_PATH_MAP_SHA256=$input_paths_sha256_before"
echo "INPUT_REGIONAL_AUTHORITY=$regional_authority"
echo "INPUT_PRODUCTION_AUTHORITY=$production_authority"
echo "INPUT_HCC1395_AUTHORITY=$hcc_authority"
echo "OUTPUT_MANIFEST=$output"
echo "OUTPUT_MANIFEST_SHA256=$(sha256sum "$output" | awk '{print $1}')"
echo "OUTPUT_DATASETS=$(jq -r '[.datasets[].dataset_id] | join(",")' "$output")"
