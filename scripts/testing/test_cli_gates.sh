#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: test_cli_gates.sh LONGLINEAGE REPO_ROOT" >&2
    exit 2
fi

producer="$1"
repo_root="$(cd "$2" && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/longlineage-cli-gates.XXXXXX")"
manifest="$scratch/authorized.json"
mismatch_manifest="$scratch/authority-mismatch.json"
synthetic_manifest="$scratch/synthetic-profile.json"
attestation_mismatch_manifest="$scratch/attestation-mismatch.json"
hcc_manifest="$scratch/hcc1395-dataset-gate.json"
hcc_authority_mismatch_manifest="$scratch/hcc1395-authority-mismatch.json"
hcc_missing_binding_manifest="$scratch/hcc1395-missing-binding.json"

digest() {
    sha256sum "$repo_root/$1" | awk '{print $1}'
}

science_sha="$(digest contracts/v1/science_parameters.json)"
catalog_sha="$(digest schema/catalog.json)"
status_sha="$(digest contracts/v1/status_reason_codes.tsv)"
types_sha="$(digest contracts/v1/type_registry.tsv)"
transforms_sha="$(digest contracts/v1/transform_registry.tsv)"
authority_sha="$(digest oracle/authority_manifest.json)"
mapping_sha="$(digest provenance/source_to_target_manifest.json)"
production_authority_sha="$(digest oracle/production_input_authority.json)"
schema_id_registry_sha="$(digest schema/id_registry.json)"
release_attestation_sha="$(digest state/release_attestation.json)"
hcc_authority_sha="$(digest oracle/hcc1395_dataset_gate_input_authority.json)"

jq -n \
    --slurpfile authority "$repo_root/oracle/production_input_authority.json" \
    --arg output_root "$scratch/.staging/production-gate" \
    --arg science "$science_sha" \
    --arg catalog "$catalog_sha" \
    --arg status "$status_sha" \
    --arg types "$types_sha" \
    --arg transforms "$transforms_sha" \
    --arg authority_sha "$authority_sha" \
    --arg mapping "$mapping_sha" \
    --arg production_authority "$production_authority_sha" \
    --arg schema_id_registry "$schema_id_registry_sha" \
    --arg release_attestation "$release_attestation_sha" '
{
  schema_name: "longlineage.production_manifest",
  schema_version: "1.0.0",
  authority_profile: "PRODUCTION_7_DATASET",
  run_id: "production-gate",
  output_root: $output_root,
  datasets: ($authority[0].datasets | map(
    . as $dataset |
    {
      dataset_id: .dataset_id,
      dataset_order: .dataset_order,
      files: [
        {role:"raw_bam", path:("/synthetic/input/" + .dataset_id + "/raw.bam"),
         size_bytes:1, sha256:("0" * 64)},
        {role:"raw_bam_index", path:("/synthetic/input/" + .dataset_id + "/raw.bam.bai"),
         size_bytes:1, sha256:("0" * 64)},
        {role:"pass_biallelic_ssnv_vcf",
         path:("/synthetic/input/" + .dataset_id + "/pass.vcf.gz"),
         size_bytes:.pass_biallelic_ssnv_vcf.size_bytes,
         sha256:.pass_biallelic_ssnv_vcf.sha256},
        {role:"pass_biallelic_ssnv_vcf_index",
         path:("/synthetic/input/" + .dataset_id + "/pass.vcf.gz.tbi"),
         size_bytes:.pass_biallelic_ssnv_vcf_index.size_bytes,
         sha256:.pass_biallelic_ssnv_vcf_index.sha256},
        {role:"latest_hp_ps_sidecar",
         path:("/synthetic/input/" + .dataset_id + "/latest.tsv.bgz"),
         size_bytes:.latest_hp_ps_sidecar.size_bytes,
         sha256:.latest_hp_ps_sidecar.sha256},
        {role:"latest_hp_ps_sidecar_index",
         path:("/synthetic/input/" + .dataset_id + "/latest.tsv.bgz.tbi"),
         size_bytes:.latest_hp_ps_sidecar_index.size_bytes,
         sha256:.latest_hp_ps_sidecar_index.sha256},
        {role:"reference_fasta", path:("/synthetic/input/" + .dataset_id + "/reference.fa"),
         size_bytes:1, sha256:("0" * 64)},
        {role:"reference_fai", path:("/synthetic/input/" + .dataset_id + "/reference.fa.fai"),
         size_bytes:1, sha256:("0" * 64)}
      ]
    }
  )),
  runtime: {
    compute_workers:40,
    writer_threads:4,
    coordinator_slots:2,
    buffer_bytes:8589934592,
    max_focal_sites_per_block:4096,
    max_estimated_alignments_per_block:250000,
    halo_bp:5000
  },
  contract_bindings: {
    science_parameters_sha256:$science,
    schema_catalog_sha256:$catalog,
    status_reason_registry_sha256:$status,
    type_registry_sha256:$types,
    transform_registry_sha256:$transforms,
    authority_manifest_sha256:$authority_sha,
    source_to_target_manifest_sha256:$mapping,
    production_input_authority_sha256:$production_authority,
    schema_id_registry_sha256:$schema_id_registry,
    release_attestation_sha256:$release_attestation
  }
}' >"$manifest"

jq -n \
    --slurpfile authority "$repo_root/oracle/hcc1395_dataset_gate_input_authority.json" \
    --arg output_root "$scratch/.staging/hcc1395-gate" \
    --arg science "$science_sha" \
    --arg catalog "$catalog_sha" \
    --arg status "$status_sha" \
    --arg types "$types_sha" \
    --arg transforms "$transforms_sha" \
    --arg authority_sha "$authority_sha" \
    --arg mapping "$mapping_sha" \
    --arg production_authority "$production_authority_sha" \
    --arg dataset_gate_authority "$hcc_authority_sha" \
    --arg schema_id_registry "$schema_id_registry_sha" \
    --arg release_attestation "$release_attestation_sha" '
{
  schema_name: "longlineage.production_manifest",
  schema_version: "1.1.0",
  authority_profile: "HCC1395_DATASET_GATE",
  run_id: "hcc1395-gate",
  output_root: $output_root,
  datasets: [{
    dataset_id: "HCC1395",
    dataset_order: 0,
    files: ($authority[0].files | map({
      role: .role,
      path: ("/synthetic/hcc1395/" + .role),
      size_bytes: .size_bytes,
      sha256: .sha256
    }))
  }],
  runtime: {
    compute_workers:24,
    writer_threads:4,
    coordinator_slots:2,
    buffer_bytes:8589934592,
    max_focal_sites_per_block:4096,
    max_estimated_alignments_per_block:250000,
    halo_bp:5000
  },
  contract_bindings: {
    science_parameters_sha256:$science,
    schema_catalog_sha256:$catalog,
    status_reason_registry_sha256:$status,
    type_registry_sha256:$types,
    transform_registry_sha256:$transforms,
    authority_manifest_sha256:$authority_sha,
    source_to_target_manifest_sha256:$mapping,
    production_input_authority_sha256:$production_authority,
    dataset_gate_input_authority_sha256:$dataset_gate_authority,
    schema_id_registry_sha256:$schema_id_registry,
    release_attestation_sha256:$release_attestation
  }
}' >"$hcc_manifest"

jq '(.datasets[0].files[] |
     select(.role == "latest_hp_ps_sidecar").sha256) = ("f" * 64)' \
    "$manifest" >"$mismatch_manifest"
jq '.authority_profile = "SYNTHETIC"' "$manifest" >"$synthetic_manifest"
jq '.contract_bindings.release_attestation_sha256 = ("f" * 64)' \
    "$manifest" >"$attestation_mismatch_manifest"
jq '.contract_bindings.dataset_gate_input_authority_sha256 = ("f" * 64)' \
    "$hcc_manifest" >"$hcc_authority_mismatch_manifest"
jq 'del(.contract_bindings.dataset_gate_input_authority_sha256)' \
    "$hcc_manifest" >"$hcc_missing_binding_manifest"

expect_exit() {
    local expected="$1"
    local label="$2"
    shift 2
    set +e
    local output
    output="$("$@" 2>&1)"
    local observed=$?
    set -e
    if [[ "$observed" -ne "$expected" ]]; then
        echo "${label}: expected=${expected} observed=${observed}" >&2
        echo "$output" >&2
        exit 1
    fi
    echo "${label}: PASS exit=${observed}"
    echo "$output" | sed -n '1p'
}

expect_exit 6 verified_kernel_gate \
    "$producer" run --manifest "$manifest" --repo "$repo_root"
expect_exit 6 manifest_bound_attestation_digest_gate \
    "$producer" run --manifest "$attestation_mismatch_manifest" --repo "$repo_root"
expect_exit 3 authority_sidecar_mismatch \
    "$producer" run --manifest "$mismatch_manifest" --repo "$repo_root"
expect_exit 3 synthetic_profile_rejected_by_run \
    "$producer" run --manifest "$synthetic_manifest" --repo "$repo_root"
expect_exit 3 hcc_profile_rejected_by_production_run \
    "$producer" run --manifest "$hcc_manifest" --repo "$repo_root"
expect_exit 3 production_profile_rejected_by_dataset_gate \
    "$producer" dataset-gate --manifest "$manifest" --repo "$repo_root"
expect_exit 3 hcc_authority_digest_mismatch \
    "$producer" dataset-gate --manifest "$hcc_authority_mismatch_manifest" --repo "$repo_root"
expect_exit 3 hcc_authority_binding_required \
    "$producer" dataset-gate --manifest "$hcc_missing_binding_manifest" --repo "$repo_root"
expect_exit 2 evaluation_option_absent_from_production \
    "$producer" run --truth-vcf /synthetic/never-created.vcf

echo "cli_gates: PASS scratch=$scratch"
