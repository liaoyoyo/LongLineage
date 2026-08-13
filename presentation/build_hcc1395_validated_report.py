#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a fail-closed, presentation-only HCC1395 w24/w40 frozen-run report.

This program intentionally does not implement scientific kernels. It requires
two explicit independently VALIDATED_FROZEN_DATASET_GATE run roots, a closed
execution-evidence envelope, and an independent C++ determinism/historical
receipt. It replays file/receipt/digest bindings and maps already-produced C++
counters and frozen settings to a standalone HTML document. It never opens BAM,
VCF, latest-tag sidecar, or reference inputs and never recomputes science.
"""

from __future__ import annotations

import argparse
import ast
import csv
import gzip
import hashlib
import html
import json
import os
import re
import stat
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, List, Mapping, Tuple


SHA256_ZERO = "0" * 64
EXPECTED_SITE_KEYS = 79_687
EXPECTED_DATASET_ID = "HCC1395"
EXPECTED_TERMINAL_STATE = "VALIDATED_FROZEN_DATASET_GATE"
EXPECTED_PROFILE = "DATASET_GATE"
EXPECTED_REPORT_SCOPE = "HCC1395_ONLY_NON_PRODUCTION_DATASET_GATE"
EXPECTED_ARTIFACT_IDS = [
    "site_reads",
    "methyl_calls",
    "bernoulli_upper",
    "m1_sites",
    "m1_assignments",
    "cooccurrence_pairs",
    "cooccurrence_sites",
    "topology_units",
]
EXPECTED_MANIFEST_DIFFERENCES = [
    "/output_root",
    "/run_id",
    "/runtime/compute_workers",
]
EXPECTED_SUMMARY_PROJECTION_FIELDS = [
    "scope",
    "counts",
    "phase_status",
]
COOCCURRENCE_REPLAY_COUNTER_KEYS = {
    "pair_rows",
    "exact_identifiable_pairs",
    "ineligible_m2_screen_pairs",
    "eligible_endpoint_a_not_testable_pairs",
    "eligible_exact_not_identifiable_pairs",
    "eligible_exact_family_pairs",
    "family_partition_total",
    "fdr_family_size",
    "global_bh_discoveries",
    "global_by_discoveries",
    "formal_pair_by_confirmed",
    "cooccurrence_site_rows",
    "partner_universe_pair_rows",
    "joint_signature_pass_sites",
    "joint_signature_not_testable_sites",
    "joint_signature_partition_total",
    "topology_units",
}
EXPECTED_AUDIT_CHECK_SEQUENCE = (
    "FROZEN_W24",
    "FROZEN_W40",
    "MANIFEST_WHITELIST",
    "SCIENCE_ARTIFACT_DETERMINISM",
    "SUMMARY_PROJECTION",
    "M2_MUTUALLY_EXCLUSIVE_CONSERVATION",
    "COOCCURRENCE_SERIALIZED_CENSUS_CONSERVATION",
    "HISTORICAL_SITE_KEY_M1",
)
EXPECTED_INPUT_ROLES = {
    "raw_bam",
    "raw_bam_index",
    "pass_biallelic_ssnv_vcf",
    "pass_biallelic_ssnv_vcf_index",
    "latest_hp_ps_sidecar",
    "latest_hp_ps_sidecar_index",
    "reference_fasta",
    "reference_fai",
}
SAFE_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
PRIVATE_PATH_PATTERN = re.compile(
    rb"/(?:big|bip)[0-9]+_disk/|/home/[^ /]+/|/Users/[^ /]+/"
)
REAL_COORDINATE_PATTERN = re.compile(
    rb"(?:^|[^A-Za-z0-9_])chr(?:[1-9]|1[0-9]|2[0-2]|X|Y|M|MT):[0-9]+"
    rb"|^chr(?:[1-9]|1[0-9]|2[0-2]|X|Y|M|MT)[ \t]+[0-9]+",
    re.MULTILINE,
)
CREDENTIAL_PATTERN = re.compile(
    rb"gh[pousr]_[A-Za-z0-9]{30,}|AKIA[0-9A-Z]{16}|"
    rb"-----BEGIN (?:[A-Z ]+ )?PRIVATE KEY-----|"
    rb"sk-(?:proj-)?[A-Za-z0-9_-]{20,}|"
    rb"(?:password|passwd|access_token|api_key)[ \t]*[:=][ \t]*[\"'][^\"']{8,}",
    re.IGNORECASE,
)
TRACKED_REPORT_MAX_BYTES = 1_048_576


class EvidenceError(RuntimeError):
    """Raised when a report input cannot support a frozen-data claim."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def is_sha256(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_sha256(value: Any) -> str:
    payload = json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def git_output(repo: Path, *arguments: str) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo), *arguments],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
    except OSError as error:
        raise EvidenceError(f"git invocation failed: {error}") from error
    require(
        completed.returncode == 0,
        f"git {' '.join(arguments)} failed: {completed.stderr.strip()}",
    )
    return completed.stdout.strip()


def safe_identifier(value: Any, label: str) -> str:
    require(
        isinstance(value, str) and SAFE_ID_PATTERN.fullmatch(value) is not None,
        f"{label} is not a portable identifier",
    )
    return value


def public_binding(binding: Mapping[str, Any]) -> Dict[str, Any]:
    """Project a verified external binding without exposing its machine path."""

    return {
        "external_binding": True,
        "size_bytes": binding["size_bytes"],
        "sha256": binding["sha256"],
    }


def projected_validation_check(check: Mapping[str, Any], label: str) -> Dict[str, Any]:
    check_id = safe_identifier(check.get("check_id"), f"{label} check_id")
    require(check.get("status") == "PASS", f"{label} check is not PASS: {check_id}")
    require(
        is_sha256(check.get("evidence_sha256")),
        f"{label} check evidence SHA-256 is invalid: {check_id}",
    )
    payload = {
        "reason": check.get("reason"),
        "expected": check.get("expected"),
        "observed": check.get("observed"),
    }
    return {
        "check_id": check_id,
        "status": "PASS",
        "evidence_sha256": check["evidence_sha256"],
        "payload_sha256": canonical_sha256(payload),
    }


def assert_tracked_payload_safe(
    model: Mapping[str, Any],
    html_payload: bytes,
    json_payload: bytes,
    known_external_paths: List[str],
) -> None:
    """Fail before write if a portable report contains restricted local evidence."""

    del model  # The final serialized payloads are the complete policy surface.
    for label, payload in (("HTML", html_payload), ("JSON", json_payload)):
        require(
            len(payload) <= TRACKED_REPORT_MAX_BYTES,
            f"{label} exceeds tracked-file ceiling: {len(payload)} bytes",
        )
        require(
            PRIVATE_PATH_PATTERN.search(payload) is None,
            f"{label} contains an absolute private path",
        )
        require(
            REAL_COORDINATE_PATTERN.search(payload) is None,
            f"{label} contains a real-coordinate-shaped payload",
        )
        require(
            CREDENTIAL_PATTERN.search(payload) is None,
            f"{label} contains a credential-like payload",
        )
        for external_path in known_external_paths:
            require(
                external_path.encode("utf-8") not in payload,
                f"{label} contains a verified external path",
            )


def collect_known_external_paths(value: Any) -> List[str]:
    paths: set[str] = set()

    def visit(item: Any) -> None:
        if isinstance(item, dict):
            for child in item.values():
                visit(child)
        elif isinstance(item, list):
            for child in item:
                visit(child)
        elif isinstance(item, str) and item.startswith("/"):
            candidate = Path(item)
            if candidate.exists() or PRIVATE_PATH_PATTERN.search(item.encode("utf-8")):
                paths.add(item)

    visit(value)
    return sorted(paths, key=lambda path: (-len(path), path))


def read_json(path: Path, label: str) -> Dict[str, Any]:
    require(path.is_file(), f"{label} missing: {path}")
    require(not path.is_symlink(), f"{label} must not be a symlink: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"{label} is not strict readable JSON: {path}: {error}") from error
    require(isinstance(value, dict), f"{label} root must be an object: {path}")
    return value


def require_closed_object(
    value: Any, expected_keys: set[str], label: str
) -> Mapping[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    observed = set(value)
    require(
        observed == expected_keys,
        f"{label} membership mismatch: missing={sorted(expected_keys - observed)} "
        f"extra={sorted(observed - expected_keys)}",
    )
    return value


def regular_absolute_file(path_value: Any, label: str) -> Path:
    require(
        isinstance(path_value, str) and path_value.startswith("/"),
        f"{label} path must be absolute",
    )
    path = Path(path_value)
    try:
        mode = path.lstat().st_mode
    except OSError as error:
        raise EvidenceError(f"{label} missing: {path}: {error}") from error
    require(stat.S_ISREG(mode), f"{label} is not a regular file: {path}")
    require(not path.is_symlink(), f"{label} must not be a symlink: {path}")
    return path.resolve(strict=True)


def verify_absolute_binding(binding: Any, label: str) -> Dict[str, Any]:
    row = require_closed_object(
        binding, {"path", "size_bytes", "sha256"}, f"{label} binding"
    )
    path = regular_absolute_file(row["path"], label)
    expected_size = row["size_bytes"]
    expected_sha = row["sha256"]
    require(
        isinstance(expected_size, int) and not isinstance(expected_size, bool)
        and expected_size > 0,
        f"{label} has invalid size binding",
    )
    require(is_sha256(expected_sha), f"{label} has invalid SHA-256 binding")
    observed_size = path.stat().st_size
    require(
        observed_size == expected_size,
        f"{label} size mismatch: observed={observed_size} expected={expected_size}",
    )
    observed_sha = sha256_file(path)
    require(
        observed_sha == expected_sha,
        f"{label} SHA-256 mismatch: observed={observed_sha} expected={expected_sha}",
    )
    return {
        "path": str(path),
        "size_bytes": observed_size,
        "sha256": observed_sha,
    }


def parse_elapsed_seconds(value: str) -> float:
    parts = value.strip().split(":")
    require(1 <= len(parts) <= 3, f"unsupported GNU time elapsed value: {value!r}")
    try:
        numbers = [float(part) for part in parts]
    except ValueError as error:
        raise EvidenceError(f"invalid GNU time elapsed value: {value!r}") from error
    require(all(number >= 0 for number in numbers), "GNU time elapsed value is negative")
    if len(numbers) == 3:
        return numbers[0] * 3600.0 + numbers[1] * 60.0 + numbers[2]
    if len(numbers) == 2:
        return numbers[0] * 60.0 + numbers[1]
    return numbers[0]


def parse_gnu_time_log(path: Path, label: str) -> Dict[str, Any]:
    try:
        text_value = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise EvidenceError(f"{label} is not readable UTF-8: {path}: {error}") from error

    def final_match(pattern: str, field: str) -> str:
        matches = re.findall(pattern, text_value, flags=re.MULTILINE)
        require(matches, f"{label} lacks GNU time field {field}")
        return matches[-1].strip()

    try:
        result = {
            "wall_seconds": parse_elapsed_seconds(
                final_match(
                    r"^[ \t]*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):[ \t]*(.+)$",
                    "wall_seconds",
                )
            ),
            "user_seconds": float(
                final_match(r"^[ \t]*User time \(seconds\):[ \t]*(.+)$", "user_seconds")
            ),
            "system_seconds": float(
                final_match(r"^[ \t]*System time \(seconds\):[ \t]*(.+)$", "system_seconds")
            ),
            "max_rss_kib": int(
                final_match(
                    r"^[ \t]*Maximum resident set size \(kbytes\):[ \t]*(.+)$",
                    "max_rss_kib",
                )
            ),
            "exit_code": int(
                final_match(r"^[ \t]*Exit status:[ \t]*(.+)$", "exit_code")
            ),
        }
    except ValueError as error:
        raise EvidenceError(f"{label} contains a non-numeric GNU time field") from error
    require(
        result["wall_seconds"] > 0
        and result["user_seconds"] >= 0
        and result["system_seconds"] >= 0
        and result["max_rss_kib"] > 0,
        f"{label} contains invalid GNU time values",
    )
    return result


def verify_timed_execution(value: Any, label: str) -> Dict[str, Any]:
    row = require_closed_object(
        value,
        {
            "stdout_log",
            "time_log",
            "status_marker",
            "exit_code",
            "wall_seconds",
            "user_seconds",
            "system_seconds",
            "max_rss_kib",
        },
        label,
    )
    stdout_binding = verify_absolute_binding(row["stdout_log"], f"{label} stdout")
    time_binding = verify_absolute_binding(row["time_log"], f"{label} time log")
    marker = row["status_marker"]
    require(
        isinstance(marker, str) and 0 < len(marker) <= 160,
        f"{label} status_marker is invalid",
    )
    try:
        stdout_text = Path(stdout_binding["path"]).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise EvidenceError(f"{label} stdout is not readable UTF-8: {error}") from error
    require(marker in stdout_text, f"{label} stdout lacks status marker {marker!r}")
    parsed = parse_gnu_time_log(Path(time_binding["path"]), f"{label} time log")
    require(row["exit_code"] == parsed["exit_code"] == 0, f"{label} exit code is not zero")
    for field in ("wall_seconds", "user_seconds", "system_seconds"):
        require(
            isinstance(row[field], (int, float)) and not isinstance(row[field], bool),
            f"{label} {field} is not numeric",
        )
        require(
            abs(float(row[field]) - parsed[field]) <= 0.011,
            f"{label} {field} differs from GNU time log: "
            f"evidence={row[field]} parsed={parsed[field]}",
        )
    require(
        isinstance(row["max_rss_kib"], int)
        and row["max_rss_kib"] == parsed["max_rss_kib"],
        f"{label} max_rss_kib differs from GNU time log",
    )
    return {
        **parsed,
        "stdout_log": stdout_binding,
        "time_log": time_binding,
        "status_marker": marker,
    }


def json_pointer_differences(left: Any, right: Any, pointer: str = "") -> List[str]:
    if type(left) is not type(right):
        return [pointer or "/"]
    if isinstance(left, dict):
        differences: List[str] = []
        for key in sorted(set(left) | set(right)):
            escaped = str(key).replace("~", "~0").replace("/", "~1")
            child = f"{pointer}/{escaped}"
            if key not in left or key not in right:
                differences.append(child)
            else:
                differences.extend(json_pointer_differences(left[key], right[key], child))
        return differences
    if isinstance(left, list):
        differences = []
        if len(left) != len(right):
            differences.append(f"{pointer}/length")
        for index, (left_item, right_item) in enumerate(zip(left, right)):
            differences.extend(
                json_pointer_differences(left_item, right_item, f"{pointer}/{index}")
            )
        return differences
    return [] if left == right else [pointer or "/"]


def regular_under(root: Path, relative: str, label: str) -> Path:
    require(isinstance(relative, str) and relative and not relative.startswith("/"),
            f"{label} relative_path is invalid: {relative!r}")
    require(".." not in Path(relative).parts, f"{label} escapes run root: {relative}")
    path = root / relative
    try:
        mode = path.lstat().st_mode
    except OSError as error:
        raise EvidenceError(f"{label} missing: {path}: {error}") from error
    require(stat.S_ISREG(mode), f"{label} is not a regular file: {path}")
    require(not path.is_symlink(), f"{label} must not be a symlink: {path}")
    resolved = path.resolve(strict=True)
    require(resolved.is_relative_to(root), f"{label} resolves outside run root: {path}")
    return resolved


def verify_file_binding(
    root: Path, binding: Mapping[str, Any], label: str
) -> Dict[str, Any]:
    relative = binding.get("relative_path")
    path = regular_under(root, relative, label)
    size = path.stat().st_size
    expected_size = binding.get("size_bytes")
    expected_sha = binding.get("physical_sha256")
    require(isinstance(expected_size, int) and expected_size > 0,
            f"{label} has invalid size binding")
    require(is_sha256(expected_sha), f"{label} has invalid SHA-256 binding")
    require(size == expected_size,
            f"{label} size mismatch: observed={size} expected={expected_size}")
    observed_sha = sha256_file(path)
    require(observed_sha == expected_sha,
            f"{label} SHA-256 mismatch: observed={observed_sha} expected={expected_sha}")
    return {
        "relative_path": relative,
        "size_bytes": size,
        "physical_sha256": observed_sha,
    }


def read_bgzf_jsonl(path: Path, label: str) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    try:
        with gzip.open(path, "rt", encoding="utf-8", newline="") as handle:
            for line_number, raw in enumerate(handle, 1):
                require(raw.endswith("\n"), f"{label} row {line_number} lacks LF terminator")
                try:
                    row = json.loads(raw)
                except json.JSONDecodeError as error:
                    raise EvidenceError(
                        f"{label} row {line_number} is invalid JSON: {error}"
                    ) from error
                require(isinstance(row, dict), f"{label} row {line_number} is not an object")
                rows.append(row)
    except (OSError, EOFError) as error:
        raise EvidenceError(f"{label} cannot be decompressed: {path}: {error}") from error
    return rows


def read_semantic_digests(path: Path) -> Dict[str, Dict[str, Any]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle, delimiter="\t")
            require(
                reader.fieldnames
                == [
                    "artifact_id",
                    "schema_name",
                    "schema_version",
                    "logical_rows",
                    "semantic_sha256",
                ],
                "semantic_digests.tsv header differs from the frozen contract",
            )
            rows: Dict[str, Dict[str, Any]] = {}
            for line_number, row in enumerate(reader, 2):
                artifact_id = row["artifact_id"]
                require(artifact_id not in rows,
                        f"semantic_digests.tsv duplicate artifact at row {line_number}")
                require(row["logical_rows"].isdigit(),
                        f"semantic_digests.tsv invalid logical_rows at row {line_number}")
                require(is_sha256(row["semantic_sha256"]),
                        f"semantic_digests.tsv invalid SHA-256 at row {line_number}")
                rows[artifact_id] = {
                    "schema_name": row["schema_name"],
                    "schema_version": row["schema_version"],
                    "logical_rows": int(row["logical_rows"]),
                    "semantic_sha256": row["semantic_sha256"],
                }
    except (OSError, UnicodeDecodeError) as error:
        raise EvidenceError(f"semantic_digests.tsv is unreadable: {path}: {error}") from error
    return rows


def parse_checksums(path: Path) -> Dict[str, str]:
    rows: Dict[str, str] = {}
    try:
        for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            parts = raw.split("  ", 1)
            require(len(parts) == 2 and is_sha256(parts[0]) and parts[1],
                    f"checksums.sha256 malformed row {line_number}")
            require(parts[1] not in rows, f"checksums.sha256 duplicate path: {parts[1]}")
            rows[parts[1]] = parts[0]
    except (OSError, UnicodeDecodeError) as error:
        raise EvidenceError(f"checksums.sha256 is unreadable: {path}: {error}") from error
    return rows


def artifact_identity(binding: Mapping[str, Any]) -> Tuple[Any, ...]:
    return (
        binding.get("artifact_id"),
        binding.get("role"),
        binding.get("relative_path"),
        binding.get("schema_name"),
        binding.get("schema_version"),
        binding.get("format"),
        binding.get("size_bytes"),
        binding.get("physical_sha256"),
        binding.get("logical_rows"),
        binding.get("semantic_sha256"),
        binding.get("index"),
        binding.get("sensitivity"),
        binding.get("transform_id"),
        binding.get("producer_executable_sha256"),
        binding.get("inputs"),
        binding.get("primary_key_first"),
        binding.get("primary_key_last"),
    )


def validate_summary(summary: Mapping[str, Any], run_id: str) -> None:
    require(summary.get("schema_name") == "longlineage.summary",
            "summary schema_name mismatch")
    require(summary.get("schema_version") == "1.0.0", "summary schema_version mismatch")
    require(summary.get("run_id") == run_id, "summary run_id mismatch")
    scope = summary.get("scope")
    require(isinstance(scope, dict), "summary scope missing")
    require(scope.get("task_type") == "B", "HCC report requires Task B")
    require(scope.get("completeness") == "FULL", "HCC report requires FULL sample scope")
    require(scope.get("dataset_count") == 1, "HCC report requires exactly one dataset")
    require(scope.get("dataset_ids") == [EXPECTED_DATASET_ID],
            "HCC report requires dataset_ids=[HCC1395]")

    counts = summary.get("counts")
    require(isinstance(counts, dict), "summary counts missing")
    required_counts = {
        "site_keys",
        "site_keys_missing",
        "site_keys_extra",
        "site_keys_duplicate",
        "m1_evaluable",
        "m1_insufficient_alt_reads",
        "m1_incomplete_distance",
        "m1_stable_assignments",
        "latest_tag_exact_joins",
        "latest_tag_missing",
        "latest_tag_conflict",
        "latest_tag_multimatch",
        "m2_eligible",
        "m2_evaluable_ineligible",
        "m2_axis_indeterminate",
        "m2_group_count_gt10",
        "raw_expected",
        "raw_matched",
        "raw_rg_only_duplicate_occurrences",
        "topology_primary_hp_units",
        "topology_regions",
        "topology_fully_complete_regions",
        "topology_incomplete_regions",
        "topology_incomplete_units_with_winner",
    }
    require(required_counts == set(counts), "summary count membership mismatch")
    require(all(isinstance(counts[key], int) and counts[key] >= 0 for key in counts),
            "summary counts must be non-negative integers")
    require(counts["site_keys"] == EXPECTED_SITE_KEYS,
            f"HCC autosomal site count must be {EXPECTED_SITE_KEYS}")
    require(
        counts["site_keys"]
        == counts["m1_evaluable"]
        + counts["m1_insufficient_alt_reads"]
        + counts["m1_incomplete_distance"],
        "M1 focal-site conservation failed",
    )
    require(
        counts["site_keys_missing"]
        == counts["site_keys_extra"]
        == counts["site_keys_duplicate"]
        == 0,
        "frozen site population has missing/extra/duplicate keys",
    )
    require(
        counts["latest_tag_missing"]
        == counts["latest_tag_conflict"]
        == counts["latest_tag_multimatch"]
        == 0,
        "latest HP/PS join conservation failed",
    )
    require(
        counts["raw_expected"] + counts["raw_rg_only_duplicate_occurrences"]
        == counts["raw_matched"],
        "raw alignment recovery conservation failed",
    )
    require(
        counts["m2_eligible"]
        + counts["m2_evaluable_ineligible"]
        + counts["m2_axis_indeterminate"]
        + counts["m2_group_count_gt10"]
        == counts["m1_stable_assignments"],
        "M2 mutually-exclusive census does not conserve M1 stable sites",
    )
    require(
        counts["topology_fully_complete_regions"]
        + counts["topology_incomplete_regions"]
        == counts["topology_regions"],
        "topology region completion census failed",
    )
    require(
        counts["topology_incomplete_units_with_winner"] == 0,
        "incomplete topology unit illegally published a winner",
    )


def load_verified_bundle(repo: Path, run_root: Path) -> Dict[str, Any]:
    """Replay the small-authority chain and physical artifact hashes."""

    repo = repo.resolve(strict=True)
    run_root = run_root.resolve(strict=True)
    require(repo.is_dir(), f"repo is not a directory: {repo}")
    require(run_root.is_dir(), f"run root is not a directory: {run_root}")

    run_path = regular_under(run_root, "run_receipt.json", "run receipt")
    validation_path = regular_under(run_root, "validation_receipt.json", "validation receipt")
    producer_path = regular_under(
        run_root, "receipts/producer_receipt.json", "producer receipt"
    )
    checksums_path = regular_under(run_root, "checksums.sha256", "checksums")
    run = read_json(run_path, "run receipt")
    validation = read_json(validation_path, "validation receipt")
    producer = read_json(producer_path, "producer receipt")

    run_id = safe_identifier(run.get("run_id"), "run receipt run_id")
    require(run.get("schema_name") == "longlineage.run_receipt", "run receipt schema mismatch")
    require(run.get("schema_version") == "1.0.0", "run receipt version mismatch")
    require(run.get("state") == EXPECTED_TERMINAL_STATE,
            f"run is not {EXPECTED_TERMINAL_STATE}")
    require(run.get("validation_profile") == EXPECTED_PROFILE,
            "run is not a DATASET_GATE validation profile")
    require(run.get("production_claim_allowed") is False,
            "HCC-only dataset gate must never permit a production claim")
    require(run.get("truth_fields_seen") == 0, "run receipt observed truth fields")
    require(run.get("input_snapshot_before_sha256")
            == run.get("input_snapshot_after_sha256"),
            "run input snapshots differ")

    history = run.get("state_history")
    require(isinstance(history, list) and len(history) == 3,
            "run state_history must contain exactly three states")
    require(
        [event.get("sequence") for event in history] == [0, 1, 2]
        and [event.get("state") for event in history]
        == ["RUNNING", "VALIDATED", EXPECTED_TERMINAL_STATE],
        "run state_history is not RUNNING→VALIDATED→VALIDATED_FROZEN_DATASET_GATE",
    )

    require(validation.get("schema_name") == "longlineage.validation_receipt",
            "validation receipt schema mismatch")
    require(validation.get("schema_version") == "1.0.0",
            "validation receipt version mismatch")
    require(validation.get("run_id") == run_id, "validation receipt run_id mismatch")
    require(validation.get("validation_profile") == EXPECTED_PROFILE,
            "validation receipt profile mismatch")
    require(validation.get("production_claim_allowed") is False,
            "validation receipt illegally permits a production claim")
    require(validation.get("all_pass") is True, "independent validation did not pass")
    require(validation.get("validator_independent") is True,
            "validator did not declare independence")
    require(validation.get("linked_producer_kernels") is False,
            "validator is linked to producer kernels")
    checks = validation.get("checks")
    require(isinstance(checks, list) and checks, "validation checks are absent")
    require(all(check.get("status") == "PASS" for check in checks),
            "at least one independent validation check is not PASS")
    require(validation.get("input_snapshot_before_sha256")
            == validation.get("input_snapshot_after_sha256")
            == run.get("input_snapshot_before_sha256"),
            "validation/run input snapshots disagree")

    require(producer.get("schema_name") == "longlineage.producer_receipt",
            "producer receipt schema mismatch")
    require(producer.get("schema_version") == "1.0.0", "producer receipt version mismatch")
    require(producer.get("run_id") == run_id, "producer receipt run_id mismatch")
    require(producer.get("state") == "RUNNING"
            and producer.get("producer_outcome") == "READY_FOR_VALIDATION",
            "producer receipt is not closed and ready for validation")
    require(producer.get("truth_fields_seen") == 0, "producer observed truth fields")
    require(producer.get("input_snapshot_before_sha256")
            == producer.get("input_snapshot_after_sha256")
            == run.get("input_snapshot_before_sha256"),
            "producer/run input snapshots disagree")
    production_executable = run.get("production_executable")
    require(isinstance(production_executable, dict), "production executable metadata missing")
    require(
        production_executable.get("executable_sha256")
        == producer.get("producer_executable_sha256")
        == validation.get("producer_executable_sha256"),
        "producer executable SHA-256 bindings disagree",
    )
    require(
        history[0].get("actor_executable_sha256")
        == producer.get("producer_executable_sha256"),
        "RUNNING state actor is not the producer executable",
    )
    require(
        history[1].get("actor_executable_sha256")
        == history[2].get("actor_executable_sha256")
        == validation.get("validator_executable_sha256"),
        "VALIDATED/frozen state actor is not the validator executable",
    )

    observed_validation_sha = sha256_file(validation_path)
    observed_producer_sha = sha256_file(producer_path)
    observed_checksums_sha = sha256_file(checksums_path)
    require(run.get("validation_receipt_sha256") == observed_validation_sha,
            "run receipt does not bind validation_receipt.json")
    require(run.get("producer_receipt_sha256") == observed_producer_sha
            == validation.get("producer_receipt_sha256"),
            "producer receipt SHA-256 binding mismatch")
    require(run.get("checksums_sha256") == observed_checksums_sha,
            "run receipt does not bind checksums.sha256")

    catalog_path = repo / "schema/catalog.json"
    science_path = repo / "contracts/v1/science_parameters.json"
    catalog_contract = read_json(catalog_path, "schema catalog")
    science_contract = read_json(science_path, "science parameters")
    require(sha256_file(catalog_path) == run.get("schema_catalog_sha256")
            == validation.get("schema_catalog_sha256")
            == producer.get("schema_catalog_sha256"),
            "schema catalog SHA-256 binding mismatch")
    require(sha256_file(science_path) == run.get("science_parameters_sha256")
            == validation.get("science_parameters_sha256")
            == producer.get("science_parameters_sha256"),
            "science parameter SHA-256 binding mismatch")

    membership = catalog_contract.get("run_membership")
    require(isinstance(membership, dict), "schema catalog run_membership missing")
    expected_run_ids = set(membership.get("run_receipt_artifact_ids", []))
    expected_science_ids = set(membership.get("scientific_artifact_ids", []))
    expected_semantic_ids = set(membership.get("semantic_digest_artifact_ids", []))
    require(expected_run_ids and expected_science_ids and expected_semantic_ids,
            "schema catalog memberships are empty")

    run_artifacts_raw = run.get("artifacts")
    producer_artifacts_raw = producer.get("artifacts")
    require(isinstance(run_artifacts_raw, list) and isinstance(producer_artifacts_raw, list),
            "receipt artifact arrays are missing")
    run_artifacts = {row.get("artifact_id"): row for row in run_artifacts_raw}
    producer_artifacts = {row.get("artifact_id"): row for row in producer_artifacts_raw}
    require(len(run_artifacts) == len(run_artifacts_raw), "run receipt has duplicate artifacts")
    require(len(producer_artifacts) == len(producer_artifacts_raw),
            "producer receipt has duplicate artifacts")
    require(set(run_artifacts) == expected_run_ids,
            "run receipt artifact membership differs from schema catalog")
    require(set(producer_artifacts) == expected_run_ids,
            "producer receipt artifact membership differs from schema catalog")
    for artifact_id in expected_run_ids:
        require(
            artifact_identity(run_artifacts[artifact_id])
            == artifact_identity(producer_artifacts[artifact_id]),
            f"producer/run artifact binding differs: {artifact_id}",
        )

    verified_files: List[Dict[str, Any]] = []
    expected_checksum_rows: Dict[str, str] = {}
    for artifact_id in sorted(run_artifacts):
        binding = run_artifacts[artifact_id]
        verified = verify_file_binding(run_root, binding, f"artifact {artifact_id}")
        verified["artifact_id"] = artifact_id
        verified["logical_rows"] = binding.get("logical_rows")
        verified["semantic_sha256"] = binding.get("semantic_sha256")
        verified_files.append(verified)
        expected_checksum_rows[binding["relative_path"]] = binding["physical_sha256"]
        index = binding.get("index")
        if index is not None:
            verified_index = verify_file_binding(
                run_root, index, f"artifact index {artifact_id}"
            )
            verified_index["artifact_id"] = f"{artifact_id}.index"
            verified_index["logical_rows"] = index.get("logical_rows")
            verified_index["semantic_sha256"] = index.get("semantic_sha256")
            verified_files.append(verified_index)
            expected_checksum_rows[index["relative_path"]] = index["physical_sha256"]
    expected_checksum_rows["receipts/producer_receipt.json"] = observed_producer_sha
    require(parse_checksums(checksums_path) == expected_checksum_rows,
            "checksums.sha256 rows differ from frozen receipt metadata")

    catalog_artifact_path = regular_under(
        run_root,
        run_artifacts["artifact_catalog"]["relative_path"],
        "artifact catalog",
    )
    catalog_rows = read_bgzf_jsonl(catalog_artifact_path, "artifact catalog")
    catalog_records: Dict[str, Dict[str, Any]] = {}
    for line_number, row in enumerate(catalog_rows, 1):
        require(row.get("schema_name") == "longlineage.artifact_catalog_record"
                and row.get("schema_version") == "1.0.0"
                and row.get("run_id") == run_id,
                f"artifact catalog envelope mismatch at row {line_number}")
        artifact = row.get("artifact")
        require(isinstance(artifact, dict), f"artifact catalog row {line_number} missing artifact")
        artifact_id = artifact.get("artifact_id")
        require(artifact_id not in catalog_records,
                f"artifact catalog duplicate artifact: {artifact_id}")
        catalog_records[artifact_id] = artifact
    require(set(catalog_records) == expected_science_ids,
            "artifact catalog science membership mismatch")
    for artifact_id in expected_science_ids:
        require(
            artifact_identity(catalog_records[artifact_id])
            == artifact_identity(run_artifacts[artifact_id]),
            f"artifact catalog/run receipt binding mismatch: {artifact_id}",
        )

    semantic_path = regular_under(
        run_root,
        run_artifacts["semantic_digests"]["relative_path"],
        "semantic digests",
    )
    semantic_rows = read_semantic_digests(semantic_path)
    require(set(semantic_rows) == expected_semantic_ids,
            "semantic digest membership mismatch")
    for artifact_id in expected_semantic_ids:
        binding = run_artifacts[artifact_id]
        observed = semantic_rows[artifact_id]
        require(
            observed
            == {
                "schema_name": binding["schema_name"],
                "schema_version": binding["schema_version"],
                "logical_rows": binding["logical_rows"],
                "semantic_sha256": binding["semantic_sha256"],
            },
            f"semantic digest row differs from frozen receipt: {artifact_id}",
        )

    summary_path = regular_under(
        run_root, run_artifacts["summary"]["relative_path"], "summary"
    )
    summary = read_json(summary_path, "summary")
    validate_summary(summary, run_id)

    mount_rows = producer.get("input_mount_identity")
    require(isinstance(mount_rows, list), "producer input_mount_identity missing")
    mount_roles = [row.get("role") for row in mount_rows]
    require(set(mount_roles) == EXPECTED_INPUT_ROLES and len(mount_roles) == 8,
            "HCC input role membership is not exactly raw BAM + seven companions")
    pair_binding = run_artifacts.get("cooccurrence_pairs")
    require(
        isinstance(pair_binding, dict)
        and pair_binding.get("schema_name") == "longlineage.cooccurrence_pairs"
        and pair_binding.get("schema_version") == "1.0.1",
        "cooccurrence_pairs must use longlineage.cooccurrence_pairs schema 1.0.1",
    )

    return {
        "repo": str(repo),
        "run_root": str(run_root),
        "run_receipt": run,
        "validation_receipt": validation,
        "producer_receipt": producer,
        "summary": summary,
        "schema_catalog": catalog_contract,
        "science_parameters": science_contract,
        "artifact_records": run_artifacts,
        "semantic_rows": semantic_rows,
        "verified_files": verified_files,
        "observed_sha256": {
            "run_receipt": sha256_file(run_path),
            "validation_receipt": observed_validation_sha,
            "producer_receipt": observed_producer_sha,
            "checksums": observed_checksums_sha,
            "schema_catalog": sha256_file(catalog_path),
            "science_parameters": sha256_file(science_path),
        },
    }


def validate_input_authorities(
    dataset_gate_path: Path,
    production_path: Path,
    manifests: Mapping[str, Mapping[str, Any]],
) -> Dict[str, Any]:
    dataset_gate = read_json(dataset_gate_path, "dataset-gate input authority")
    require(
        dataset_gate.get("schema_name") == "longlineage.dataset_gate_input_authority"
        and dataset_gate.get("schema_version") == "1.0.0",
        "dataset-gate input authority schema mismatch",
    )
    require(dataset_gate.get("dataset_id") == EXPECTED_DATASET_ID,
            "dataset-gate input authority dataset mismatch")
    require(dataset_gate.get("truth_fields") == 0,
            "dataset-gate input authority contains truth fields")
    require(dataset_gate.get("tagged_bam_persisted") is False,
            "dataset-gate input authority claims a persisted tagged BAM")
    require(
        dataset_gate.get("latest_tag_join") == "EXACT_PROJECTION_NO_FALLBACK",
        "dataset-gate input authority permits a non-exact tag join",
    )
    claim = dataset_gate.get("claim")
    require(
        isinstance(claim, dict)
        and claim.get("production_seven_dataset_claim_allowed") is False
        and claim.get("cross_dataset_generalization_allowed") is False,
        "dataset-gate authority claim ceiling drifted",
    )
    authority_files_raw = dataset_gate.get("files")
    require(isinstance(authority_files_raw, list), "dataset-gate authority files missing")
    authority_files = {row.get("role"): row for row in authority_files_raw}
    require(
        len(authority_files) == len(authority_files_raw)
        and set(authority_files) == EXPECTED_INPUT_ROLES,
        "dataset-gate authority input role membership mismatch",
    )

    production = read_json(production_path, "production input authority")
    require(
        production.get("schema_name") == "longlineage.production_input_authority"
        and production.get("schema_version") == "1.0.0",
        "production input authority schema mismatch",
    )
    constraints = production.get("constraints")
    require(
        isinstance(constraints, dict)
        and constraints.get("latest_tag_join") == "EXACT_PROJECTION_NO_FALLBACK"
        and constraints.get("tagged_bam_output_allowed") is False,
        "production input authority tag contract drifted",
    )
    production_rows = [
        row for row in production.get("datasets", [])
        if row.get("dataset_id") == EXPECTED_DATASET_ID
    ]
    require(len(production_rows) == 1, "production authority HCC1395 row is not unique")
    production_hcc = production_rows[0]
    require(production_hcc.get("persisted_tagged_bam") is False,
            "production authority claims a persisted tagged BAM")

    for label, manifest in manifests.items():
        datasets = manifest.get("datasets")
        require(
            isinstance(datasets, list) and len(datasets) == 1
            and datasets[0].get("dataset_id") == EXPECTED_DATASET_ID
            and datasets[0].get("dataset_order") == 0,
            f"{label} manifest dataset scope is not exactly HCC1395",
        )
        manifest_files_raw = datasets[0].get("files")
        require(isinstance(manifest_files_raw, list), f"{label} manifest files missing")
        manifest_files = {row.get("role"): row for row in manifest_files_raw}
        require(
            len(manifest_files) == len(manifest_files_raw)
            and set(manifest_files) == EXPECTED_INPUT_ROLES,
            f"{label} manifest input role membership mismatch",
        )
        for role in EXPECTED_INPUT_ROLES:
            authority_row = authority_files[role]
            manifest_row = manifest_files[role]
            require(
                manifest_row.get("size_bytes") == authority_row.get("size_bytes")
                and manifest_row.get("sha256") == authority_row.get("sha256"),
                f"{label} manifest input differs from dataset-gate authority: {role}",
            )
        bindings = manifest.get("contract_bindings")
        require(isinstance(bindings, dict), f"{label} manifest contract_bindings missing")
        require(
            bindings.get("dataset_gate_input_authority_sha256")
            == sha256_file(dataset_gate_path),
            f"{label} manifest does not bind dataset-gate input authority",
        )
        require(
            bindings.get("production_input_authority_sha256")
            == sha256_file(production_path),
            f"{label} manifest does not bind production input authority",
        )

    for role, production_key in [
        ("latest_hp_ps_sidecar", "latest_hp_ps_sidecar"),
        ("latest_hp_ps_sidecar_index", "latest_hp_ps_sidecar_index"),
        ("pass_biallelic_ssnv_vcf", "pass_biallelic_ssnv_vcf"),
        ("pass_biallelic_ssnv_vcf_index", "pass_biallelic_ssnv_vcf_index"),
    ]:
        require(
            production_hcc.get(production_key, {}).get("size_bytes")
            == authority_files[role].get("size_bytes")
            and production_hcc.get(production_key, {}).get("sha256")
            == authority_files[role].get("sha256"),
            f"production/dataset-gate authority mismatch: {role}",
        )
    return {
        "dataset_gate": dataset_gate,
        "production": production,
        "dataset_gate_sha256": sha256_file(dataset_gate_path),
        "production_sha256": sha256_file(production_path),
    }


def validate_manifest(
    manifest_path: Path,
    label: str,
    workers: int,
    bundle: Mapping[str, Any],
) -> Dict[str, Any]:
    manifest = read_json(manifest_path, f"{label} production manifest")
    require_closed_object(
        manifest,
        {
            "schema_name",
            "schema_version",
            "authority_profile",
            "run_id",
            "output_root",
            "datasets",
            "runtime",
            "contract_bindings",
        },
        f"{label} production manifest",
    )
    require(
        manifest.get("schema_name") == "longlineage.production_manifest"
        and manifest.get("schema_version") == "1.1.0"
        and manifest.get("authority_profile") == "HCC1395_DATASET_GATE",
        f"{label} production manifest identity mismatch",
    )
    run = bundle["run_receipt"]
    producer = bundle["producer_receipt"]
    require(manifest.get("run_id") == run.get("run_id"),
            f"{label} manifest/run receipt run_id mismatch")
    output_root = manifest.get("output_root")
    require(
        isinstance(output_root, str)
        and Path(output_root).name == run.get("run_id")
        and Path(output_root).parent.name == ".staging",
        f"{label} manifest output_root is not the governed staging identity",
    )
    runtime = manifest.get("runtime")
    require(isinstance(runtime, dict), f"{label} manifest runtime missing")
    require(runtime.get("compute_workers") == workers,
            f"{label} compute_workers is not {workers}")
    observed_sha = sha256_file(manifest_path)
    require(
        observed_sha
        == run.get("manifest_sha256")
        == producer.get("manifest_sha256"),
        f"{label} manifest SHA-256 is not bound by producer/frozen run",
    )
    bindings = manifest.get("contract_bindings")
    require(
        bindings.get("science_parameters_sha256")
        == run.get("science_parameters_sha256")
        and bindings.get("schema_catalog_sha256")
        == run.get("schema_catalog_sha256"),
        f"{label} manifest science/schema contract bindings differ from frozen run",
    )
    return manifest


def validate_build_receipt(
    path: Path,
    bundles: Mapping[str, Mapping[str, Any]],
    expected_commit: str,
) -> Dict[str, Any]:
    receipt = read_json(path, "executable build receipt")
    require(
        receipt.get("schema_name")
        == "longlineage.hcc1395_executable_build_receipt"
        and receipt.get("schema_version") == "1.0.0",
        "executable build receipt schema mismatch",
    )
    require(receipt.get("git_commit") == expected_commit,
            "build receipt git commit mismatch")
    require(receipt.get("source_worktree_clean") is True,
            "build receipt source worktree was not clean")
    require(receipt.get("production_claim_allowed") is False,
            "build receipt illegally permits a production claim")
    executables = receipt.get("executables")
    require(isinstance(executables, dict), "build receipt executables missing")
    for key in ("producer", "validator", "governance"):
        require(isinstance(executables.get(key), dict),
                f"build receipt executable missing: {key}")
        row = executables[key]
        require(is_sha256(row.get("sha256")), f"build receipt {key} SHA-256 invalid")
        require(
            isinstance(row.get("size_bytes"), int) and row["size_bytes"] > 0,
            f"build receipt {key} size invalid",
        )
        if "path" in row:
            verified = verify_absolute_binding(
                {
                    "path": row["path"],
                    "size_bytes": row["size_bytes"],
                    "sha256": row["sha256"],
                },
                f"build receipt executable {key}",
            )
            require(verified["sha256"] == row["sha256"],
                    f"build receipt executable {key} replay failed")
    producer_shas = {
        bundle["producer_receipt"]["producer_executable_sha256"]
        for bundle in bundles.values()
    }
    validator_shas = {
        bundle["validation_receipt"]["validator_executable_sha256"]
        for bundle in bundles.values()
    }
    require(
        producer_shas == {executables["producer"]["sha256"]},
        "build receipt producer executable does not bind both runs",
    )
    require(
        validator_shas == {executables["validator"]["sha256"]},
        "build receipt validator executable does not bind both runs",
    )
    release = receipt.get("release_build")
    require(
        isinstance(release, dict)
        and release.get("build_type") == "Release"
        and release.get("warnings_as_errors") is True
        and release.get("require_exact_htslib") is True
        and release.get("htslib_version") == "1.18",
        "build receipt Release/HTSlib contract mismatch",
    )
    tests = receipt.get("ctest")
    require(
        isinstance(tests, dict)
        and tests.get("exit_code") == 0
        and isinstance(tests.get("total"), int)
        and tests.get("passed") == tests.get("total")
        and tests.get("failed") == 0,
        "build receipt clean CTest evidence is not all-pass",
    )
    return receipt


def compare_frozen_bundles(
    bundles: Mapping[str, Mapping[str, Any]],
    manifests: Mapping[str, Mapping[str, Any]],
) -> Dict[str, Any]:
    w24 = bundles["w24"]
    w40 = bundles["w40"]
    require(
        w24["run_receipt"]["run_id"] != w40["run_receipt"]["run_id"],
        "w24 and w40 must be distinct runs",
    )
    differences = json_pointer_differences(manifests["w24"], manifests["w40"])
    require(
        differences == EXPECTED_MANIFEST_DIFFERENCES,
        "manifest difference allowlist mismatch: "
        f"observed={differences} expected={EXPECTED_MANIFEST_DIFFERENCES}",
    )
    require(
        w24["producer_receipt"]["producer_executable_sha256"]
        == w40["producer_receipt"]["producer_executable_sha256"],
        "w24/w40 producer executable SHA-256 differs",
    )
    require(
        w24["validation_receipt"]["validator_executable_sha256"]
        == w40["validation_receipt"]["validator_executable_sha256"],
        "w24/w40 validator executable SHA-256 differs",
    )
    require(
        w24["run_receipt"]["input_snapshot_before_sha256"]
        == w40["run_receipt"]["input_snapshot_before_sha256"],
        "w24/w40 frozen input snapshot differs",
    )
    require(
        canonical_sha256(w24["producer_receipt"]["input_mount_identity"])
        == canonical_sha256(w40["producer_receipt"]["input_mount_identity"]),
        "w24/w40 input mount identity differs",
    )

    artifact_rows = []
    for artifact_id in EXPECTED_ARTIFACT_IDS:
        left = w24["artifact_records"].get(artifact_id)
        right = w40["artifact_records"].get(artifact_id)
        require(
            isinstance(left, dict) and isinstance(right, dict),
            f"determinism artifact missing: {artifact_id}",
        )
        projection_left = {
            key: left.get(key)
            for key in ("schema_name", "schema_version", "logical_rows", "semantic_sha256")
        }
        projection_right = {
            key: right.get(key)
            for key in ("schema_name", "schema_version", "logical_rows", "semantic_sha256")
        }
        require(
            projection_left == projection_right,
            f"w24/w40 science artifact semantic metadata differs: {artifact_id}",
        )
        artifact_rows.append({"artifact_id": artifact_id, **projection_left})

    summary_left = {
        key: w24["summary"][key] for key in EXPECTED_SUMMARY_PROJECTION_FIELDS
    }
    summary_right = {
        key: w40["summary"][key] for key in EXPECTED_SUMMARY_PROJECTION_FIELDS
    }
    require(summary_left == summary_right, "w24/w40 summary projection differs")
    return {
        "status": "PASS",
        "manifest_differences": differences,
        "artifacts": artifact_rows,
        "summary_projection_sha256": canonical_sha256(summary_left),
        "summary_projection": summary_left,
    }

def validate_audit_closed_shape(receipt: Mapping[str, Any]) -> None:
    require_closed_object(
        receipt,
        {
            "schema_name",
            "schema_version",
            "overall_status",
            "production_claim_allowed",
            "generator",
            "runs",
            "determinism",
            "historical",
            "checks",
        },
        "C++ audit receipt",
    )
    require_closed_object(
        receipt["generator"],
        {
            "language",
            "executable_sha256",
            "git_commit",
            "independent_of_producer_kernels",
            "reads_alignment_inputs",
        },
        "C++ audit generator",
    )
    runs = require_closed_object(receipt["runs"], {"w24", "w40"}, "C++ audit runs")
    for label in ("w24", "w40"):
        require_closed_object(
            runs[label],
            {
                "run_id",
                "compute_workers",
                "run_root",
                "manifest_path",
                "manifest_sha256",
                "run_receipt_sha256",
                "validation_receipt_sha256",
            },
            f"C++ audit {label} run",
        )
    determinism = require_closed_object(
        receipt["determinism"],
        {
            "status",
            "manifest_comparison",
            "artifacts",
            "summary_projection",
            "m2_conservation",
            "cooccurrence_replay",
        },
        "C++ audit determinism",
    )
    require_closed_object(
        determinism["manifest_comparison"],
        {
            "status",
            "allowed_differences",
            "normalized_sha256",
            "w24_compute_workers",
            "w40_compute_workers",
        },
        "C++ audit manifest comparison",
    )
    require(
        isinstance(determinism["artifacts"], list)
        and len(determinism["artifacts"]) == 8,
        "C++ audit artifacts must contain exactly eight rows",
    )
    for index, row in enumerate(determinism["artifacts"]):
        require_closed_object(
            row,
            {
                "artifact_id",
                "schema_name",
                "schema_version",
                "w24_logical_rows",
                "w40_logical_rows",
                "semantic_sha256",
                "semantic_equal",
                "physical_equal",
                "physical_comparison",
            },
            f"C++ audit artifact row {index}",
        )
    require_closed_object(
        determinism["summary_projection"],
        {
            "status",
            "semantic_equal",
            "semantic_sha256",
            "canonicalization",
            "scope",
            "counts",
            "phase_status",
        },
        "C++ audit summary projection",
    )
    m2 = require_closed_object(
        determinism["m2_conservation"],
        {"status", "w24", "w40"},
        "C++ audit M2 conservation",
    )
    m2_keys = {
        "m1_site_rows",
        "m1_stable_assignments",
        "cooccurrence_site_rows",
        "unstable_not_run",
        "m2_eligible",
        "m2_evaluable_ineligible",
        "m2_axis_indeterminate",
        "m2_group_count_gt10",
        "partition_total",
    }
    for label in ("w24", "w40"):
        require_closed_object(m2[label], m2_keys, f"C++ audit M2 replay {label}")
    cooccurrence = require_closed_object(
        determinism["cooccurrence_replay"],
        {"status", "authority", "interpretation", "w24", "w40"},
        "C++ audit co-occurrence replay",
    )
    for label in ("w24", "w40"):
        require_closed_object(
            cooccurrence[label],
            COOCCURRENCE_REPLAY_COUNTER_KEYS,
            f"C++ audit co-occurrence replay {label}",
        )

    historical = require_closed_object(
        receipt["historical"],
        {
            "source",
            "site_keys",
            "m1",
            "m2",
            "cooccurrence",
            "regional_topology",
            "runtime",
        },
        "C++ audit historical",
    )
    require_closed_object(
        historical["source"],
        {
            "path",
            "physical_sha256",
            "authority_profile",
            "selected_dataset",
            "selected_columns",
            "truth_derived_fields_consumed",
        },
        "C++ audit historical source",
    )
    require_closed_object(
        historical["site_keys"],
        {
            "status",
            "verdict",
            "canonical_rule",
            "old_count",
            "new_count",
            "old_ordered_sha256",
            "new_ordered_sha256",
            "old_sorted_set_sha256",
            "new_sorted_set_sha256",
            "missing",
            "extra",
            "old_duplicates",
            "new_duplicates",
        },
        "C++ audit historical site keys",
    )
    m1 = require_closed_object(
        historical["m1"],
        {
            "status",
            "verdict",
            "old_counts",
            "new_counts",
            "status_mismatches",
            "status_transitions",
            "stable_transition",
        },
        "C++ audit historical M1",
    )
    m1_count_keys = {
        "evaluable",
        "insufficient_alt_reads",
        "incomplete_distance_below_minimum",
        "stable",
    }
    require_closed_object(m1["old_counts"], m1_count_keys, "C++ audit old M1 counts")
    require_closed_object(m1["new_counts"], m1_count_keys, "C++ audit new M1 counts")
    require(
        isinstance(m1["status_transitions"], list)
        and len(m1["status_transitions"]) == 9,
        "C++ audit M1 status transition matrix must contain nine rows",
    )
    for index, row in enumerate(m1["status_transitions"]):
        require_closed_object(
            row, {"from", "to", "count"}, f"C++ audit M1 transition row {index}"
        )
    stable = require_closed_object(
        m1["stable_transition"],
        {
            "true_to_true",
            "true_to_false",
            "false_to_true",
            "false_to_false",
            "symmetric_difference",
            "jaccard",
        },
        "C++ audit M1 stable transition",
    )
    require_closed_object(
        stable["jaccard"], {"intersection", "union"}, "C++ audit M1 Jaccard"
    )
    not_comparable_keys = {"status", "verdict", "reason_code", "explanation"}
    for key in ("m2", "regional_topology", "runtime"):
        require_closed_object(
            historical[key], not_comparable_keys, f"C++ audit historical {key}"
        )
    require_closed_object(
        historical["cooccurrence"],
        {
            "status",
            "verdict",
            "reason_code",
            "explanation",
            "old_formal_result_exists",
            "new_pair_rows",
        },
        "C++ audit historical cooccurrence",
    )
    require(
        isinstance(receipt["checks"], list) and len(receipt["checks"]) == 8,
        "C++ audit checks must contain exactly eight rows",
    )
    for index, row in enumerate(receipt["checks"]):
        require_closed_object(
            row, {"check_id", "status", "evidence_sha256"},
            f"C++ audit check row {index}",
        )


def validate_audit_receipt(
    path: Path,
    bundles: Mapping[str, Mapping[str, Any]],
    comparison: Mapping[str, Any],
    expected_audit_commit: str,
    expected_audit_executable_sha256: str,
) -> Dict[str, Any]:
    """Validate the stable audit subset while the C++ schema remains independently owned."""

    receipt = read_json(path, "C++ determinism/historical receipt")
    validate_audit_closed_shape(receipt)
    require(
        receipt.get("schema_name")
        == "longlineage.hcc1395_determinism_historical_receipt"
        and receipt.get("schema_version") == "2.0.0",
        "C++ audit receipt schema mismatch",
    )
    require(receipt.get("overall_status") == "PASS",
            "C++ audit receipt overall_status is not PASS")
    require(receipt.get("production_claim_allowed") is False,
            "C++ audit receipt illegally permits a production claim")
    generator = receipt.get("generator")
    require(
        isinstance(generator, dict)
        and generator.get("language") == "C++17"
        and generator.get("independent_of_producer_kernels") is True
        and generator.get("reads_alignment_inputs") is False
        and generator.get("git_commit") == expected_audit_commit
        and generator.get("git_commit") != "0" * 40
        and generator.get("executable_sha256")
        == expected_audit_executable_sha256,
        "C++ audit generator identity/independence mismatch",
    )
    runs = receipt.get("runs")
    require(isinstance(runs, dict) and set(runs) == {"w24", "w40"},
            "C++ audit run membership mismatch")
    for label, workers in (("w24", 24), ("w40", 40)):
        run_row = runs[label]
        require(
            isinstance(run_row, dict)
            and run_row.get("run_id") == bundles[label]["run_receipt"]["run_id"]
            and run_row.get("compute_workers") == workers
            and Path(run_row.get("run_root", "")).resolve(strict=True)
            == Path(bundles[label]["run_root"]).resolve(strict=True)
            and run_row.get("run_receipt_sha256")
            == bundles[label]["observed_sha256"]["run_receipt"],
            f"C++ audit {label} run binding mismatch",
        )
        audit_manifest_path = regular_absolute_file(
            run_row["manifest_path"], f"C++ audit {label} manifest"
        )
        require(
            run_row["manifest_sha256"]
            == sha256_file(audit_manifest_path)
            == bundles[label]["run_receipt"]["manifest_sha256"],
            f"C++ audit {label} manifest binding mismatch",
        )
        require(
            run_row["validation_receipt_sha256"]
            == bundles[label]["observed_sha256"]["validation_receipt"],
            f"C++ audit {label} validation receipt binding mismatch",
        )

    determinism = receipt.get("determinism")
    require(isinstance(determinism, dict) and determinism.get("status") == "PASS",
            "C++ audit determinism status is not PASS")
    manifest_comparison = determinism["manifest_comparison"]
    require(
        manifest_comparison["status"] == "PASS"
        and manifest_comparison["allowed_differences"]
        == ["run_id", "output_root", "runtime.compute_workers"]
        and manifest_comparison["w24_compute_workers"] == 24
        and manifest_comparison["w40_compute_workers"] == 40
        and is_sha256(manifest_comparison["normalized_sha256"]),
        "C++ audit manifest comparison contract mismatch",
    )
    audit_artifacts_raw = determinism.get("artifacts")
    require(isinstance(audit_artifacts_raw, list),
            "C++ audit determinism artifacts missing")
    audit_artifacts = {row.get("artifact_id"): row for row in audit_artifacts_raw}
    require(
        len(audit_artifacts) == len(audit_artifacts_raw)
        and list(row.get("artifact_id") for row in audit_artifacts_raw)
        == EXPECTED_ARTIFACT_IDS,
        "C++ audit determinism artifact order/membership mismatch",
    )
    expected_rows = {row["artifact_id"]: row for row in comparison["artifacts"]}
    for artifact_id in EXPECTED_ARTIFACT_IDS:
        row = audit_artifacts[artifact_id]
        expected = expected_rows[artifact_id]
        require(row.get("semantic_equal") is True,
                f"C++ audit semantic_equal is not true: {artifact_id}")
        for field in ("schema_name", "schema_version", "semantic_sha256"):
            require(
                row.get(field) == expected[field],
                f"C++ audit artifact {field} mismatch: {artifact_id}",
            )
        require(
            row.get("w24_logical_rows") == expected["logical_rows"]
            and row.get("w40_logical_rows") == expected["logical_rows"]
            and row.get("physical_comparison") == "DIAGNOSTIC_ONLY"
            and isinstance(row.get("physical_equal"), bool),
            f"C++ audit artifact w24/w40 row/physical metadata mismatch: {artifact_id}",
        )
    summary_projection = determinism.get("summary_projection")
    require(
        isinstance(summary_projection, dict)
        and summary_projection.get("status") == "PASS"
        and summary_projection.get("semantic_equal") is True
        and summary_projection.get("canonicalization")
        == "COMPACT_SORTED_JSON_NO_LF over {counts,phase_status,scope}"
        and summary_projection.get("semantic_sha256")
        == comparison["summary_projection_sha256"],
        "C++ audit summary projection mismatch",
    )
    require(
        summary_projection["scope"]
        == comparison["summary_projection"]["scope"]
        and summary_projection["counts"]
        == comparison["summary_projection"]["counts"]
        and summary_projection["phase_status"]
        == comparison["summary_projection"]["phase_status"],
        "C++ audit summary projection payload differs from frozen runs",
    )
    m2_conservation = determinism.get("m2_conservation")
    require(
        isinstance(m2_conservation, dict)
        and m2_conservation.get("status") == "PASS",
        "C++ audit M2 conservation status is not PASS",
    )
    for label in ("w24", "w40"):
        replay = m2_conservation[label]
        run_counts = bundles[label]["summary"]["counts"]
        require(
            replay["m1_site_rows"] == run_counts["site_keys"]
            and replay["m1_stable_assignments"]
            == run_counts["m1_stable_assignments"]
            and replay["cooccurrence_site_rows"] == run_counts["site_keys"]
            and replay["unstable_not_run"]
            == run_counts["site_keys"] - run_counts["m1_stable_assignments"]
            and replay["m2_eligible"] == run_counts["m2_eligible"]
            and replay["m2_evaluable_ineligible"]
            == run_counts["m2_evaluable_ineligible"]
            and replay["m2_axis_indeterminate"]
            == run_counts["m2_axis_indeterminate"]
            and replay["m2_group_count_gt10"]
            == run_counts["m2_group_count_gt10"]
            and replay["partition_total"]
            == run_counts["m1_stable_assignments"],
            f"C++ audit M2 replay differs from frozen summary: {label}",
        )
    cooccurrence_replay = determinism.get("cooccurrence_replay")
    require(
        isinstance(cooccurrence_replay, dict)
        and cooccurrence_replay.get("status") == "PASS"
        and cooccurrence_replay.get("authority")
        == "VALIDATED_SERIALIZED_CENSUS_NOT_PQ_RECOMPUTATION"
        and cooccurrence_replay.get("interpretation")
        == "PAIR_ROWS_ARE_RECORDS_NOT_POSITIVE_DISCOVERIES",
        "C++ audit co-occurrence serialized-census authority boundary mismatch",
    )
    for label in ("w24", "w40"):
        replay = cooccurrence_replay[label]
        require(
            all(type(replay[key]) is int and replay[key] >= 0
                for key in COOCCURRENCE_REPLAY_COUNTER_KEYS),
            f"C++ audit co-occurrence replay has invalid counters: {label}",
        )
        family_total = (
            replay["ineligible_m2_screen_pairs"]
            + replay["eligible_endpoint_a_not_testable_pairs"]
            + replay["eligible_exact_not_identifiable_pairs"]
            + replay["eligible_exact_family_pairs"]
        )
        require(
            replay["family_partition_total"] == family_total
            == replay["pair_rows"]
            and replay["fdr_family_size"]
            == replay["eligible_exact_family_pairs"]
            and replay["eligible_exact_family_pairs"]
            <= replay["exact_identifiable_pairs"]
            <= replay["pair_rows"]
            and replay["formal_pair_by_confirmed"]
            <= replay["global_by_discoveries"]
            <= replay["global_bh_discoveries"]
            <= replay["eligible_exact_family_pairs"],
            f"C++ audit co-occurrence family/discovery hierarchy failed: {label}",
        )
        require(
            replay["partner_universe_pair_rows"] == replay["pair_rows"]
            == bundles[label]["artifact_records"]["cooccurrence_pairs"][
                "logical_rows"
            ]
            and replay["cooccurrence_site_rows"]
            == bundles[label]["artifact_records"]["cooccurrence_sites"][
                "logical_rows"
            ]
            == m2_conservation[label]["cooccurrence_site_rows"]
            and replay["joint_signature_partition_total"]
            == replay["joint_signature_pass_sites"]
            + replay["joint_signature_not_testable_sites"]
            == replay["cooccurrence_site_rows"]
            and replay["joint_signature_pass_sites"]
            == replay["topology_units"]
            == bundles[label]["artifact_records"]["topology_units"][
                "logical_rows"
            ]
            == bundles[label]["summary"]["counts"]["topology_primary_hp_units"],
            f"C++ audit co-occurrence pair/site/topology-unit conservation failed: {label}",
        )
    require(
        cooccurrence_replay["w24"] == cooccurrence_replay["w40"],
        "C++ audit co-occurrence replay counters differ between w24 and w40",
    )

    historical = receipt.get("historical")
    require(isinstance(historical, dict), "C++ audit historical section missing")
    historical_source = historical["source"]
    source_path = regular_absolute_file(
        historical_source["path"], "C++ audit historical source"
    )
    require(
        sha256_file(source_path) == historical_source["physical_sha256"]
        and historical_source["selected_dataset"] == EXPECTED_DATASET_ID
        and historical_source["truth_derived_fields_consumed"] == 0
        and historical_source["selected_columns"]
        == [
            "dataset",
            "chrom",
            "pos",
            "ref",
            "alt",
            "analysis_status",
            "stable_null_multigroup",
        ],
        "C++ audit historical source binding/selection mismatch",
    )
    site_keys = historical.get("site_keys")
    require(
        isinstance(site_keys, dict)
        and site_keys.get("status") == "PASS"
        and site_keys.get("verdict") == "EXACT"
        and site_keys.get("canonical_rule")
        == "UTF-8 dataset\\tchrom\\tpos\\tref\\talt\\n; no header; LF; original row order"
        and site_keys.get("old_count") == EXPECTED_SITE_KEYS
        and site_keys.get("new_count") == EXPECTED_SITE_KEYS
        and site_keys.get("missing") == 0
        and site_keys.get("extra") == 0
        and site_keys.get("old_duplicates") == 0
        and site_keys.get("new_duplicates") == 0
        and site_keys.get("old_ordered_sha256")
        == site_keys.get("new_ordered_sha256")
        and site_keys.get("old_sorted_set_sha256")
        == site_keys.get("new_sorted_set_sha256"),
        "C++ audit historical site-key EXACT comparison failed",
    )
    m1 = historical.get("m1")
    require(
        isinstance(m1, dict)
        and m1.get("status") == "PASS"
        and m1.get("verdict") == "COMPARABLE_DIFFERENT",
        "C++ audit historical M1 must be COMPARABLE_DIFFERENT",
    )
    for key in ("old_counts", "new_counts", "stable_transition"):
        require(isinstance(m1.get(key), dict), f"C++ audit historical M1 {key} missing")
    new_counts = m1["new_counts"]
    current_counts = bundles["w24"]["summary"]["counts"]
    require(
        new_counts.get("evaluable") == current_counts["m1_evaluable"]
        and new_counts.get("insufficient_alt_reads")
        == current_counts["m1_insufficient_alt_reads"]
        and new_counts.get("incomplete_distance_below_minimum")
        == current_counts["m1_incomplete_distance"]
        and new_counts.get("stable") == current_counts["m1_stable_assignments"],
        "C++ audit historical M1 new census differs from frozen run",
    )
    require(m1.get("status_mismatches") == 0,
            "C++ audit historical M1 status mismatch is non-zero")
    require(
        sum(m1["old_counts"][key] for key in (
            "evaluable",
            "insufficient_alt_reads",
            "incomplete_distance_below_minimum",
        ))
        == EXPECTED_SITE_KEYS
        and sum(m1["new_counts"][key] for key in (
            "evaluable",
            "insufficient_alt_reads",
            "incomplete_distance_below_minimum",
        ))
        == EXPECTED_SITE_KEYS,
        "C++ audit historical M1 status counts do not conserve site keys",
    )
    transition_pairs = {
        (row["from"], row["to"]) for row in m1["status_transitions"]
    }
    require(
        len(transition_pairs) == 9
        and sum(row["count"] for row in m1["status_transitions"])
        == EXPECTED_SITE_KEYS,
        "C++ audit historical M1 status transition matrix is incomplete",
    )
    stable = m1["stable_transition"]
    require(
        stable["true_to_true"] + stable["true_to_false"]
        == m1["old_counts"]["stable"]
        and stable["true_to_true"] + stable["false_to_true"]
        == m1["new_counts"]["stable"]
        and stable["true_to_false"] + stable["false_to_true"]
        == stable["symmetric_difference"]
        and stable["jaccard"]["intersection"] == stable["true_to_true"]
        and stable["jaccard"]["union"]
        == stable["true_to_true"]
        + stable["true_to_false"]
        + stable["false_to_true"],
        "C++ audit historical M1 stable transition does not conserve",
    )
    m2 = historical.get("m2")
    cooccurrence = historical.get("cooccurrence")
    regional = historical.get("regional_topology")
    runtime = historical.get("runtime")
    require(
        isinstance(m2, dict)
        and m2.get("status") == "PASS"
        and m2.get("verdict") == "NOT_COMPARABLE_METHOD_CHANGED"
        and m2.get("reason_code") == "HISTORICAL_SCREENING_AGGREGATE_ONLY",
        "C++ audit historical M2 section missing",
    )
    require(
        isinstance(cooccurrence, dict)
        and cooccurrence.get("status") == "PASS"
        and cooccurrence.get("verdict")
        == "NOT_COMPARABLE_NO_FORMAL_OLD_RESULT"
        and cooccurrence.get("reason_code")
        == "NO_SUCCESSFUL_FORMAL_HISTORICAL_AUTHORITY",
        "C++ audit historical cooccurrence section missing",
    )
    require(cooccurrence.get("old_formal_result_exists") is False,
            "C++ audit fabricated a historical formal cooccurrence result")
    require(
        cooccurrence.get("new_pair_rows")
        == cooccurrence_replay["w24"]["pair_rows"]
        == bundles["w24"]["artifact_records"]["cooccurrence_pairs"]["logical_rows"],
        "C++ audit new cooccurrence pair count differs from frozen artifact",
    )
    require(
        isinstance(regional, dict)
        and regional.get("status") == "PASS"
        and regional.get("verdict")
        == "NOT_COMPARABLE_METHOD_AND_GATE_CHANGED"
        and regional.get("reason_code")
        == "HISTORICAL_TOPOLOGY_USED_DIFFERENT_METHOD_CN_LOH_BOUNDARY",
        "C++ audit historical regional topology section missing",
    )
    require(
        isinstance(runtime, dict)
        and runtime.get("status") == "PASS"
        and runtime.get("verdict")
        == "NOT_COMPARABLE_PROGRAM_SCOPE_THREAD_OUTPUT_CHANGED"
        and runtime.get("reason_code") == "NO_MATCHED_RUNTIME_DENOMINATOR",
        "C++ audit historical runtime section missing",
    )
    check_ids = tuple(row["check_id"] for row in receipt["checks"])
    require(
        check_ids == EXPECTED_AUDIT_CHECK_SEQUENCE
        and all(
            row["status"] == "PASS" and is_sha256(row["evidence_sha256"])
            for row in receipt["checks"]
        ),
        "C++ audit checks are not the fixed-order eight all-PASS rows",
    )
    return receipt


def load_execution_evidence(
    evidence_path: Path,
    repo: Path,
    run_roots: Mapping[str, Path],
    audit_path: Path,
    expected_builder_path: Path,
) -> Dict[str, Any]:
    evidence = read_json(evidence_path, "execution evidence")
    require_closed_object(
        evidence,
        {
            "schema_name",
            "schema_version",
            "evidence_id",
            "created_at",
            "dataset_id",
            "report_scope",
            "production_claim_allowed",
            "truth_fields",
            "tagged_bam_persisted",
            "latest_tag_join",
            "software_provenance",
            "runs",
            "authorities",
            "audit_receipt",
            "determinism_contract",
        },
        "execution evidence",
    )
    require(
        evidence.get("schema_name") == "longlineage.hcc1395_execution_evidence"
        and evidence.get("schema_version") == "2.0.0",
        "execution evidence schema mismatch",
    )
    require(
        isinstance(evidence.get("evidence_id"), str)
        and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}",
                         evidence["evidence_id"]) is not None,
        "execution evidence evidence_id is invalid",
    )
    require(
        isinstance(evidence.get("created_at"), str)
        and re.fullmatch(
            r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})",
            evidence["created_at"],
        ) is not None,
        "execution evidence created_at is not RFC3339-shaped",
    )
    require(
        evidence.get("dataset_id") == EXPECTED_DATASET_ID
        and evidence.get("report_scope") == EXPECTED_REPORT_SCOPE
        and evidence.get("production_claim_allowed") is False
        and evidence.get("truth_fields") == 0
        and evidence.get("tagged_bam_persisted") is False
        and evidence.get("latest_tag_join") == "EXACT_PROJECTION_NO_FALLBACK",
        "execution evidence scope/truth/tag contract mismatch",
    )
    software = require_closed_object(
        evidence.get("software_provenance"),
        {"science_execution", "audit_report_generation"},
        "execution evidence software_provenance",
    )
    science = require_closed_object(
        software["science_execution"],
        {"repository", "executable_build_receipt"},
        "execution evidence science_execution",
    )
    audit_report = require_closed_object(
        software["audit_report_generation"],
        {
            "repository",
            "source_worktree_clean",
            "audit_executable",
            "report_builder_source",
            "release_build",
            "ctest",
            "ctest_log",
        },
        "execution evidence audit_report_generation",
    )
    science_repo = require_closed_object(
        science["repository"], {"path", "git_commit"},
        "execution evidence science repository",
    )
    audit_repo = require_closed_object(
        audit_report["repository"], {"path", "git_commit"},
        "execution evidence audit/report repository",
    )
    require(
        Path(science_repo["path"]).resolve(strict=True) == repo.resolve(strict=True)
        and Path(audit_repo["path"]).resolve(strict=True) == repo.resolve(strict=True),
        "execution evidence repository path differs from --repo",
    )
    science_commit = safe_identifier(
        science_repo["git_commit"], "execution evidence science git_commit"
    )
    audit_commit = safe_identifier(
        audit_repo["git_commit"], "execution evidence audit/report git_commit"
    )
    require(
        re.fullmatch(r"[0-9a-f]{40}", science_commit) is not None
        and re.fullmatch(r"[0-9a-f]{40}", audit_commit) is not None
        and science_commit != "0" * 40
        and audit_commit != "0" * 40,
        "execution evidence git commit is invalid or all-zero",
    )
    require(
        audit_report["source_worktree_clean"] is True,
        "execution evidence audit/report source worktree was not clean",
    )
    require(
        git_output(repo, "rev-parse", "HEAD") == audit_commit,
        "audit/report checkout HEAD differs from execution evidence",
    )
    require(
        git_output(repo, "status", "--porcelain=v1") == "",
        "audit/report source worktree is not clean",
    )
    git_output(repo, "cat-file", "-e", f"{science_commit}^{{commit}}")
    git_output(repo, "cat-file", "-e", f"{audit_commit}^{{commit}}")
    try:
        ancestor = subprocess.run(
            ["git", "-C", str(repo), "merge-base", "--is-ancestor",
             science_commit, audit_commit],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
    except OSError as error:
        raise EvidenceError(f"git merge-base invocation failed: {error}") from error
    require(
        ancestor.returncode == 0,
        "science execution commit is not an ancestor of audit/report commit",
    )
    audit_executable = verify_absolute_binding(
        audit_report["audit_executable"],
        "execution evidence audit executable",
    )
    builder_source = verify_absolute_binding(
        audit_report["report_builder_source"],
        "execution evidence report builder source",
    )
    require(
        Path(builder_source["path"]).resolve(strict=True)
        == expected_builder_path.resolve(strict=True),
        "execution evidence report builder differs from the executing source",
    )
    try:
        builder_relative = Path(builder_source["path"]).resolve(
            strict=True
        ).relative_to(repo.resolve(strict=True))
    except ValueError as error:
        raise EvidenceError(
            "execution evidence report builder is outside the repository"
        ) from error
    git_output(repo, "ls-files", "--error-unmatch", builder_relative.as_posix())
    expected_blob = git_output(
        repo, "rev-parse", f"{audit_commit}:{builder_relative.as_posix()}"
    )
    observed_blob = git_output(repo, "hash-object", builder_relative.as_posix())
    require(
        expected_blob == observed_blob,
        "executing report builder differs from the audit/report commit blob",
    )
    release = require_closed_object(
        audit_report["release_build"],
        {
            "build_type",
            "warnings_as_errors",
            "require_exact_htslib",
            "htslib_version",
        },
        "execution evidence audit release build",
    )
    require(
        release["build_type"] == "Release"
        and release["warnings_as_errors"] is True
        and release["require_exact_htslib"] is True
        and release["htslib_version"] == "1.18",
        "execution evidence audit release build contract mismatch",
    )
    ctest = require_closed_object(
        audit_report["ctest"],
        {"total", "passed", "failed", "exit_code"},
        "execution evidence audit CTest",
    )
    require(
        isinstance(ctest["total"], int)
        and ctest["total"] > 0
        and ctest["passed"] == ctest["total"]
        and ctest["failed"] == 0
        and ctest["exit_code"] == 0,
        "execution evidence audit CTest is not all-pass",
    )
    ctest_log = verify_absolute_binding(
        audit_report["ctest_log"], "execution evidence audit CTest log"
    )
    try:
        ctest_text = Path(ctest_log["path"]).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise EvidenceError(f"audit CTest log is not readable UTF-8: {error}") from error
    ctest_summaries = re.findall(
        r"100% tests passed, 0 tests failed out of ([0-9]+)", ctest_text
    )
    require(
        ctest_summaries
        and int(ctest_summaries[-1]) == ctest["total"],
        "execution evidence audit CTest log does not confirm the declared total",
    )
    contract = require_closed_object(
        evidence.get("determinism_contract"),
        {
            "artifact_ids",
            "manifest_difference_pointers",
            "summary_projection_fields",
        },
        "execution evidence determinism_contract",
    )
    require(contract["artifact_ids"] == EXPECTED_ARTIFACT_IDS,
            "execution evidence artifact contract drifted")
    require(contract["manifest_difference_pointers"] == EXPECTED_MANIFEST_DIFFERENCES,
            "execution evidence manifest difference contract drifted")
    require(contract["summary_projection_fields"] == EXPECTED_SUMMARY_PROJECTION_FIELDS,
            "execution evidence summary projection contract drifted")

    runs = require_closed_object(evidence.get("runs"), {"w24", "w40"},
                                 "execution evidence runs")
    verified_runs: Dict[str, Any] = {}
    for label, workers in (("w24", 24), ("w40", 40)):
        row = require_closed_object(
            runs[label],
            {
                "label",
                "compute_workers",
                "run_root",
                "manifest",
                "producer_execution",
                "validator_execution",
            },
            f"execution evidence {label}",
        )
        require(row["label"] == label and row["compute_workers"] == workers,
                f"execution evidence {label} label/worker mismatch")
        evidence_root = Path(row["run_root"])
        require(
            evidence_root.is_absolute()
            and evidence_root.resolve(strict=True) == run_roots[label].resolve(strict=True),
            f"execution evidence {label} run root differs from explicit CLI root",
        )
        require(not evidence_root.is_symlink(),
                f"execution evidence {label} run root must not be a symlink")
        verified_runs[label] = {
            "run_root": str(evidence_root.resolve(strict=True)),
            "compute_workers": workers,
            "manifest": verify_absolute_binding(
                row["manifest"], f"execution evidence {label} manifest"
            ),
            "producer_execution": verify_timed_execution(
                row["producer_execution"], f"{label} producer execution"
            ),
            "validator_execution": verify_timed_execution(
                row["validator_execution"], f"{label} validator execution"
            ),
        }

    authorities = require_closed_object(
        evidence.get("authorities"),
        {"dataset_gate_input", "production_input"},
        "execution evidence authorities",
    )
    verified_authorities = {
        key: verify_absolute_binding(value, f"execution evidence authority {key}")
        for key, value in authorities.items()
    }
    build_binding = verify_absolute_binding(
        science["executable_build_receipt"],
        "execution evidence science build receipt",
    )
    audit_binding = verify_absolute_binding(
        evidence["audit_receipt"], "execution evidence audit receipt"
    )
    require(
        Path(audit_binding["path"]) == audit_path.resolve(strict=True),
        "execution evidence audit receipt differs from --audit-receipt",
    )
    return {
        "document": evidence,
        "sha256": sha256_file(evidence_path),
        "path": str(evidence_path.resolve(strict=True)),
        "science_git_commit": science_commit,
        "audit_report_git_commit": audit_commit,
        "audit_executable": audit_executable,
        "report_builder_source": builder_source,
        "audit_release_build": dict(release),
        "audit_ctest": dict(ctest),
        "audit_ctest_log": ctest_log,
        "runs": verified_runs,
        "authorities": verified_authorities,
        "build_receipt": build_binding,
        "audit_receipt": audit_binding,
    }


def validate_dual_evidence_chain(
    repo: Path,
    run_roots: Mapping[str, Path],
    evidence_path: Path,
    audit_path: Path,
    expected_builder_path: Path | None = None,
) -> Dict[str, Any]:
    builder_path = (
        Path(__file__).resolve(strict=True)
        if expected_builder_path is None
        else expected_builder_path.resolve(strict=True)
    )
    execution = load_execution_evidence(
        evidence_path, repo, run_roots, audit_path, builder_path
    )
    bundles = {
        label: load_verified_bundle(repo, run_roots[label])
        for label in ("w24", "w40")
    }
    manifests = {}
    for label, workers in (("w24", 24), ("w40", 40)):
        manifests[label] = validate_manifest(
            Path(execution["runs"][label]["manifest"]["path"]),
            label,
            workers,
            bundles[label],
        )
    authorities = validate_input_authorities(
        Path(execution["authorities"]["dataset_gate_input"]["path"]),
        Path(execution["authorities"]["production_input"]["path"]),
        manifests,
    )
    comparison = compare_frozen_bundles(bundles, manifests)
    build_receipt = validate_build_receipt(
        Path(execution["build_receipt"]["path"]),
        bundles,
        execution["science_git_commit"],
    )
    for label in ("w24", "w40"):
        production_executable = bundles[label]["run_receipt"]["production_executable"]
        require(
            production_executable.get("git_commit")
            == execution["science_git_commit"],
            f"{label} frozen run git commit differs from execution evidence",
        )
    audit = validate_audit_receipt(
        Path(execution["audit_receipt"]["path"]),
        bundles,
        comparison,
        execution["audit_report_git_commit"],
        execution["audit_executable"]["sha256"],
    )
    return {
        "execution": execution,
        "bundles": bundles,
        "manifests": manifests,
        "authorities": authorities,
        "build_receipt": build_receipt,
        "audit_receipt": audit,
        "comparison": comparison,
    }


def historical_paths(historical_root: Path) -> Dict[str, Tuple[Path, str]]:
    root = historical_root.resolve(strict=True)
    require(root.is_dir(), f"historical root is not a directory: {root}")
    output_root = root.parent / "big7_disk_output"
    research = (
        root
        / "research/20260715_all_ssnv_focal_alt_multigroup_cooccurrence_validation"
    )
    m1_root = (
        output_root
        / "synthesis/observation_workspaces/"
        "20260715_all_ssnv_focal_alt_multigroup_cooccurrence_validation/"
        "all_ssnv_focal_alt_multigroup_v10_source_locked_thread_pinned_recovered_full"
    )
    region_root = (
        output_root
        / "synthesis/research_rounds/"
        "20260713_layered_reconstruction_v3_raw_all_lps_pass_v5"
    )
    return {
        "extraction_verification": (
            research / "results/all_ssnv_intersubmod_batch.v2_verification_fix.json",
            "78247190a3c348da4187943064b517457e3faa34ca06e859ca8c7a85e6e3339f",
        ),
        "m1_summary": (
            m1_root / "all_ssnv_summary.json",
            "454949afd1081f7e9475a3893d5ef0bde37c7e0d2b553fd231c8d8ae9fdf1f80",
        ),
        "m1_run_manifest": (
            m1_root / "run_manifest.json",
            "7cc16dae110bf9371378a07290195b210d6bdd0b5507616915884e1df49881b2",
        ),
        "m2_gate_recount": (
            research / "results/independent_m2_gate_recount.v3.json",
            "4a0f411b9ce39128d07e5d15abfc5a5c999797b8a3f1eb33886185e830031daf",
        ),
        "m2_failed_incident": (
            research
            / "results/cooccurrence_execution_incident.v3_attempt6_summary_totality.json",
            "af06b2df2226b46b6baac3874ee2cb8e6939d483be9dde99da94ba2a098125c4",
        ),
        "regional_verification": (
            region_root / "verification_summary.json",
            "9cdce65e7cb7488c1ae7b51bf0f5c87cbafee53605442503887dc7cf81bf983e",
        ),
        "regional_manifest": (
            region_root / "samples/HCC1395/output_manifest.json",
            "b208aa0bf4e697ea5d5877ff6fd0b12b2bf152c108db92b0e75ec45ea44eada6",
        ),
        "regional_reconstruction": (
            region_root / "samples/HCC1395/layered_reconstruction_HCC1395.json",
            "666d2d8a54322e0b7e1fa174c7b6ff2878891c4c88243669c05b63695ef82099",
        ),
        "regional_view": (
            region_root / "samples/HCC1395/layered_region_view_HCC1395.json",
            "9e28d98087d73cf4ebbe2185ab8cb4159bbc382799291c8c4025adc55f34c9d8",
        ),
    }


def load_historical_authority(historical_root: Path) -> Dict[str, Any]:
    paths = historical_paths(historical_root)
    verified: Dict[str, Dict[str, Any]] = {}
    documents: Dict[str, Dict[str, Any]] = {}
    for authority_id, (path, expected_sha) in paths.items():
        require(path.is_file() and not path.is_symlink(),
                f"historical authority missing: {path}")
        observed_sha = sha256_file(path)
        require(observed_sha == expected_sha,
                f"historical authority SHA-256 mismatch: {authority_id}: "
                f"observed={observed_sha} expected={expected_sha}")
        verified[authority_id] = {
            "path": str(path),
            "size_bytes": path.stat().st_size,
            "sha256": observed_sha,
        }
        if authority_id not in {"regional_reconstruction"}:
            documents[authority_id] = read_json(path, f"historical {authority_id}")

    extraction = documents["extraction_verification"]
    hcc_receipts = [
        row for row in extraction.get("receipts", []) if row.get("sample") == EXPECTED_DATASET_ID
    ]
    require(len(hcc_receipts) == 1, "historical extraction HCC1395 receipt is not unique")
    extraction_hcc = hcc_receipts[0]
    require(extraction_hcc.get("pass") is True
            and extraction_hcc.get("validation", {}).get("expected_vcf_sites")
            == EXPECTED_SITE_KEYS,
            "historical extraction HCC1395 census did not pass")

    m1_summary = documents["m1_summary"]
    m1_hcc = m1_summary.get("per_dataset", {}).get(EXPECTED_DATASET_ID)
    require(isinstance(m1_hcc, dict), "historical M1 HCC1395 summary missing")
    require(m1_hcc.get("n_sites") == EXPECTED_SITE_KEYS, "historical M1 site count drifted")
    require(m1_hcc.get("strict_confirm_status_counts") == {"NOT_RUN": EXPECTED_SITE_KEYS},
            "historical strict-confirm status is no longer all NOT_RUN")

    m1_manifest = documents["m1_run_manifest"]
    require(m1_manifest.get("status") == "EXECUTION_PASS"
            and m1_manifest.get("pass_semantics")
            == "execution_integrity_only_not_scientific_confirmation",
            "historical M1 manifest semantics drifted")
    require(
        m1_manifest.get("outputs", {}).get("summary", {}).get("sha256")
        == verified["m1_summary"]["sha256"],
        "historical M1 manifest does not bind the summary",
    )

    m2_recount = documents["m2_gate_recount"]
    m2_hcc = m2_recount.get("per_dataset", {}).get(EXPECTED_DATASET_ID)
    require(isinstance(m2_hcc, dict) and m2_hcc.get("m1_stable_rows")
            == m1_hcc.get("n_stable_null_multigroup"),
            "historical M2 census does not conserve HCC1395 M1 stable rows")
    incident = documents["m2_failed_incident"]
    require(
        incident.get("scientific_result") is False
        and incident.get("requested_output", {}).get("formal_run_receipt_created") is False
        and incident.get("requested_output", {}).get("formal_release_receipt_created") is False,
        "historical co-occurrence incident no longer proves absence of a formal result",
    )

    regional_verification = documents["regional_verification"]
    require(regional_verification.get("all_pass") is True,
            "historical regional verification is not all-pass")
    regional_manifest = documents["regional_manifest"]
    outputs = {row.get("role"): row for row in regional_manifest.get("outputs", [])}
    require(
        outputs.get("layered_reconstruction", {}).get("sha256")
        == verified["regional_reconstruction"]["sha256"]
        and outputs.get("layered_region_view", {}).get("sha256")
        == verified["regional_view"]["sha256"],
        "historical regional manifest output bindings drifted",
    )
    regional_view = documents["regional_view"]
    require(regional_view.get("sample") == EXPECTED_DATASET_ID,
            "historical regional view sample mismatch")
    census = regional_view.get("census")
    require(isinstance(census, dict), "historical regional census missing")

    return {
        "verified_files": verified,
        "extraction": {
            "wall_seconds": 7061.9845,
            "site_keys": extraction_hcc["validation"]["expected_vcf_sites"],
            "read_observations": 5_997_878,
            "valid_read_pairs": 251_517_047,
            "invalid_read_pairs": 1_377_718,
            "meaning": "BERNOULLI read×read distance pairs；不是 sSNV 共現 pairs。",
        },
        "m1": {
            "site_keys": m1_hcc["n_sites"],
            "evaluable": m1_hcc["status_counts"]["evaluable"],
            "insufficient_alt_reads": m1_hcc["status_counts"]["insufficient_alt_reads"],
            "incomplete_distance": m1_hcc["status_counts"][
                "incomplete_distance_below_minimum"
            ],
            "stable_assignments": m1_hcc["n_stable_null_multigroup"],
            "strict_confirm_not_run": m1_hcc["strict_confirm_status_counts"]["NOT_RUN"],
        },
        "m2": {
            "eligible": m2_hcc.get("eligible", 0),
            "evaluable_ineligible": m2_hcc.get("evaluable_ineligible", 0),
            "axis_indeterminate": m2_hcc.get("not_evaluable_axis_indeterminate", 0),
            "group_count_gt10": m2_hcc.get("not_evaluable_group_count_gt10", 0),
            "formal_scientific_result": False,
            "failed_elapsed": incident["execution_window"]["approximate_elapsed"],
            "failure": f"{incident['failure']['exception']}: {incident['failure']['message']}",
        },
        "regional": {
            "regions": census["n_regions"],
            "units": census["L1"]["n_units_total_including_unphased"],
            "primary_lineage_units": census["L1"]["n_primary_lineage_units"],
            "region_determinacy": census["region_determinacy"],
            "cn_source": regional_view.get("copy_number_contract", {}).get("source"),
            "method_scope": regional_view.get("analysis_contract", {}).get("claim_scope"),
        },
    }


def fmt_int(value: int) -> str:
    return f"{value:,}"


def fmt_bytes(value: int) -> str:
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    amount = float(value)
    unit = units[0]
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            break
        amount /= 1024.0
    return f"{amount:,.2f} {unit}"


def fmt_duration(seconds: float) -> str:
    seconds = float(seconds)
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    remain = seconds % 60
    if hours:
        return f"{hours:d}:{minutes:02d}:{remain:05.2f}"
    if minutes:
        return f"{minutes:d}:{remain:05.2f}"
    return f"{remain:.2f} 秒"


def esc(value: Any) -> str:
    return html.escape(str(value), quote=True)


def short_sha(value: str) -> str:
    return f"{value[:12]}…{value[-8:]}"


def pct(part: int, whole: int) -> float:
    return 0.0 if whole == 0 else 100.0 * part / whole


def build_report_model(
    bundle: Mapping[str, Any],
    historical: Mapping[str, Any],
    public_mounts: List[Dict[str, Any]],
) -> Dict[str, Any]:
    run = bundle["run_receipt"]
    validation = bundle["validation_receipt"]
    producer = bundle["producer_receipt"]
    summary = bundle["summary"]
    counts = summary["counts"]
    performance = run["performance"]
    artifacts = sorted(
        (
            {
                "artifact_id": artifact_id,
                "relative_path": row["relative_path"],
                "format": row["format"],
                "logical_rows": row["logical_rows"],
                "size_bytes": row["size_bytes"],
                "physical_sha256": row["physical_sha256"],
                "semantic_sha256": row["semantic_sha256"],
            }
            for artifact_id, row in bundle["artifact_records"].items()
        ),
        key=lambda row: row["artifact_id"],
    )

    comparison = [
        {
            "class": "EXACT",
            "topic": "分析母體",
            "old": f"{fmt_int(historical['m1']['site_keys'])} autosomal PASS biallelic sSNVs",
            "new": f"{fmt_int(counts['site_keys'])} site keys；missing/extra/duplicate = 0/0/0",
            "verdict": "同一 HCC1395 chr1–22 site population；鍵集合曾以獨立 stream digest 核對。",
        },
        {
            "class": "SEMANTIC",
            "topic": "M1 / M2 / topology",
            "old": "舊 Python 分類僅作 frozen context",
            "new": "C++ artifact semantic SHA-256 + independent validator conservation replay",
            "verdict": "比較狀態與守恆語意；不要求不同 schema 的 physical bytes 相同。",
        },
        {
            "class": "CONTEXT",
            "topic": "區域結構",
            "old": (
                f"{fmt_int(historical['regional']['regions'])} regions / "
                f"{fmt_int(historical['regional']['units'])} units；"
                f"CN={historical['regional']['cn_source']}"
            ),
            "new": (
                f"{fmt_int(counts['topology_regions'])} regions / "
                f"{fmt_int(counts['topology_primary_hp_units'])} primary HP units"
            ),
            "verdict": "舊方法、CN/LOH gating 與完成語意不同，只能解釋規模與方法影響。",
        },
        {
            "class": "NOT_COMPARABLE",
            "topic": "正式 sSNV 共現結果",
            "old": "舊正式流程在發布前失敗；無 formal comparable authority",
            "new": f"{fmt_int(bundle['artifact_records']['cooccurrence_pairs']['logical_rows'])} pair rows，受 frozen validator 約束",
            "verdict": "舊流程沒有 formal comparable authority；不可宣稱數值 parity 或 speedup。",
        },
    ]

    bottlenecks = [
        {
            "rank": 1,
            "name": "端到端 wall time",
            "value": fmt_duration(performance["wall_seconds"]),
            "detail": "C++ producer receipt 的整體科學執行時間；不是把歷史時間相加。",
        },
        {
            "rank": 2,
            "name": "I/O",
            "value": (
                f"read {fmt_bytes(performance['io_read_bytes'])} / "
                f"write {fmt_bytes(performance['io_write_bytes'])}"
            ),
            "detail": "raw BAM 在 NFS；HP/PS sidecar 與輸出在 big7。full SHA 與 BGZF write 可能主導 wall time。",
        },
        {
            "rank": 3,
            "name": "排程等待",
            "value": (
                f"queue {performance['queue_wait_seconds']:.2f}s / "
                f"reorder {performance['reorder_wait_seconds']:.2f}s"
            ),
            "detail": "可區分 compute 飽和與 deterministic reorder/backpressure。",
        },
        {
            "rank": 4,
            "name": "長尾 block",
            "value": f"p99 {performance['task_latency_seconds']['p99']:.2f}s",
            "detail": (
                f"p50 {performance['task_latency_seconds']['p50']:.2f}s；"
                f"max {performance['task_latency_seconds']['max']:.2f}s。"
            ),
        },
    ]

    return {
        "schema_name": "longlineage.hcc1395_validated_report",
        "schema_version": "1.0.0",
        "report_scope": "HCC1395_ONLY_NON_PRODUCTION_DATASET_GATE",
        "production_claim_allowed": False,
        "run_id": run["run_id"],
        "validated_at": validation["validated_at"],
        "terminal_state": run["state"],
        "validation_profile": run["validation_profile"],
        "validator_independent": validation["validator_independent"],
        "truth_fields_seen": run["truth_fields_seen"],
        "input_contract": {
            "statement": (
                "本輪輸入是 raw BAM + external latest HP/PS sidecar；"
                "不是一個持久化、已 tag 的 production BAM。"
            ),
            "raw_bam_plus_sidecar_not_tagged_bam": True,
            "mounts": public_mounts,
        },
        "counts": counts,
        "scope": summary["scope"],
        "producer_run_local_phase_status": {
            "scope": "RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER",
            "status": summary["phase_status"],
        },
        "performance": performance,
        "timing_stages": [
            {
                "stage": "Input identity / preflight",
                "time": "NOT_SEPARATELY_RECORDED",
                "status": "BOUND_NOT_TIMED",
                "meaning": (
                    "八角色 SHA/index probes 由 dataset-gate 執行；final run receipt "
                    "只保留 snapshot/digest，不虛構分段時間。"
                ),
            },
            {
                "stage": "C++ scientific producer",
                "time": fmt_duration(performance["wall_seconds"]),
                "status": "MEASURED",
                "meaning": "M1、sSNV co-occurrence、topology 與 native artifact write。",
            },
            {
                "stage": "Deterministic scheduling waits",
                "time": (
                    f"queue {performance['queue_wait_seconds']:.2f}s / "
                    f"reorder {performance['reorder_wait_seconds']:.2f}s"
                ),
                "status": "MEASURED_SUBCOMPONENT",
                "meaning": "包含在 producer wall；不可再次相加。",
            },
            {
                "stage": "Independent validator + freeze",
                "time": "NOT_SEPARATELY_RECORDED",
                "status": "PASS_UNTIMED",
                "meaning": (
                    f"{len(validation['checks'])} checks 全 PASS；receipt 只記 validated_at，"
                    "不由兩個 timestamp 猜 elapsed。"
                ),
            },
            {
                "stage": "HTML digest replay + render",
                "time": "CALLER_MEASURES_END_TO_END",
                "status": "PRESENTATION_ONLY",
                "meaning": "不屬科學 wall time；由外層 /usr/bin/time 與 QA receipt 紀錄。",
            },
        ],
        "bottlenecks": bottlenecks,
        "artifacts": artifacts,
        "validation_checks": [
            projected_validation_check(row, "w24 validation")
            for row in validation["checks"]
        ],
        "semantic_rows": bundle["semantic_rows"],
        "current_authority_sha256": bundle["observed_sha256"],
        "historical": historical,
        "comparison": comparison,
        "limitations": [
            "這是 1/7 dataset gate；production_claim_allowed=false，不可代表七資料集 production。",
            "舊正式流程在發布前失敗，沒有 formal comparable authority；pair/site 數值只能驗新流程內部守恆與 oracle，不能做舊新 parity。",
            "舊 regional tree 使用不同 Python-led 方法及 SEQC2 CN context；區域與 unit 數不可直接做改善率。",
            "舊 n_strict_confirm_candidates=12,838 是 legacy alias；79,687 個 strict_confirm_status 全為 NOT_RUN。",
            "Python 不讀 scientific artifact data rows（M1/co-occurrence/topology 等）；"
            "允許讀 artifact catalog/semantic digest metadata rows 與 summary/receipts，"
            "並只用於驗證、呈現，不重算 M1/M2/topology 科學推論。",
        ],
    }


def audit_historical_for_presentation(chain: Mapping[str, Any]) -> Dict[str, Any]:
    historical = chain["audit_receipt"]["historical"]
    m1 = historical["m1"]
    old_counts = m1["old_counts"]
    regional = historical["regional_topology"]
    old_determinacy = regional.get(
        "old_region_determinacy",
        {
            "all_determined": 0,
            "has_ambiguous": 0,
            "has_capped": 0,
            "has_recurrence": 0,
            "no_primary_lineage": 0,
        },
    )
    for key in (
        "all_determined",
        "has_ambiguous",
        "has_capped",
        "has_recurrence",
        "no_primary_lineage",
    ):
        require(isinstance(old_determinacy.get(key), int),
                f"C++ audit old regional determinacy missing: {key}")
    return {
        "verified_files": {
            "cxx_determinism_historical_receipt": public_binding(
                chain["execution"]["audit_receipt"]
            ),
            "dataset_gate_input_authority": public_binding(
                chain["execution"]["authorities"]["dataset_gate_input"]
            ),
            "production_input_authority": public_binding(
                chain["execution"]["authorities"]["production_input"]
            ),
            "executable_build_receipt": public_binding(
                chain["execution"]["build_receipt"]
            ),
        },
        "extraction": {
            "wall_seconds": 0.0,
            "site_keys": historical["site_keys"]["old_count"],
            "read_observations": historical.get("read_evidence", {}).get(
                "old_read_observations", 0
            ),
            "valid_read_pairs": historical.get("read_evidence", {}).get(
                "old_bernoulli_read_pairs", 0
            ),
            "invalid_read_pairs": historical.get("read_evidence", {}).get(
                "old_invalid_read_pairs", 0
            ),
            "meaning": "歷史 read-grain 數值只由 C++ audit receipt 呈現，不作 ratio。",
        },
        "m1": {
            "site_keys": historical["site_keys"]["old_count"],
            "evaluable": old_counts.get("evaluable", 0),
            "insufficient_alt_reads": old_counts.get("insufficient_alt_reads", 0),
            "incomplete_distance": old_counts.get(
                "incomplete_distance_below_minimum", 0
            ),
            "stable_assignments": old_counts.get("stable", 0),
            "strict_confirm_not_run": historical["site_keys"]["old_count"],
            "verdict": "COMPARABLE_DIFFERENT",
            "stable_transition": m1["stable_transition"],
            "stable_jaccard_percent": m1.get("stable_jaccard_percent"),
        },
        "m2": {
            "eligible": historical["m2"].get("old_counts", {}).get("eligible", 0),
            "evaluable_ineligible": historical["m2"].get("old_counts", {}).get(
                "evaluable_ineligible", 0
            ),
            "axis_indeterminate": historical["m2"].get("old_counts", {}).get(
                "axis_indeterminate", 0
            ),
            "group_count_gt10": historical["m2"].get("old_counts", {}).get(
                "group_count_gt10", 0
            ),
            "formal_scientific_result": False,
            "failed_elapsed": "historical failed attempt",
            "failure": "歷史 full co-occurrence 在正式結果發布前失敗。",
            "verdict": "NOT_COMPARABLE_METHOD_CHANGED",
        },
        "regional": {
            "context_counts_available": all(
                key in regional
                for key in (
                    "old_regions",
                    "old_units",
                    "old_primary_lineage_units",
                    "old_region_determinacy",
                )
            ),
            "regions": regional.get("old_regions", 0),
            "units": regional.get("old_units", 0),
            "primary_lineage_units": regional.get("old_primary_lineage_units", 0),
            "region_determinacy": old_determinacy,
            "cn_source": regional.get("old_cn_source", "historical-method-specific"),
            "method_scope": "NOT_COMPARABLE_METHOD_AND_GATE_CHANGED",
        },
    }


def build_dual_report_model(chain: Mapping[str, Any]) -> Dict[str, Any]:
    bundles = chain["bundles"]
    historical = audit_historical_for_presentation(chain)
    w24 = bundles["w24"]
    w40 = bundles["w40"]
    execution = chain["execution"]
    audit = chain["audit_receipt"]
    authority_files = {
        row["role"]: row for row in chain["authorities"]["dataset_gate"]["files"]
    }
    producer_mounts = {
        row["role"]: row
        for row in w24["producer_receipt"]["input_mount_identity"]
    }
    require(
        set(authority_files) == set(producer_mounts) == EXPECTED_INPUT_ROLES,
        "public input mount projection membership mismatch",
    )
    public_mounts: List[Dict[str, Any]] = []
    for role in sorted(EXPECTED_INPUT_ROLES):
        authority_row = authority_files[role]
        mount_row = producer_mounts[role]
        public_mounts.append(
            {
                "role": role,
                "path_token": safe_identifier(
                    authority_row.get("path_token"),
                    f"input authority path token {role}",
                ),
                "filesystem_type": safe_identifier(
                    mount_row.get("filesystem_type"),
                    f"input filesystem type {role}",
                ),
                "readonly": mount_row.get("readonly"),
            }
        )
        require(
            isinstance(public_mounts[-1]["readonly"], bool),
            f"input mount readonly flag is invalid: {role}",
        )
    model = build_report_model(bundles["w24"], historical, public_mounts)
    counts = w24["summary"]["counts"]
    model.update(
        {
            "schema_name": "longlineage.hcc1395_validated_report",
            "schema_version": "2.0.0",
            "report_scope": EXPECTED_REPORT_SCOPE,
            "run_id": f"{w24['run_receipt']['run_id']} + {w40['run_receipt']['run_id']}",
            "run_ids": {
                "w24": w24["run_receipt"]["run_id"],
                "w40": w40["run_receipt"]["run_id"],
            },
            "validated_at": {
                "w24": w24["validation_receipt"]["validated_at"],
                "w40": w40["validation_receipt"]["validated_at"],
            },
            "terminal_state": {
                "w24": w24["run_receipt"]["state"],
                "w40": w40["run_receipt"]["state"],
            },
            "truth_fields_seen": 0,
            "determinism": {
                "status": "PASS",
                "artifact_count": len(chain["comparison"]["artifacts"]),
                "artifacts": chain["comparison"]["artifacts"],
                "summary_projection_sha256": chain["comparison"][
                    "summary_projection_sha256"
                ],
                "manifest_difference_pointers": chain["comparison"][
                    "manifest_differences"
                ],
                "audit_receipt_sha256": execution["audit_receipt"]["sha256"],
                "audit_generator": audit["generator"],
            },
            "evidence": {
                "execution_evidence": {
                    "external_binding": True,
                    "size_bytes": Path(execution["path"]).stat().st_size,
                    "sha256": execution["sha256"],
                },
                "science_build_receipt": public_binding(
                    execution["build_receipt"]
                ),
                "audit_receipt": public_binding(execution["audit_receipt"]),
                "audit_executable": public_binding(
                    execution["audit_executable"]
                ),
                "report_builder_source": public_binding(
                    execution["report_builder_source"]
                ),
                "audit_ctest_log": public_binding(
                    execution["audit_ctest_log"]
                ),
                "authorities": {
                    key: public_binding(value)
                    for key, value in execution["authorities"].items()
                },
                "science_git_commit": execution["science_git_commit"],
                "audit_report_git_commit": execution["audit_report_git_commit"],
            },
            "science_parameters": w24["science_parameters"],
            "cooccurrence_replay": {
                "status": audit["determinism"]["cooccurrence_replay"]["status"],
                "authority": audit["determinism"]["cooccurrence_replay"][
                    "authority"
                ],
                "interpretation": audit["determinism"]["cooccurrence_replay"][
                    "interpretation"
                ],
                "counters": dict(
                    audit["determinism"]["cooccurrence_replay"]["w24"]
                ),
                "worker_invariant": (
                    audit["determinism"]["cooccurrence_replay"]["w24"]
                    == audit["determinism"]["cooccurrence_replay"]["w40"]
                ),
            },
        }
    )
    artifacts = []
    for artifact_id in EXPECTED_ARTIFACT_IDS:
        left = w24["artifact_records"][artifact_id]
        right = w40["artifact_records"][artifact_id]
        artifacts.append(
            {
                "artifact_id": artifact_id,
                "relative_path": left["relative_path"],
                "format": left["format"],
                "schema_name": left["schema_name"],
                "schema_version": left["schema_version"],
                "logical_rows": left["logical_rows"],
                "size_bytes": left["size_bytes"],
                "physical_sha256": left["physical_sha256"],
                "w40_size_bytes": right["size_bytes"],
                "w40_physical_sha256": right["physical_sha256"],
                "semantic_sha256": left["semantic_sha256"],
                "semantic_equal": True,
            }
        )
    model["artifacts"] = artifacts

    validation_checks = []
    for label in ("w24", "w40"):
        for check in bundles[label]["validation_receipt"]["checks"]:
            copied = projected_validation_check(check, f"{label} validation")
            copied["check_id"] = f"{label.upper()}__{copied['check_id']}"
            validation_checks.append(copied)
    model["validation_checks"] = validation_checks
    model["current_authority_sha256"] = {
        "w24_validation_receipt": w24["observed_sha256"]["validation_receipt"],
        "w40_validation_receipt": w40["observed_sha256"]["validation_receipt"],
        "validation_receipt": w24["observed_sha256"]["validation_receipt"],
        "execution_evidence": execution["sha256"],
        "audit_receipt": execution["audit_receipt"]["sha256"],
    }

    model["comparison"] = [
        {
            "class": "EXACT",
            "topic": "HCC1395 site-key set",
            "old": (
                f"{fmt_int(audit['historical']['site_keys']['old_count'])} keys；"
                f"digest {short_sha(audit['historical']['site_keys'].get('old_ordered_sha256', SHA256_ZERO))}"
            ),
            "new": (
                f"{fmt_int(counts['site_keys'])} keys；missing/extra/duplicate = 0/0/0"
            ),
            "verdict": "EXACT",
        },
        {
            "class": "SEMANTIC",
            "topic": "M1 per-key status / stable membership",
            "old": f"stable {fmt_int(historical['m1']['stable_assignments'])}",
            "new": f"stable {fmt_int(counts['m1_stable_assignments'])}",
            "verdict": (
                "COMPARABLE_DIFFERENT：不能以 aggregate 淨差取代 per-key churn；"
                f"transition={json.dumps(historical['m1']['stable_transition'], sort_keys=True)}"
            ),
        },
        {
            "class": "NOT_COMPARABLE",
            "topic": "正式 sSNV co-occurrence",
            "old": "沒有 durable historical formal result",
            "new": (
                f"{fmt_int(model['cooccurrence_replay']['counters']['pair_rows'])} "
                "validated pair rows；"
                f"{fmt_int(model['cooccurrence_replay']['counters']['formal_pair_by_confirmed'])} "
                "formal BY-confirmed"
            ),
            "verdict": "NOT_COMPARABLE_NO_FORMAL_OLD_RESULT",
        },
        {
            "class": "NOT_COMPARABLE",
            "topic": "regional tree / LongLineage topology",
            "old": (
                f"{fmt_int(historical['regional']['regions'])} regions / "
                f"{fmt_int(historical['regional']['units'])} units"
            ),
            "new": (
                f"{fmt_int(counts['topology_regions'])} regions / "
                f"{fmt_int(counts['topology_primary_hp_units'])} primary HP units"
            ),
            "verdict": "NOT_COMPARABLE_METHOD_AND_GATE_CHANGED",
        },
        {
            "class": "NOT_COMPARABLE",
            "topic": "historical runtime / v5 runtime",
            "old": "stage、scope、cache condition 不同",
            "new": "w24/w40 producer 與 validator 各自量測",
            "verdict": "NOT_COMPARABLE_PROGRAM_SCOPE_THREAD_OUTPUT_CHANGED",
        },
    ]

    timing_stages = []
    for label in ("w24", "w40"):
        science = bundles[label]["run_receipt"]["performance"]
        producer_outer = execution["runs"][label]["producer_execution"]
        validator_outer = execution["runs"][label]["validator_execution"]
        timing_stages.extend(
            [
                {
                    "stage": f"{label} C++ producer science",
                    "time": fmt_duration(science["wall_seconds"]),
                    "status": "MEASURED_RECEIPT",
                    "meaning": "run receipt 的 science total；不含外層 input SHA preflight 差額。",
                },
                {
                    "stage": f"{label} producer outer /usr/bin/time",
                    "time": fmt_duration(producer_outer["wall_seconds"]),
                    "status": "MEASURED_LOG",
                    "meaning": (
                        f"exit=0；max RSS={fmt_bytes(producer_outer['max_rss_kib'] * 1024)}。"
                    ),
                },
                {
                    "stage": f"{label} independent validator + freeze",
                    "time": fmt_duration(validator_outer["wall_seconds"]),
                    "status": "MEASURED_LOG",
                    "meaning": (
                        f"exit=0；max RSS={fmt_bytes(validator_outer['max_rss_kib'] * 1024)}。"
                    ),
                },
            ]
        )
    timing_stages.append(
        {
            "stage": "HTML receipt replay + render",
            "time": "RECORDED_IN_OUTPUT_JSON",
            "status": "PRESENTATION_ONLY",
            "meaning": "Python 只 hash/驗證/呈現；不讀 BAM/VCF/sidecar，也不重算 science。",
        }
    )
    model["timing_stages"] = timing_stages
    model["bottlenecks"] = [
        {
            "rank": 1,
            "name": "Producer outer wall",
            "value": (
                f"w24 {fmt_duration(execution['runs']['w24']['producer_execution']['wall_seconds'])} / "
                f"w40 {fmt_duration(execution['runs']['w40']['producer_execution']['wall_seconds'])}"
            ),
            "detail": "含輸入 identity/full SHA 與 C++ science；只做同輪實測比較。",
        },
        {
            "rank": 2,
            "name": "Independent validator wall",
            "value": (
                f"w24 {fmt_duration(execution['runs']['w24']['validator_execution']['wall_seconds'])} / "
                f"w40 {fmt_duration(execution['runs']['w40']['validator_execution']['wall_seconds'])}"
            ),
            "detail": "獨立 physical/schema/conservation replay 與 atomic freeze。",
        },
        {
            "rank": 3,
            "name": "Producer I/O",
            "value": (
                f"w24 read {fmt_bytes(w24['run_receipt']['performance']['io_read_bytes'])} / "
                f"w40 read {fmt_bytes(w40['run_receipt']['performance']['io_read_bytes'])}"
            ),
            "detail": "raw BAM 位於 NFS4；這是 receipt counter，不單獨推論因果占比。",
        },
        {
            "rank": 4,
            "name": "Peak RSS",
            "value": (
                f"w24 {fmt_bytes(execution['runs']['w24']['producer_execution']['max_rss_kib'] * 1024)} / "
                f"w40 {fmt_bytes(execution['runs']['w40']['producer_execution']['max_rss_kib'] * 1024)}"
            ),
            "detail": "GNU time process peak；不與 validator peak 相加。",
        },
    ]
    model["limitations"] = [
        "HCC1395 是 1/7 dataset gate；production_claim_allowed=false，不能外推七資料集 production。",
        "w24/w40 只證明同一 HCC1395 frozen input 下八項 science artifact 的 semantic determinism。",
        "M1 舊新是 COMPARABLE_DIFFERENT，屬 legacy Python decision-parity release blocker：stable aggregate 僅淨差不代表 site membership 相同，必須保留 transition/churn。",
        "舊 co-occurrence 正式流程在發布前失敗，沒有 formal durable/comparable authority；"
        "舊 read×read distance pair 與新 sSNV pair 定義不同，不可做 parity、ratio 或 speedup。",
        "舊 regional tree 與新 topology 的方法、CN/LOH gating、membership 及 completion semantics 不同。",
        "queue_wait_seconds 是多 worker aggregate thread-seconds，已包含在 producer 執行內；不得當 wall time 相加或排名。",
        "共現 counters 是 C++ audit 對已驗證序列化 artifact 的 census；它不重算 p-value 或 q-value，獨立科學重算仍由 validator 負責。",
        "Python 不讀 scientific artifact data rows（M1/co-occurrence/topology 等）或 BAM/VCF/sidecar；"
        "允許讀 artifact catalog/semantic digest metadata rows 與 summary/receipts，"
        "並只用於 hash、驗證與呈現，不重算 M1/M2/co-occurrence/topology。",
        "raw BAM 不是持久化 tagged BAM；HP/PS 來自 frozen sidecar 的 EXACT_PROJECTION_NO_FALLBACK。",
        "Frozen manifest/receipt 沒有獨立的 run-level M1 representation 欄位；本報告不把 raw-point 或 historical round6/round4 的選擇冒充為 receipt-bound fact。這是 legacy parity 根因分析前必須修補的 provenance gap。",
    ]
    return model


def badge(value: str) -> str:
    css = value.lower().replace("_", "-")
    return f'<span class="badge {esc(css)}">{esc(value)}</span>'


def bar_row(label: str, value: int, total: int, tone: str) -> str:
    actual_percent = min(100.0, pct(value, total)) if value else 0.0
    visual_width = max(0.2, actual_percent) if value else 0.0
    return (
        '<div class="bar-row">'
        f'<div class="bar-label"><span>{esc(label)}</span><strong>{fmt_int(value)}</strong></div>'
        '<div class="track" aria-hidden="true">'
        f'<span class="{esc(tone)}" style="width:{visual_width:.4f}%"></span></div>'
        f'<small>{actual_percent:.2f}% of {fmt_int(total)}</small>'
        "</div>"
    )


def render_html(model: Mapping[str, Any]) -> str:
    counts = model["counts"]
    performance = model["performance"]
    historical = model["historical"]
    cooccurrence = model["cooccurrence_replay"]["counters"]
    science = model["science_parameters"]
    read_filter = science["read_filter"]
    methylation = science["methylation"]
    m1_contract = science["m1"]
    m2_contract = science["m2_eligibility"]
    cooccurrence_contract = science["cooccurrence"]
    topology_contract = science["topology"]

    def setting_value(value: Any) -> str:
        if isinstance(value, (dict, list)):
            return json.dumps(value, ensure_ascii=False, separators=(",", ":"))
        if isinstance(value, bool):
            return "true" if value else "false"
        return str(value)

    method_rows_data = [
        (
            "Read filter",
            "excluded_flag_masks",
            read_filter["excluded_flag_masks"],
            "排除 unmapped、secondary、duplicate、supplementary；任何保留 read 必須通過同一 frozen mask。",
        ),
        (
            "Read filter",
            "MAPQ / query length",
            {
                "minimum_mapq": read_filter["minimum_mapq"],
                "minimum_query_length": read_filter["minimum_query_length"],
            },
            "先做 mapping 品質與長讀長基本門檻，再進入 MM/ML 與 site projection。",
        ),
        (
            "Read filter",
            "MM / ML / duplicate",
            {
                "require_mm": read_filter["require_mm"],
                "require_ml": read_filter["require_ml"],
                "duplicate_policy": read_filter["non_equivalent_duplicate_policy"],
            },
            "缺甲基化 tag 或非等價 duplicate 不補值，直接 fail closed。",
        ),
        (
            "Methylation",
            "MM/ML decode",
            {
                "accepted_mm_group": methylation["accepted_mm_group"],
                "orientation": methylation["orientation"],
                "ml_interval": methylation["ml_interval"],
                "mn_mismatch_policy": methylation["mn_mismatch_policy"],
                "parse_once_per_read": methylation["parse_once_per_read"],
            },
            "C++ 每 read 只解析一次；MN 不一致不得猜測座標或機率。",
        ),
        (
            "M1",
            "minimum evidence",
            {
                "joined_alt_reads": m1_contract["minimum_joined_alt_reads"],
                "after_distance_peel": m1_contract[
                    "minimum_after_complete_distance_peel"
                ],
                "group_size": m1_contract["minimum_group_size"],
                "groups_max": m1_contract["maximum_groups_considered"],
                "common_cpg": m1_contract["minimum_common_cpg"],
            },
            "不足時保留 INSUFFICIENT/INCOMPLETE 狀態；不強行分群。",
        ),
        (
            "M1",
            "null / stability",
            {
                "null_mode": m1_contract["null_mode"],
                "replicates": m1_contract["null_replicates_per_split"],
                "valid_min": m1_contract["minimum_valid_null_replicates"],
                "between_within_ratio": m1_contract["minimum_between_within_ratio"],
                "modal_fraction": m1_contract["modal_fraction_minimum"],
                "modal_ari": m1_contract["modal_assignment_ari_minimum"],
            },
            "observed 必須嚴格大於 frozen percentile，且 assignment modal fraction/ARI 同時過門檻。",
        ),
        (
            "M1",
            "deterministic RNG / axes",
            {
                "rng": m1_contract["rng"],
                "seed": m1_contract["site_seed_derivation"],
                "axis_order": m1_contract["axis_order"],
                "axis_evaluation": m1_contract["axis_evaluation"],
            },
            "seed 綁 sample|chrom|pos|offset；axis 僅在 stable gate 後評估，避免母體漂移。",
        ),
        (
            "M2 eligibility",
            "power / precedence",
            {
                "permutations": m2_contract["permutations"],
                "minimum_group_size": m2_contract["minimum_group_size"],
                "maximum_groups": m2_contract["maximum_groups"],
                "minimum_power": m2_contract["minimum_power"],
                "reason_precedence": m2_contract["reason_precedence"],
            },
            "每個 stable site 依 precedence 恰落一個互斥 bin；四 bin 必須守恆。",
        ),
        (
            "Co-occurrence A",
            "exact test / FDR",
            {
                "window_bp": cooccurrence_contract["partner_window_bp"],
                "test": cooccurrence_contract["endpoint_a"]["test"],
                "state_space_ceiling": cooccurrence_contract["endpoint_a"][
                    "state_space_ceiling"
                ],
                "fallback": cooccurrence_contract["endpoint_a"]["fallback"],
                "fdr_primary": cooccurrence_contract["endpoint_a"][
                    "global_fdr_primary"
                ],
                "q_maximum": cooccurrence_contract["endpoint_a"]["q_maximum"],
                "cramers_v_minimum": cooccurrence_contract["endpoint_a"][
                    "cramers_v_minimum"
                ],
                "delta_alt_fraction_minimum": cooccurrence_contract["endpoint_a"][
                    "delta_alt_fraction_minimum"
                ],
            },
            "K×2 fixed-margin exact；超過 state ceiling 沒有近似 fallback，維持 NOT_IDENTIFIABLE。",
        ),
        (
            "Co-occurrence B",
            "callability",
            {
                "allele_calls": cooccurrence_contract["endpoint_b"]["allele_calls"],
                "o_x_collapse": cooccurrence_contract["endpoint_b"][
                    "o_x_cell_collapse_allowed"
                ],
                "minimum_called_depth": cooccurrence_contract["endpoint_b"][
                    "minimum_called_depth"
                ],
                "multiplicity": cooccurrence_contract["endpoint_b"]["multiplicity"],
                "claim": cooccurrence_contract["endpoint_b"]["claim"],
            },
            "R/A/O/X 分開；O 與 X 不得併為 REF，endpoint B 只提供 compatibility 證據。",
        ),
        (
            "Joint signature",
            "marker set",
            cooccurrence_contract["joint_signature"],
            "最多 3 markers、至少 20 bp，且要求同一 complete read set；spacing 不代表統計獨立。",
        ),
        (
            "Topology",
            "model / solver / abstention",
            {
                "primary_model": topology_contract["primary_model"],
                "strict_infinite_sites": topology_contract["strict_infinite_sites"],
                "loss_supported_dollo": topology_contract["loss_supported_dollo"],
                "solver_order": topology_contract["solver_order"],
                "incomplete_family_winner_allowed": topology_contract[
                    "incomplete_family_winner_allowed"
                ],
            },
            "先 exact reductions/DP/B&B，再 parent mapping；family 不完整、cap/deadline 均不得發布 winner。",
        ),
    ]
    method_rows = "".join(
        "<tr>"
        f"<td><strong>{esc(stage)}</strong></td>"
        f"<td><code>{esc(setting)}</code></td>"
        f"<td><code>{esc(setting_value(value))}</code></td>"
        f"<td>{esc(reason)}</td>"
        "</tr>"
        for stage, setting, value, reason in method_rows_data
    )
    mount_rows = "".join(
        "<tr>"
        f"<td><strong>{esc(row['role'])}</strong></td>"
        f"<td><code>{esc(row['path_token'])}</code></td>"
        f"<td>{esc(row['filesystem_type'])}</td>"
        f"<td>{'RO' if row['readonly'] else 'RW'}</td>"
        "</tr>"
        for row in model["input_contract"]["mounts"]
    )
    comparison_rows = "".join(
        "<tr>"
        f"<td>{badge(row['class'])}</td>"
        f"<td><strong>{esc(row['topic'])}</strong></td>"
        f"<td>{esc(row['old'])}</td>"
        f"<td>{esc(row['new'])}</td>"
        f"<td>{esc(row['verdict'])}</td>"
        "</tr>"
        for row in model["comparison"]
    )
    determinism_rows = "".join(
        "<tr>"
        f"<td><strong>{esc(row['artifact_id'])}</strong></td>"
        f"<td><code>{esc(row['schema_name'])}@{esc(row['schema_version'])}</code></td>"
        f"<td class=\"num\">{fmt_int(row['logical_rows'])}</td>"
        f"<td><code>{esc(short_sha(row['semantic_sha256']))}</code></td>"
        f"<td>{badge('PASS')}</td>"
        "</tr>"
        for row in model["determinism"]["artifacts"]
    )
    artifact_rows = "".join(
        "<tr>"
        f"<td><strong>{esc(row['artifact_id'])}</strong><small>{esc(row['format'])}</small></td>"
        f"<td><code>{esc(row['relative_path'])}</code></td>"
        f"<td><code>{esc(row['schema_name'])}@{esc(row['schema_version'])}</code></td>"
        f"<td class=\"num\">{fmt_int(row['logical_rows'])}</td>"
        f"<td><small>w24 {fmt_bytes(row['size_bytes'])}</small>"
        f"<code>{esc(short_sha(row['physical_sha256']))}</code>"
        f"<small>w40 {fmt_bytes(row['w40_size_bytes'])}</small>"
        f"<code>{esc(short_sha(row['w40_physical_sha256']))}</code></td>"
        f"<td><code>{esc(short_sha(row['semantic_sha256']))}</code></td>"
        "</tr>"
        for row in model["artifacts"]
    )
    bottlenecks = "".join(
        '<article class="bottleneck">'
        f'<span class="rank">{row["rank"]:02d}</span><div>'
        f"<h3>{esc(row['name'])}</h3><strong>{esc(row['value'])}</strong>"
        f"<p>{esc(row['detail'])}</p></div></article>"
        for row in model["bottlenecks"]
    )
    timing_rows = "".join(
        "<tr>"
        f"<td><strong>{esc(row['stage'])}</strong></td>"
        f"<td>{badge(row['status'])}</td>"
        f"<td><code>{esc(row['time'])}</code></td>"
        f"<td>{esc(row['meaning'])}</td>"
        "</tr>"
        for row in model["timing_stages"]
    )
    validation_details = "".join(
        "<details>"
        f"<summary>{badge(check['status'])} {esc(check['check_id'])}</summary>"
        '<div class="detail-body">'
        f"<p><strong>Payload digest</strong> <code>{esc(check['payload_sha256'])}</code></p>"
        f"<p><strong>Evidence</strong> <code>{esc(check['evidence_sha256'])}</code></p>"
        "</div></details>"
        for check in model["validation_checks"]
    )
    historical_files = "".join(
        "<li>"
        f"<strong>{esc(authority_id)}</strong>"
        "<code>EXTERNAL_BINDING</code>"
        f"<span>{fmt_bytes(row['size_bytes'])} · {esc(short_sha(row['sha256']))}</span>"
        "</li>"
        for authority_id, row in historical["verified_files"].items()
    )
    limit_rows = "".join(f"<li>{esc(item)}</li>" for item in model["limitations"])
    m1_bars = "".join(
        [
            bar_row("evaluable", counts["m1_evaluable"], counts["site_keys"], "blue"),
            bar_row(
                "insufficient ALT reads",
                counts["m1_insufficient_alt_reads"],
                counts["site_keys"],
                "amber",
            ),
            bar_row(
                "incomplete distance",
                counts["m1_incomplete_distance"],
                counts["site_keys"],
                "red",
            ),
        ]
    )
    topology_total = max(1, counts["topology_regions"])
    topology_bars = "".join(
        [
            bar_row(
                "fully complete regions",
                counts["topology_fully_complete_regions"],
                topology_total,
                "green",
            ),
            bar_row(
                "incomplete regions",
                counts["topology_incomplete_regions"],
                topology_total,
                "amber",
            ),
        ]
    )
    history_determinacy = historical["regional"]["region_determinacy"]
    if historical["regional"]["context_counts_available"]:
        old_region_bars = "".join(
            bar_row(label, value, historical["regional"]["regions"], tone)
            for label, value, tone in [
                ("old all_determined", history_determinacy["all_determined"], "green"),
                ("old has_ambiguous", history_determinacy["has_ambiguous"], "blue"),
                ("old has_capped", history_determinacy["has_capped"], "amber"),
                ("old has_recurrence", history_determinacy["has_recurrence"], "red"),
                (
                    "old no_primary_lineage",
                    history_determinacy["no_primary_lineage"],
                    "gray",
                ),
            ]
        )
        regional_context_text = (
            f"舊資料為 {fmt_int(historical['regional']['regions'])} regions、"
            f"{fmt_int(historical['regional']['units'])} units、"
            f"{fmt_int(historical['regional']['primary_lineage_units'])} "
            f"primary-lineage units，CN context={historical['regional']['cn_source']}。"
        )
    else:
        old_region_bars = (
            '<div class="callout"><strong>數值比較停用：</strong>'
            "C++ audit receipt 只授權 NOT_COMPARABLE 邊界，未攜帶可顯示的舊 regional census；"
            "HTML 不以硬編數字補值。</div>"
        )
        regional_context_text = (
            "舊 regional 方法、CN/LOH gating、membership 與 completion semantics "
            "被 audit receipt 判為 NOT_COMPARABLE；本頁不產生差值或改善率。"
        )
    terminal_display = " / ".join(
        f"{label}={state}" for label, state in model["terminal_state"].items()
    )
    validated_display = " / ".join(
        f"{label}={value}" for label, value in model["validated_at"].items()
    )

    return f"""<!doctype html>
<html lang="zh-Hant" data-partial="true" data-report-id="{esc(model['run_id'])}">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light">
  <title>HCC1395 完整科學運算與資料確認 · LongLineage</title>
  <style>
    :root {{
      --paper:#f1eee5; --sheet:#fbfaf5; --ink:#162129; --muted:#5f6a6f;
      --steel:#19384a; --steel2:#28566b; --line:#c8c4b8; --orange:#d85b22;
      --acid:#c8df52; --green:#287a5a; --blue:#2f6f92; --amber:#bd7a19;
      --red:#a83a34; --gray:#7a8282; --shadow:0 16px 38px rgba(18,33,41,.11);
      --serif:"Iowan Old Style","Baskerville","Noto Serif TC","PMingLiU",serif;
      --sans:"Avenir Next","Noto Sans TC","PingFang TC","Microsoft JhengHei",sans-serif;
      --mono:"IBM Plex Mono","SFMono-Regular","Cascadia Mono",monospace;
    }}
    * {{ box-sizing:border-box; }}
    html {{ scroll-behavior:smooth; overflow-x:clip; }}
    body {{
      margin:0; color:var(--ink); background:
        linear-gradient(rgba(25,56,74,.045) 1px,transparent 1px),
        linear-gradient(90deg,rgba(25,56,74,.045) 1px,transparent 1px),
        var(--paper);
      background-size:32px 32px; font-family:var(--sans); line-height:1.62;
    }}
    code,pre {{ font-family:var(--mono); overflow-wrap:anywhere; word-break:break-word; }}
    .skip-link {{ position:absolute; left:.75rem; top:-5rem; z-index:20; padding:.7rem 1rem; color:#fff; background:#000; }}
    .skip-link:focus {{ top:.75rem; }}
    .ribbon {{
      position:relative; z-index:5; padding:.62rem 1rem; text-align:center;
      color:#fff; background-color:#96322d;
      background-image:repeating-linear-gradient(135deg,#a83a34 0 14px,#96322d 14px 28px);
      font-size:.78rem; font-weight:850; letter-spacing:.08em;
    }}
    .layout {{ max-width:1540px; margin:0 auto; display:grid; grid-template-columns:270px minmax(0,1fr); }}
    aside {{
      position:sticky; top:0; height:100vh; padding:2rem 1.35rem;
      color:#eaf1f1; background:var(--steel); border-right:5px solid var(--acid);
    }}
    .brand {{ font-family:var(--serif); font-size:1.45rem; font-weight:800; line-height:1.05; }}
    .brand small {{ display:block; margin-top:.55rem; color:#a9c1cb; font:700 .72rem/1.35 var(--mono); }}
    nav {{ display:grid; gap:.18rem; margin-top:2.2rem; }}
    nav a {{
      color:#c9d8dc; text-decoration:none; padding:.48rem .65rem;
      border-left:3px solid transparent; font-size:.86rem;
    }}
    nav a:hover,nav a:focus-visible {{ color:#fff; border-color:var(--acid); background:#244b5f; }}
    .aside-note {{ position:absolute; left:1.35rem; right:1.35rem; bottom:1.6rem; color:#9eb5be; font-size:.75rem; }}
    main {{ min-width:0; padding:2rem clamp(1rem,4vw,4.5rem) 5rem; }}
    .hero {{
      position:relative; overflow:hidden; padding:clamp(2rem,5vw,5rem);
      color:#fff; background:var(--steel); box-shadow:var(--shadow); border-bottom:8px solid var(--orange);
    }}
    .hero:after {{
      content:""; position:absolute; width:340px; height:340px; right:-100px; top:-120px;
      border:44px solid rgba(200,223,82,.17); border-radius:50%;
    }}
    .eyebrow {{ color:var(--acid); font:800 .76rem/1.2 var(--mono); letter-spacing:.13em; text-transform:uppercase; }}
    h1,h2 {{ font-family:var(--serif); }}
    h1 {{ max-width:1050px; margin:.65rem 0 1rem; font-size:clamp(2.2rem,5.6vw,5.3rem); line-height:.98; letter-spacing:-.048em; }}
    .hero-lead {{ max-width:920px; font-size:clamp(1rem,2vw,1.28rem); color:#dce7e9; }}
    .state-line {{ display:flex; flex-wrap:wrap; gap:.55rem; align-items:center; margin-top:1.6rem; }}
    .terminal {{ color:var(--steel); background:var(--acid); padding:.45rem .7rem; font:850 .9rem/1.2 var(--mono); }}
    .hero-grid {{ margin-top:2.4rem; display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); border:1px solid #557484; }}
    .hero-grid div {{ padding:1rem; background:#21475a; border-right:1px solid #557484; }}
    .hero-grid span {{ display:block; color:#abc2cb; font:700 .68rem/1.3 var(--mono); text-transform:uppercase; }}
    .hero-grid strong {{ display:block; margin-top:.25rem; font-size:1.08rem; overflow-wrap:anywhere; }}
    section {{ scroll-margin-top:1rem; padding:4.4rem 0 0; }}
    .kicker {{ color:var(--orange); font:850 .74rem/1.2 var(--mono); letter-spacing:.12em; text-transform:uppercase; }}
    h2 {{ margin:.32rem 0 .85rem; color:var(--steel); font-size:clamp(1.75rem,3.3vw,3.1rem); line-height:1.08; letter-spacing:-.028em; }}
    h3 {{ margin:.25rem 0 .4rem; line-height:1.2; }}
    .lead {{ max-width:930px; color:var(--muted); font-size:1.04rem; }}
    .cards {{ display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:1rem; margin-top:1.4rem; }}
    .card {{ min-width:0; padding:1.15rem; background:var(--sheet); border:1px solid var(--line); border-top:5px solid var(--steel); box-shadow:0 5px 15px rgba(18,33,41,.055); }}
    .card .label {{ color:var(--muted); font:700 .72rem/1.3 var(--mono); text-transform:uppercase; }}
    .card .value {{ display:block; margin:.38rem 0; color:var(--steel); font:850 clamp(1.35rem,2.5vw,2.3rem)/1 var(--serif); }}
    .card p {{ margin:.4rem 0 0; font-size:.86rem; color:var(--muted); }}
    .callout {{ margin-top:1.2rem; padding:1.2rem 1.3rem; background:#fff7ef; border-left:7px solid var(--orange); }}
    .callout strong {{ color:var(--red); }}
    .flow {{ display:grid; grid-template-columns:repeat(6,minmax(0,1fr)); gap:.7rem; margin-top:1.6rem; }}
    .flow div {{ position:relative; padding:1rem .75rem; min-height:120px; background:var(--steel); color:#fff; }}
    .flow div:not(:last-child):after {{ content:"→"; position:absolute; right:-.58rem; top:41%; z-index:2; color:var(--orange); font-size:1.5rem; }}
    .flow span {{ color:var(--acid); font:800 .7rem/1.3 var(--mono); }}
    .flow strong {{ display:block; margin-top:.35rem; }}
    .flow small {{ display:block; color:#b9cdd4; margin-top:.5rem; }}
    .two-col {{ display:grid; grid-template-columns:1fr 1fr; gap:1.2rem; margin-top:1.4rem; }}
    .panel {{ min-width:0; background:var(--sheet); border:1px solid var(--line); padding:1.2rem; }}
    .bar-row {{ margin:.9rem 0; }}
    .bar-label {{ display:flex; justify-content:space-between; gap:1rem; }}
    .track {{ height:12px; margin:.35rem 0; background:#dedbd1; overflow:hidden; }}
    .track span {{ display:block; height:100%; }}
    .track .green {{ background:var(--green); }} .track .blue {{ background:var(--blue); }}
    .track .amber {{ background:var(--amber); }} .track .red {{ background:var(--red); }}
    .track .gray {{ background:var(--gray); }}
    .bar-row small {{ color:var(--muted); font-family:var(--mono); }}
    .table-wrap {{
      width:100%; max-width:100%; min-width:0; margin-top:1.3rem;
      overflow:auto; border:1px solid var(--line); background:var(--sheet);
    }}
    .table-wrap:focus-visible {{ outline:3px solid var(--orange); outline-offset:3px; }}
    table {{ width:100%; border-collapse:collapse; font-size:.86rem; }}
    th {{ position:sticky; top:0; color:#fff; background:var(--steel); text-align:left; font:750 .72rem/1.25 var(--mono); letter-spacing:.04em; }}
    th,td {{ padding:.75rem; border-bottom:1px solid var(--line); vertical-align:top; }}
    tr:last-child td {{ border-bottom:0; }}
    td code {{ display:block; max-width:58ch; overflow-wrap:anywhere; font-size:.76rem; color:#365464; }}
    td small {{ display:block; color:var(--muted); }}
    .num {{ text-align:right; white-space:nowrap; font-family:var(--mono); }}
    .badge {{ display:inline-block; padding:.26rem .45rem; font:850 .68rem/1.15 var(--mono); letter-spacing:.03em; background:#e1e4df; }}
    .badge.exact,.badge.pass {{ color:#145d43; background:#dcefe5; }}
    .badge.semantic {{ color:#215d7a; background:#dcecf4; }}
    .badge.context {{ color:#875610; background:#f6e8ca; }}
    .badge.not-comparable {{ color:#8a302d; background:#f3ddda; }}
    .bottleneck {{ display:grid; grid-template-columns:56px 1fr; gap:1rem; padding:1.1rem 0; border-top:1px solid var(--line); }}
    .rank {{ display:grid; place-items:center; width:46px; height:46px; color:#fff; background:var(--orange); font:850 1rem/1 var(--mono); }}
    .bottleneck strong {{ color:var(--steel); font:800 1.08rem/1.3 var(--mono); }}
    .bottleneck p {{ margin:.35rem 0 0; color:var(--muted); }}
    details {{ margin:.7rem 0; border:1px solid var(--line); background:var(--sheet); }}
    summary {{ cursor:pointer; padding:.85rem 1rem; font-weight:750; }}
    summary:hover {{ background:#f3f0e7; }}
    .detail-body {{ padding:.1rem 1rem 1rem; border-top:1px solid var(--line); }}
    .detail-body code {{ display:block; overflow-wrap:anywhere; color:#365464; }}
    .source-list {{ list-style:none; padding:0; margin:1rem 0 0; display:grid; gap:.55rem; }}
    .source-list li {{ padding:.8rem; border-left:4px solid var(--steel2); background:var(--sheet); }}
    .source-list code,.source-list span {{ display:block; overflow-wrap:anywhere; font-size:.76rem; color:var(--muted); }}
    .limits {{ padding:1.2rem 1.4rem 1.2rem 2.5rem; background:#f6e8ca; border:1px solid #dfc58e; }}
    .limits li {{ margin:.55rem 0; }}
    footer {{ margin-top:4.4rem; padding:1.4rem; color:#c7d7dd; background:var(--steel); font-size:.8rem; }}
    @media(max-width:1050px) {{
      .layout {{ grid-template-columns:1fr; }} aside {{ position:relative; height:auto; border-right:0; border-bottom:5px solid var(--acid); }}
      nav {{ grid-template-columns:repeat(3,minmax(0,1fr)); }} .aside-note {{ position:static; margin-top:1.2rem; }}
      .cards {{ grid-template-columns:repeat(2,minmax(0,1fr)); }} .flow {{ grid-template-columns:repeat(3,minmax(0,1fr)); }}
    }}
    @media(max-width:680px) {{
      main {{ padding:1rem .8rem 3rem; }} nav {{ grid-template-columns:1fr 1fr; }}
      .hero {{ padding:2rem 1.1rem; }} .hero-grid,.cards,.two-col {{ grid-template-columns:1fr; }}
      .flow {{ grid-template-columns:1fr; }} .flow div:not(:last-child):after {{ content:"↓"; right:auto; left:48%; top:auto; bottom:-1.15rem; }}
      th,td {{ min-width:150px; }} h1 {{ font-size:2.45rem; }}
    }}
    @media(prefers-reduced-motion:reduce) {{ html {{ scroll-behavior:auto; }} }}
    @media print {{
      .ribbon {{ position:static; }} .layout {{ display:block; }} aside {{ display:none; }} main {{ padding:0; }}
      .hero,footer {{ print-color-adjust:exact; }} section {{ break-inside:avoid; }} details {{ break-inside:avoid; }} details > * {{ display:block; }}
    }}
  </style>
</head>
<body>
  <a class="skip-link" href="#report-main">跳到主要內容</a>
  <div class="ribbon partial-ribbon">HCC1395 ONLY · DATASET GATE · NON-PRODUCTION · 1 / 7 DATASETS</div>
  <div class="layout">
    <aside>
      <div class="brand">LongLineage<small>VALIDATED SCIENTIFIC AUDIT / HCC1395</small></div>
      <nav aria-label="報告章節">
        <a href="#verdict">結論</a><a href="#chain">證據鏈</a><a href="#inputs">輸入契約</a><a href="#methods">判斷與設定</a>
        <a href="#m1">M1 守恆</a><a href="#cooccurrence">sSNV 共現</a><a href="#topology">區域拓樸</a>
        <a href="#comparison">舊新比較</a><a href="#timing">時間與瓶頸</a><a href="#artifacts">Artifacts</a>
        <a href="#checks">Validator checks</a><a href="#limits">限制</a>
      </nav>
      <p class="aside-note">Offline self-contained · Python presentation only · C++ scientific outputs remain authoritative.</p>
    </aside>
    <main id="report-main">
      <header class="hero">
        <div class="eyebrow">Task B · comprehensive validation · 影響高 / 信心高</div>
        <h1>HCC1395 完整科學運算與資料確認</h1>
        <p class="hero-lead">完整 chr1–22 HCC1395 dataset gate 已由獨立 validator 凍結；工程與 worker-determinism gate 通過，但 legacy Python M1 decision parity 尚未通過。本頁只呈現已驗證的 C++ artifacts、守恆計數、時間與歷史證據邊界。</p>
        <div class="state-line"><span class="terminal">{esc(terminal_display)}</span>{badge("PASS")}<span>dataset gate PASS · legacy M1 parity BLOCKED · production_claim_allowed = false</span></div>
        <div class="hero-grid">
          <div><span>w24 run</span><strong>{esc(model['run_ids']['w24'])}</strong></div>
          <div><span>w40 run</span><strong>{esc(model['run_ids']['w40'])}</strong></div>
          <div><span>Autosomal sites</span><strong>{fmt_int(counts['site_keys'])}</strong></div>
          <div><span>8-artifact determinism</span><strong>{model['determinism']['artifact_count']} / 8 PASS</strong></div>
        </div>
        <p class="hero-lead"><small>Validated: {esc(validated_display)}</small></p>
      </header>

      <section id="verdict">
        <div class="kicker">01 · Answer first</div>
        <h2>工程執行與凍結通過；legacy Python 科學 parity 未通過</h2>
        <p class="lead">終態、receipt chain、artifact physical SHA、semantic bindings、site/M1/M2/topology 守恆與輸入快照均已 fail-closed 重驗；但 historical M1 stable membership 仍有 per-key churn。結論可用於定位差異與優化流程，不足以宣稱 Python-equivalent，也不可把 1/7 gate 改寫成 production claim。</p>
        <div class="cards">
          <article class="card"><span class="label">Terminal state</span><span class="value">FROZEN</span><p>RUNNING → VALIDATED → DATASET_GATE frozen</p></article>
          <article class="card"><span class="label">Validation checks</span><span class="value">{fmt_int(len(model['validation_checks']))}</span><p>全部 PASS；validator independent=true</p></article>
          <article class="card"><span class="label">Site population</span><span class="value">{fmt_int(counts['site_keys'])}</span><p>missing / extra / duplicate 全為 0</p></article>
          <article class="card"><span class="label">Production claim</span><span class="value">禁止</span><p>DATASET_GATE profile 固定 false</p></article>
        </div>
        <div class="callout"><strong>Legacy parity blocker：</strong>old stable {fmt_int(historical['m1']['stable_assignments'])}、new stable {fmt_int(counts['m1_stable_assignments'])}，但 symmetric difference = {fmt_int(historical['m1']['stable_transition']['symmetric_difference'])}；Jaccard intersection / union = {fmt_int(historical['m1']['stable_transition']['jaccard']['intersection'])} / {fmt_int(historical['m1']['stable_transition']['jaccard']['union'])}。Aggregate 淨差不能取代逐站 parity。</div>
        <div class="callout"><strong>最重要的輸入澄清：</strong>{esc(model['input_contract']['statement'])} HP/PS 由同一 frozen sidecar exact-join；不可把 raw BAM 說成已持久化 tagged BAM。</div>
      </section>

      <section id="chain">
        <div class="kicker">02 · Evidence chain</div>
        <h2>不是「程式跑完」；而是五層證據都封閉</h2>
        <div class="flow">
          <div><span>01 / INPUT</span><strong>8 roles locked</strong><small>raw BAM、BAI、VCF/CSI、sidecar/TBI、FASTA/FAI</small></div>
          <div><span>02 / C++</span><strong>Science artifacts</strong><small>M1 → co-occurrence → topology</small></div>
          <div><span>03 / RECEIPT</span><strong>Producer closeout</strong><small>truth_fields_seen=0</small></div>
          <div><span>04 / REPLAY</span><strong>Independent validator</strong><small>unlinked producer kernels</small></div>
          <div><span>05 / FREEZE</span><strong>Atomic terminal</strong><small>input snapshots equal</small></div>
          <div><span>06 / PRESENT</span><strong>HTML only</strong><small>no Python science recomputation</small></div>
        </div>
        <div class="table-wrap" tabindex="0" aria-label="Worker-invariant science artifacts"><table>
          <thead><tr><th>Invariant artifact</th><th>Schema</th><th>Logical rows</th><th>Semantic SHA</th><th>w24 = w40</th></tr></thead>
          <tbody>{determinism_rows}</tbody>
        </table></div>
        <div class="callout"><strong>Manifest 差異白名單：</strong>{esc(", ".join(model['determinism']['manifest_difference_pointers']))}；summary projection digest = <code>{esc(model['determinism']['summary_projection_sha256'])}</code>。</div>
      </section>

      <section id="inputs">
        <div class="kicker">03 · Input contract</div>
        <h2>raw BAM + latest HP/PS sidecar；truth 輸入為零</h2>
        <p class="lead">這裡顯示由 input authority exact-join 投影的 portable token 與 producer mount 類型；完整 canonical path 只留在外部 frozen receipt。raw BAM 與 latest_hp_ps_sidecar 角色彼此分離，這就是「不是 tagged BAM」的契約證據。</p>
        <div class="table-wrap" tabindex="0" aria-label="Frozen input authority roles"><table>
          <thead><tr><th>Role</th><th>Authority token</th><th>Filesystem</th><th>Mode</th></tr></thead>
          <tbody>{mount_rows}</tbody>
        </table></div>
      </section>

      <section id="methods">
        <div class="kicker">03B · Methods and exact settings</div>
        <h2>判斷順序、門檻、fallback 與 abstention 全部來自 frozen C++ contract</h2>
        <p class="lead">下表直接映射 <code>contracts/v1/science_parameters.json</code>；HTML 沒有在 Python 重新計算 p-value、FDR、cluster 或 topology。任何不足證據、state ceiling、MN mismatch、非等價 duplicate 或 incomplete family 都保持 fail closed / unresolved。</p>
        <div class="callout"><strong>Representation provenance gap：</strong>frozen manifest／receipt 未獨立記錄 run-level M1 representation。表內 raw ML point estimator 與程式可能採用的 compatibility serialization 不能互相代稱；在新增明確欄位並重跑前，本頁不宣稱此選擇已由 receipt 綁定。</div>
        <div class="table-wrap" tabindex="0" aria-label="Frozen science method settings"><table>
          <thead><tr><th>Stage</th><th>Setting</th><th>Frozen value</th><th>判斷理由與失敗行為</th></tr></thead>
          <tbody>{method_rows}</tbody>
        </table></div>
      </section>

      <section id="m1">
        <div class="kicker">04 · M1</div>
        <h2>狀態守恆通過；stable membership parity 仍被阻擋</h2>
        <div class="two-col">
          <div class="panel"><h3>現行 C++ frozen census</h3>{m1_bars}</div>
          <div class="panel"><h3>舊 Python context</h3>
            <p><strong>{fmt_int(historical['m1']['stable_assignments'])}</strong> stable-null multigroup rows。</p>
            <p>{fmt_int(historical['m1']['evaluable'])} evaluable；{fmt_int(historical['m1']['insufficient_alt_reads'])} insufficient；{fmt_int(historical['m1']['incomplete_distance'])} incomplete。</p>
            <div class="callout"><strong>不可誤讀：</strong>舊 {fmt_int(historical['m1']['stable_assignments'])} 是 legacy candidate alias，不是 strict-confirmed co-occurrence 或 subclone；{fmt_int(historical['m1']['strict_confirm_not_run'])} 個 strict_confirm_status 全為 NOT_RUN。</div>
          </div>
        </div>
        <div class="callout"><strong>逐站差異：</strong>true→true {fmt_int(historical['m1']['stable_transition']['true_to_true'])}、true→false {fmt_int(historical['m1']['stable_transition']['true_to_false'])}、false→true {fmt_int(historical['m1']['stable_transition']['false_to_true'])}、false→false {fmt_int(historical['m1']['stable_transition']['false_to_false'])}。在解釋 RNG、distance peel、tie-break 或 representation 差異並通過 real golden 之前，不得將 C++ stable decisions 宣稱為 Python parity。</div>
      </section>

      <section id="cooccurrence">
        <div class="kicker">05 · sSNV co-occurrence</div>
        <h2>Pair rows ≠ positive discoveries；舊流程無 formal comparable authority，不能宣稱 parity</h2>
        <div class="cards">
          <article class="card"><span class="label">Candidate pair rows</span><span class="value">{fmt_int(cooccurrence['pair_rows'])}</span><p>序列化 records，並非陽性發現數</p></article>
          <article class="card"><span class="label">Eligible exact-family</span><span class="value">{fmt_int(cooccurrence['eligible_exact_family_pairs'])}</span><p>唯一進入 global FDR family 的 rows</p></article>
          <article class="card"><span class="label">Exact BY discoveries</span><span class="value">{fmt_int(cooccurrence['global_by_discoveries'])}</span><p>q ≤ 0.05 且 effect gate（V ≥ 0.30、ΔAF ≥ 0.50）；BY discoveries 是 BH discoveries 的子集（可相等）</p></article>
          <article class="card"><span class="label">Formal pair BY-confirmed</span><span class="value">{fmt_int(cooccurrence['formal_pair_by_confirmed'])}</span><p>BY + conditional + callability 全通過</p></article>
        </div>
        <div class="table-wrap" tabindex="0" aria-label="Serialized co-occurrence census"><table>
          <thead><tr><th>C++ serialized census</th><th>Count</th><th>守恆／解讀</th></tr></thead>
          <tbody>
            <tr><td>M2-eligible focal sites</td><td class="num">{fmt_int(counts['m2_eligible'])}</td><td>screen 通過的 focal sites，不是 pair discoveries</td></tr>
            <tr><td>Exact-identifiable pairs</td><td class="num">{fmt_int(cooccurrence['exact_identifiable_pairs'])}</td><td>≤ pair rows</td></tr>
            <tr><td>Family bins</td><td class="num">{fmt_int(cooccurrence['ineligible_m2_screen_pairs'])} / {fmt_int(cooccurrence['eligible_endpoint_a_not_testable_pairs'])} / {fmt_int(cooccurrence['eligible_exact_not_identifiable_pairs'])} / {fmt_int(cooccurrence['eligible_exact_family_pairs'])}</td><td>ineligible / endpoint-A not-testable / exact not-identifiable / exact-family；總和 = pair rows</td></tr>
            <tr><td>Global BH discoveries</td><td class="num">{fmt_int(cooccurrence['global_bh_discoveries'])}</td><td>q ≤ 0.05 且使用相同 effect gate；formal ≤ BY ≤ BH ≤ eligible exact-family</td></tr>
            <tr><td>Co-occurrence site rows</td><td class="num">{fmt_int(cooccurrence['cooccurrence_site_rows'])}</td><td>joint PASS + not-testable 完整分割</td></tr>
            <tr><td>Joint-signature PASS / not-testable</td><td class="num">{fmt_int(cooccurrence['joint_signature_pass_sites'])} / {fmt_int(cooccurrence['joint_signature_not_testable_sites'])}</td><td>PASS = topology units ({fmt_int(cooccurrence['topology_units'])})</td></tr>
          </tbody>
        </table></div>
        <div class="callout"><strong>Authority boundary：</strong>以上數字由 C++ audit 對 validator 已驗證的序列化 pair/site artifacts 做 census，w24 = w40；audit 不重算 p/q。Python 不讀 scientific artifact data rows（M1/co-occurrence/topology 等）；允許讀 artifact catalog/semantic digest metadata rows 與 summary/receipts，且不重算科學結果。</div>
        <div class="callout"><strong>歷史邊界：</strong>C++ audit receipt 僅支持「舊正式流程在發布前失敗，沒有 formal comparable authority」；因此不能宣稱舊新數值 parity 或 speedup。</div>
      </section>

      <section id="topology">
        <div class="kicker">06 · Regional topology</div>
        <h2>完整與未完成分開；未完成 unit 不得發布 winner</h2>
        <div class="two-col">
          <div class="panel"><h3>現行 LongLineage</h3>{topology_bars}
            <p><strong>{fmt_int(counts['topology_incomplete_units_with_winner'])}</strong> incomplete units with winner（必須為 0）。</p>
          </div>
          <div class="panel"><h3>舊 region-tree context</h3>{old_region_bars}</div>
        </div>
        <p class="lead">{esc(regional_context_text)}</p>
      </section>

      <section id="comparison">
        <div class="kicker">07 · Comparison taxonomy</div>
        <h2>EXACT、SEMANTIC、CONTEXT、NOT_COMPARABLE 四層不能混用</h2>
        <div class="table-wrap" tabindex="0" aria-label="Historical comparison taxonomy"><table>
          <thead><tr><th>Class</th><th>Topic</th><th>Earlier evidence</th><th>LongLineage</th><th>Allowed conclusion</th></tr></thead>
          <tbody>{comparison_rows}</tbody>
        </table></div>
      </section>

      <section id="timing">
        <div class="kicker">08 · Timing and bottlenecks</div>
        <h2>w24 與 w40 分開量測；producer、validator、report 不混加</h2>
        <p class="lead">每個 stage 都由 execution evidence 綁定 stdout 與 GNU time log。歷史 runtime 的 stage、scope、cache condition 不同，C++ audit 明示 NOT_COMPARABLE，不計算虛假的 speedup。</p>
        <div class="cards">
          <article class="card"><span class="label">w24 science wall</span><span class="value">{fmt_duration(performance['wall_seconds'])}</span><p>producer run receipt</p></article>
          <article class="card"><span class="label">CPU user + sys</span><span class="value">{fmt_duration(performance['user_seconds'] + performance['system_seconds'])}</span><p>{performance['peak_threads']} peak threads</p></article>
          <article class="card"><span class="label">Peak memory</span><span class="value">{fmt_bytes(performance['memory_peak_bytes'])}</span><p>OOM events = {performance['oom_events']}</p></article>
          <article class="card"><span class="label">Logical records</span><span class="value">{fmt_int(performance['logical_records'])}</span><p>{fmt_bytes(performance['logical_bytes'])} logical bytes</p></article>
        </div>
        <div class="table-wrap" tabindex="0" aria-label="Execution timing stages"><table>
          <thead><tr><th>Stage</th><th>Status</th><th>Time</th><th>Interpretation boundary</th></tr></thead>
          <tbody>{timing_rows}</tbody>
        </table></div>
        <div style="margin-top:1.4rem">{bottlenecks}</div>
      </section>

      <section id="artifacts">
        <div class="kicker">09 · Frozen artifacts</div>
        <h2>每個輸出都有 physical 與 semantic digest</h2>
        <p class="lead">本 builder 在產生 HTML 前重算所有 receipt-declared artifact 與 index physical SHA；semantic SHA 則跨 catalog、semantic_digests.tsv、producer receipt 與 final run receipt 逐列一致。</p>
        <div class="table-wrap" tabindex="0" aria-label="Frozen output artifact inventory"><table>
          <thead><tr><th>Artifact</th><th>Path</th><th>Schema</th><th>Logical rows</th><th>w24 / w40 physical</th><th>Semantic SHA</th></tr></thead>
          <tbody>{artifact_rows}</tbody>
        </table></div>
        <h3 style="margin-top:2rem">歷史 authority（只作明示層級的比較）</h3>
        <ul class="source-list">{historical_files}</ul>
      </section>

      <section id="checks">
        <div class="kicker">10 · Independent replay</div>
        <h2>Validator 判斷細節</h2>
        <p class="lead">每列顯示 expected、observed 與 evidence SHA。這些不是前端自行判斷的科學結論。</p>
        {validation_details}
      </section>

      <section id="limits">
        <div class="kicker">11 · Claim boundary</div>
        <h2>可以說什麼；不能說什麼</h2>
        <ul class="limits">{limit_rows}</ul>
      </section>

      <footer>
        HCC1395_ONLY_NON_PRODUCTION_DATASET_GATE · w24 + w40 independently frozen ·
        validation receipt {esc(short_sha(model['current_authority_sha256']['validation_receipt']))} ·
        audit receipt {esc(short_sha(model['current_authority_sha256']['audit_receipt']))} ·
        offline standalone HTML · Python presentation-only
      </footer>
    </main>
  </div>
</body>
</html>
"""


def atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.parent / f".{path.name}.tmp.{os.getpid()}"
    try:
        with temporary.open("wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def fixture_artifact(
    root: Path,
    artifact_id: str,
    relative_path: str,
    schema_name: str,
    schema_version: str,
    fmt: str,
    logical_rows: int,
    payload: bytes,
) -> Dict[str, Any]:
    path = root / relative_path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return {
        "artifact_id": artifact_id,
        "role": artifact_id,
        "relative_path": relative_path,
        "schema_name": schema_name,
        "schema_version": schema_version,
        "format": fmt,
        "size_bytes": len(payload),
        "physical_sha256": sha256_file(path),
        "logical_rows": logical_rows,
        "semantic_sha256": hashlib.sha256(b"semantic:" + artifact_id.encode()).hexdigest(),
        "index": None,
        "sensitivity": "REAL_RESTRICTED",
        "transform_id": "self_test",
        "producer_executable_sha256": hashlib.sha256(b"self-test-exe").hexdigest(),
        "inputs": [],
        "primary_key_first": None,
        "primary_key_last": None,
    }


def write_self_test_fixture(
    base: Path,
    run_id: str = "self-test-hcc1395-w24",
    run_directory: str = "run-w24",
    workers: int = 24,
    git_commit: str = "1" * 40,
    executable_sha: str | None = None,
    validator_sha: str | None = None,
    manifest_sha: str = SHA256_ZERO,
) -> Tuple[Path, Path]:
    """Create a minimal internally consistent frozen run for renderer tests."""

    repo = base / "repo"
    run_root = base / run_directory
    (repo / "schema").mkdir(parents=True, exist_ok=True)
    (repo / "contracts/v1").mkdir(parents=True, exist_ok=True)
    run_root.mkdir(parents=True, exist_ok=True)
    science_ids = [
        "site_reads",
        "methyl_calls",
        "bernoulli_upper",
        "m1_sites",
        "m1_assignments",
        "cooccurrence_pairs",
        "cooccurrence_sites",
        "topology_units",
        "summary",
    ]
    schemas = {
        "site_reads": ("longlineage.site_reads", "1.0.0", "TSV_BGZF"),
        "methyl_calls": ("longlineage.methyl_calls", "1.0.0", "TSV_BGZF"),
        "bernoulli_upper": ("longlineage.bernoulli_upper", "1.0.0", "LLM_BGZF"),
        "m1_sites": ("longlineage.m1_sites", "1.0.0", "TSV_BGZF"),
        "m1_assignments": ("longlineage.m1_assignment", "1.0.0", "JSONL_BGZF"),
        "cooccurrence_pairs": ("longlineage.cooccurrence_pairs", "1.0.1", "TSV_BGZF"),
        "cooccurrence_sites": ("longlineage.cooccurrence_sites", "1.0.0", "TSV_BGZF"),
        "topology_units": ("longlineage.topology_unit", "2.0.0", "JSONL_BGZF"),
    }
    summary = {
        "schema_name": "longlineage.summary",
        "schema_version": "1.0.0",
        "run_id": run_id,
        "scope": {
            "task_type": "B",
            "completeness": "FULL",
            "dataset_count": 1,
            "dataset_ids": ["HCC1395"],
            "site_population": "SELF_TEST_AUTOSOMAL",
        },
        "counts": {
            "site_keys": EXPECTED_SITE_KEYS,
            "site_keys_missing": 0,
            "site_keys_extra": 0,
            "site_keys_duplicate": 0,
            "m1_evaluable": 78_629,
            "m1_insufficient_alt_reads": 1_044,
            "m1_incomplete_distance": 14,
            "m1_stable_assignments": 2,
            "latest_tag_exact_joins": 4,
            "latest_tag_missing": 0,
            "latest_tag_conflict": 0,
            "latest_tag_multimatch": 0,
            "m2_eligible": 1,
            "m2_evaluable_ineligible": 0,
            "m2_axis_indeterminate": 1,
            "m2_group_count_gt10": 0,
            "raw_expected": 4,
            "raw_matched": 4,
            "raw_rg_only_duplicate_occurrences": 0,
            "topology_primary_hp_units": 1,
            "topology_regions": 2,
            "topology_fully_complete_regions": 1,
            "topology_incomplete_regions": 1,
            "topology_incomplete_units_with_winner": 0,
        },
        "phase_status": {
            "P0": "VERIFIED", "P1": "VERIFIED", "P2": "VERIFIED",
            "P3": "VERIFIED", "P4": "VERIFIED", "P5": "VERIFIED",
            "P6": "IN_PROGRESS", "P7": "NOT_STARTED", "P8": "NOT_STARTED",
        },
    }
    artifacts: Dict[str, Dict[str, Any]] = {}
    for artifact_id in science_ids[:-1]:
        schema_name, schema_version, fmt = schemas[artifact_id]
        payload = gzip.compress(f"fixture-{artifact_id}\n".encode(), mtime=0)
        logical_rows = (
            EXPECTED_SITE_KEYS
            if artifact_id in {"m1_sites", "cooccurrence_sites"}
            else 1
        )
        artifacts[artifact_id] = fixture_artifact(
            run_root, artifact_id, f"{artifact_id}.bgz",
            schema_name, schema_version, fmt, logical_rows, payload
        )
    summary_payload = (
        json.dumps(summary, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode()
    artifacts["summary"] = fixture_artifact(
        run_root, "summary", "summary.json", "longlineage.summary", "1.0.0",
        "JSON", 1, summary_payload
    )

    catalog_lines = [
        json.dumps(
            {
                "schema_name": "longlineage.artifact_catalog_record",
                "schema_version": "1.0.0",
                "run_id": run_id,
                "artifact": artifacts[artifact_id],
            },
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        for artifact_id in science_ids
    ]
    catalog_payload = gzip.compress(("\n".join(catalog_lines) + "\n").encode(), mtime=0)
    artifacts["artifact_catalog"] = fixture_artifact(
        run_root, "artifact_catalog", "artifact_catalog.jsonl.bgz",
        "longlineage.artifact_catalog_record", "1.0.0", "JSONL_BGZF",
        len(science_ids), catalog_payload
    )
    artifacts["data_lineage"] = fixture_artifact(
        run_root, "data_lineage", "data_lineage.jsonl.bgz",
        "longlineage.data_lineage_record", "1.0.0", "JSONL_BGZF",
        len(science_ids), gzip.compress(b"fixture-lineage\n", mtime=0)
    )
    semantic_ids = science_ids + ["artifact_catalog", "data_lineage"]
    semantic_lines = [
        "artifact_id\tschema_name\tschema_version\tlogical_rows\tsemantic_sha256"
    ]
    for artifact_id in sorted(semantic_ids):
        row = artifacts[artifact_id]
        semantic_lines.append(
            "\t".join(
                [
                    artifact_id, row["schema_name"], row["schema_version"],
                    str(row["logical_rows"]), row["semantic_sha256"],
                ]
            )
        )
    artifacts["semantic_digests"] = fixture_artifact(
        run_root, "semantic_digests", "semantic_digests.tsv",
        "longlineage.semantic_digest", "1.0.0", "TSV",
        len(semantic_ids), ("\n".join(semantic_lines) + "\n").encode()
    )

    run_ids = list(artifacts)
    catalog_contract = {
        "schema_name": "longlineage.artifact_schema_catalog",
        "schema_version": "1.0.0",
        "run_membership": {
            "scientific_artifact_ids": science_ids,
            "artifact_catalog_row_artifact_ids": science_ids,
            "semantic_digest_artifact_ids": semantic_ids,
            "producer_receipt_artifact_ids": run_ids,
            "run_receipt_artifact_ids": run_ids,
        },
    }
    (repo / "schema/catalog.json").write_text(
        json.dumps(catalog_contract, separators=(",", ":"), sort_keys=True) + "\n"
    )
    science_fixture_source = (
        Path(__file__).resolve().parents[1] / "contracts/v1/science_parameters.json"
    )
    require(science_fixture_source.is_file(),
            "self-test cannot locate frozen science parameters")
    (repo / "contracts/v1/science_parameters.json").write_bytes(
        science_fixture_source.read_bytes()
    )
    catalog_sha = sha256_file(repo / "schema/catalog.json")
    science_sha = sha256_file(repo / "contracts/v1/science_parameters.json")
    snapshot = hashlib.sha256(b"snapshot").hexdigest()
    executable_sha = executable_sha or artifacts["summary"]["producer_executable_sha256"]
    validator_sha = validator_sha or hashlib.sha256(b"validator").hexdigest()
    mounts = [
        {
            "dataset_id": "HCC1395", "role": role,
            "canonical_path": "/" + "big8_disk" + f"/private/{role}",
            "mount_source": "fixture:/" + "big8_disk",
            "filesystem_type": "tmpfs", "readonly": True,
            "mount_options_sha256": hashlib.sha256(role.encode()).hexdigest(),
        }
        for role in [
            "raw_bam", "raw_bam_index", "pass_biallelic_ssnv_vcf",
            "pass_biallelic_ssnv_vcf_index", "latest_hp_ps_sidecar",
            "latest_hp_ps_sidecar_index", "reference_fasta", "reference_fai",
        ]
    ]
    performance = {
        "wall_seconds": 12.5, "user_seconds": 20.0, "system_seconds": 2.0,
        "memory_peak_bytes": 1024, "oom_events": 0, "io_read_bytes": 2048,
        "io_write_bytes": 4096, "major_page_faults": 0, "minor_page_faults": 10,
        "peak_threads": workers, "queue_wait_seconds": 0.1, "reorder_wait_seconds": 0.2,
        "task_latency_seconds": {"p50": 1.0, "p95": 2.0, "p99": 3.0, "max": 4.0},
        "logical_records": 12, "logical_bytes": 2048, "final_file_count": 15,
        "transient_file_count": 0, "cache_condition": "UNKNOWN",
    }
    producer = {
        "schema_name": "longlineage.producer_receipt", "schema_version": "1.0.0",
        "run_id": run_id, "state": "RUNNING",
        "producer_outcome": "READY_FOR_VALIDATION",
        "producer_executable_sha256": executable_sha,
        "producer_hostname": "fixture", "producer_kernel_release": "fixture",
        "input_mount_identity": mounts, "manifest_sha256": manifest_sha,
        "input_snapshot_before_sha256": snapshot, "input_snapshot_after_sha256": snapshot,
        "schema_catalog_sha256": catalog_sha, "science_parameters_sha256": science_sha,
        "run_receipt_draft": {}, "artifacts": list(artifacts.values()),
        "truth_fields_seen": 0, "failure_reason": None,
        "finished_at": "2026-07-20T00:00:00Z",
    }
    (run_root / "receipts").mkdir(exist_ok=True)
    producer_path = run_root / "receipts/producer_receipt.json"
    producer_path.write_text(json.dumps(producer, separators=(",", ":"), sort_keys=True) + "\n")
    producer_sha = sha256_file(producer_path)
    checksum_rows = {}
    for artifact in artifacts.values():
        checksum_rows[artifact["relative_path"]] = artifact["physical_sha256"]
    checksum_rows["receipts/producer_receipt.json"] = producer_sha
    checksums_path = run_root / "checksums.sha256"
    checksums_path.write_text(
        "".join(f"{digest}  {path}\n" for path, digest in sorted(checksum_rows.items()))
    )
    checksums_sha = sha256_file(checksums_path)
    validation = {
        "schema_name": "longlineage.validation_receipt", "schema_version": "1.0.0",
        "run_id": run_id, "validation_profile": "DATASET_GATE",
        "production_claim_allowed": False, "producer_receipt_sha256": producer_sha,
        "producer_executable_sha256": executable_sha,
        "validator_executable_sha256": validator_sha,
        "producer_hostname": "fixture", "producer_kernel_release": "fixture",
        "validator_hostname": "fixture", "validator_kernel_release": "fixture",
        "input_mount_identity_sha256": hashlib.sha256(b"mounts").hexdigest(),
        "schema_catalog_sha256": catalog_sha, "science_parameters_sha256": science_sha,
        "all_pass": True,
        "checks": [{
            "check_id": "SELF_TEST", "status": "PASS", "reason": None,
            "observed": (
                "/" + "big7_disk" + "/private/" + "chr" + "1" + ":" + "12345"
            ),
            "expected": True,
            "evidence_sha256": hashlib.sha256(b"evidence").hexdigest(),
        }],
        "validated_at": "2026-07-20T00:01:00Z", "validator_independent": True,
        "linked_producer_kernels": False, "input_snapshot_before_sha256": snapshot,
        "input_snapshot_after_sha256": snapshot,
    }
    validation_path = run_root / "validation_receipt.json"
    validation_path.write_text(
        json.dumps(validation, separators=(",", ":"), sort_keys=True) + "\n"
    )
    validation_sha = sha256_file(validation_path)
    validator_sha = validation["validator_executable_sha256"]
    run = {
        "schema_name": "longlineage.run_receipt", "schema_version": "1.0.0",
        "run_id": run_id, "state": EXPECTED_TERMINAL_STATE,
        "validation_profile": "DATASET_GATE", "production_claim_allowed": False,
        "production_executable": {
            "name": "longlineage", "version": "self-test",
            "git_commit": git_commit, "executable_sha256": executable_sha,
            "compiler": "self-test", "hts" "lib_version": "1.18",
        }, "producer_hostname": "fixture",
        "producer_kernel_release": "fixture", "validator_hostname": "fixture",
        "validator_kernel_release": "fixture",
        "input_mount_identity_sha256": validation["input_mount_identity_sha256"],
        "manifest_sha256": manifest_sha, "input_lock_sha256": SHA256_ZERO,
        "phase_ledger_sha256": SHA256_ZERO, "artifacts": list(artifacts.values()),
        "truth_fields_seen": 0, "input_snapshot_before_sha256": snapshot,
        "input_snapshot_after_sha256": snapshot, "schema_catalog_sha256": catalog_sha,
        "science_parameters_sha256": science_sha,
        "state_history": [
            {"sequence": 0, "state": "RUNNING", "at": "2026-07-20T00:00:00Z",
             "actor_executable_sha256": executable_sha, "previous_event_sha256": None},
            {"sequence": 1, "state": "VALIDATED", "at": "2026-07-20T00:01:00Z",
             "actor_executable_sha256": validator_sha, "previous_event_sha256": SHA256_ZERO},
            {"sequence": 2, "state": EXPECTED_TERMINAL_STATE, "at": "2026-07-20T00:01:00Z",
             "actor_executable_sha256": validator_sha, "previous_event_sha256": SHA256_ZERO},
        ],
        "performance": performance, "producer_receipt_sha256": producer_sha,
        "validation_receipt_sha256": validation_sha, "checksums_sha256": checksums_sha,
    }
    (run_root / "run_receipt.json").write_text(
        json.dumps(run, separators=(",", ":"), sort_keys=True) + "\n"
    )
    return repo, run_root


def write_json_document(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        + "\n",
        encoding="utf-8",
    )


def absolute_binding(path: Path) -> Dict[str, Any]:
    path = path.resolve(strict=True)
    return {
        "path": str(path),
        "size_bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def write_gnu_time_fixture(
    directory: Path,
    stem: str,
    marker: str,
    wall_seconds: float,
    user_seconds: float,
    system_seconds: float,
    max_rss_kib: int,
) -> Dict[str, Any]:
    stdout_path = directory / f"{stem}.stdout.log"
    time_path = directory / f"{stem}.stderr.time.log"
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stdout_path.write_text(
        json.dumps({"status": marker}, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    minutes = int(wall_seconds // 60)
    seconds = wall_seconds - minutes * 60
    time_path.write_text(
        "\n".join(
            [
                '\tCommand being timed: "synthetic"',
                f"\tUser time (seconds): {user_seconds:.2f}",
                f"\tSystem time (seconds): {system_seconds:.2f}",
                (
                    "\tElapsed (wall clock) time (h:mm:ss or m:ss): "
                    f"{minutes}:{seconds:05.2f}"
                ),
                f"\tMaximum resident set size (kbytes): {max_rss_kib}",
                "\tExit status: 0",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return {
        "stdout_log": absolute_binding(stdout_path),
        "time_log": absolute_binding(time_path),
        "status_marker": marker,
        "exit_code": 0,
        "wall_seconds": wall_seconds,
        "user_seconds": user_seconds,
        "system_seconds": system_seconds,
        "max_rss_kib": max_rss_kib,
    }


def create_synthetic_manifest(
    path: Path,
    run_id: str,
    workers: int,
    dataset_authority_sha: str,
    production_authority_sha: str,
    science_parameters_sha: str,
    schema_catalog_sha: str,
    input_files: List[Dict[str, Any]],
) -> Dict[str, Any]:
    manifest = {
        "schema_name": "longlineage.production_manifest",
        "schema_version": "1.1.0",
        "authority_profile": "HCC1395_DATASET_GATE",
        "run_id": run_id,
        "output_root": str(path.parent / ".staging" / run_id),
        "datasets": [
            {
                "dataset_id": EXPECTED_DATASET_ID,
                "dataset_order": 0,
                "files": input_files,
            }
        ],
        "runtime": {
            "compute_workers": workers,
            "writer_threads": 4,
            "coordinator_slots": 2,
            "buffer_bytes": 8_589_934_592,
            "max_focal_sites_per_block": 4096,
            "max_estimated_alignments_per_block": 250_000,
            "halo_bp": 5000,
        },
        "contract_bindings": {
            "science_parameters_sha256": science_parameters_sha,
            "schema_catalog_sha256": schema_catalog_sha,
            "status_reason_registry_sha256": hashlib.sha256(b"status").hexdigest(),
            "type_registry_sha256": hashlib.sha256(b"types").hexdigest(),
            "transform_registry_sha256": hashlib.sha256(b"transforms").hexdigest(),
            "authority_manifest_sha256": hashlib.sha256(b"authority").hexdigest(),
            "source_to_target_manifest_sha256": hashlib.sha256(b"source").hexdigest(),
            "production_input_authority_sha256": production_authority_sha,
            "dataset_gate_input_authority_sha256": dataset_authority_sha,
            "schema_id_registry_sha256": hashlib.sha256(b"schema-ids").hexdigest(),
            "release_attestation_sha256": hashlib.sha256(b"release").hexdigest(),
        },
    }
    write_json_document(path, manifest)
    return manifest


def create_dual_self_test_fixture(base: Path) -> Dict[str, Path]:
    base.mkdir(parents=True, exist_ok=True)
    repo = base / "repo"
    oracle = base / "authorities"
    logs = base / "logs"
    manifests_dir = base / "manifests"
    executable_dir = base / "executables"
    for directory in (repo, oracle, logs, manifests_dir, executable_dir):
        directory.mkdir(parents=True, exist_ok=True)
    (repo / "schema").mkdir(parents=True, exist_ok=True)
    (repo / "contracts/v1").mkdir(parents=True, exist_ok=True)
    science_source = (
        Path(__file__).resolve().parents[1] / "contracts/v1/science_parameters.json"
    )
    (repo / "contracts/v1/science_parameters.json").write_bytes(
        science_source.read_bytes()
    )
    science_ids = EXPECTED_ARTIFACT_IDS + ["summary"]
    semantic_ids = science_ids + ["artifact_catalog", "data_lineage"]
    run_ids_membership = semantic_ids + ["semantic_digests"]
    catalog_contract = {
        "schema_name": "longlineage.artifact_schema_catalog",
        "schema_version": "1.0.0",
        "run_membership": {
            "scientific_artifact_ids": science_ids,
            "artifact_catalog_row_artifact_ids": science_ids,
            "semantic_digest_artifact_ids": semantic_ids,
            "producer_receipt_artifact_ids": run_ids_membership,
            "run_receipt_artifact_ids": run_ids_membership,
        },
    }
    write_json_document(repo / "schema/catalog.json", catalog_contract)
    git_output(repo, "init", "-q")
    git_output(repo, "config", "user.name", "LongLineage self-test")
    git_output(repo, "config", "user.email", "self-test@longlineage.invalid")
    git_output(repo, "add", "schema/catalog.json", "contracts/v1/science_parameters.json")
    git_output(repo, "commit", "-q", "-m", "synthetic science execution source")
    science_commit = git_output(repo, "rev-parse", "HEAD")

    input_files = []
    authority_files = []
    for index, role in enumerate(sorted(EXPECTED_INPUT_ROLES), 1):
        digest = hashlib.sha256(f"input:{role}".encode()).hexdigest()
        size = 1000 + index
        input_files.append(
            {
                "role": role,
                "path": "/" + "big7_disk" + f"/private/input/{role}",
                "size_bytes": size,
                "sha256": digest,
            }
        )
        authority_files.append(
            {
                "role": role,
                "path_token": f"SYNTHETIC_{role.upper()}",
                "size_bytes": size,
                "sha256": digest,
            }
        )
    dataset_authority = {
        "schema_name": "longlineage.dataset_gate_input_authority",
        "schema_version": "1.0.0",
        "authority_id": "SYNTHETIC_HCC1395_REPORT_SELF_TEST",
        "authority_profile": "HCC1395_DATASET_GATE",
        "allowed_terminal_state": EXPECTED_TERMINAL_STATE,
        "dataset_id": EXPECTED_DATASET_ID,
        "dataset_order": 0,
        "truth_fields": 0,
        "private_source_paths_stored": False,
        "tagged_bam_persisted": False,
        "latest_tag_join": "EXACT_PROJECTION_NO_FALLBACK",
        "variant_scope": {
            "all_pass_biallelic_ssnv": EXPECTED_SITE_KEYS,
            "autosomal_chr1_to_chr22": EXPECTED_SITE_KEYS,
            "outside_autosomal_scope": 0,
        },
        "files": authority_files,
        "full_content_freeze": {
            "command": "synthetic",
            "started_at": "2026-07-20T00:00:00Z",
            "observed_complete_no_later_than": "2026-07-20T00:00:01Z",
            "observed_wall_upper_bound_seconds": 1,
            "timing_precision": "SYNTHETIC",
            "cost_domain": "SELF_TEST",
        },
        "claim": {
            "dataset_gate_allowed": True,
            "production_seven_dataset_claim_allowed": False,
            "cross_dataset_generalization_allowed": False,
        },
    }
    dataset_authority_path = oracle / "dataset_gate_input_authority.json"
    write_json_document(dataset_authority_path, dataset_authority)
    role_rows = {row["role"]: row for row in authority_files}
    production_authority = {
        "schema_name": "longlineage.production_input_authority",
        "schema_version": "1.0.0",
        "profile_id": "SYNTHETIC_RAW_ALL_V2",
        "dataset_order": [EXPECTED_DATASET_ID],
        "constraints": {
            "latest_tag_join": "EXACT_PROJECTION_NO_FALLBACK",
            "tagged_bam_output_allowed": False,
            "production_requires_exact_dataset_set": True,
            "private_source_paths_stored": False,
        },
        "source_closeout": {
            "schema_name": "synthetic.closeout",
            "schema_version": "1.0.0",
            "size_bytes": 1,
            "sha256": hashlib.sha256(b"closeout").hexdigest(),
            "dataset_count": 1,
            "all_pass": True,
            "total_mapped_alignments": 4,
        },
        "datasets": [
            {
                "dataset_id": EXPECTED_DATASET_ID,
                "dataset_order": 0,
                "source_receipt_sha256": hashlib.sha256(b"source-receipt").hexdigest(),
                "mapped_alignment_count": 4,
                "persisted_tagged_bam": False,
            }
        ],
    }
    production_hcc = production_authority["datasets"][0]
    for key in (
        "latest_hp_ps_sidecar",
        "latest_hp_ps_sidecar_index",
        "pass_biallelic_ssnv_vcf",
        "pass_biallelic_ssnv_vcf_index",
    ):
        production_hcc[key] = {
            "size_bytes": role_rows[key]["size_bytes"],
            "sha256": role_rows[key]["sha256"],
        }
    production_authority_path = oracle / "production_input_authority.json"
    write_json_document(production_authority_path, production_authority)

    run_ids = {
        "w24": "self-test-hcc1395-w24",
        "w40": "self-test-hcc1395-w40",
    }
    manifest_paths = {
        label: manifests_dir / f"{run_ids[label]}.production_manifest.json"
        for label in run_ids
    }
    for label, workers in (("w24", 24), ("w40", 40)):
        create_synthetic_manifest(
            manifest_paths[label],
            run_ids[label],
            workers,
            sha256_file(dataset_authority_path),
            sha256_file(production_authority_path),
            sha256_file(repo / "contracts/v1/science_parameters.json"),
            sha256_file(repo / "schema/catalog.json"),
            input_files,
        )

    producer_binary = executable_dir / "longlineage"
    validator_binary = executable_dir / "longlineage-validate"
    governance_binary = executable_dir / "longlineage-governance"
    producer_binary.write_bytes(b"synthetic producer executable\n")
    validator_binary.write_bytes(b"synthetic validator executable\n")
    governance_binary.write_bytes(b"synthetic governance executable\n")
    producer_sha = sha256_file(producer_binary)
    validator_sha = sha256_file(validator_binary)
    run_roots = {}
    for label, workers in (("w24", 24), ("w40", 40)):
        _, run_roots[label] = write_self_test_fixture(
            base,
            run_id=run_ids[label],
            run_directory=f"run-{label}",
            workers=workers,
            git_commit=science_commit,
            executable_sha=producer_sha,
            validator_sha=validator_sha,
            manifest_sha=sha256_file(manifest_paths[label]),
        )
    builder_source = repo / "presentation/build_hcc1395_validated_report.py"
    builder_source.parent.mkdir(parents=True, exist_ok=True)
    builder_source.write_bytes(Path(__file__).resolve(strict=True).read_bytes())
    git_output(repo, "add", "presentation/build_hcc1395_validated_report.py")
    git_output(repo, "commit", "-q", "-m", "synthetic audit and report source")
    audit_report_commit = git_output(repo, "rev-parse", "HEAD")

    build_receipt = {
        "schema_name": "longlineage.hcc1395_executable_build_receipt",
        "schema_version": "1.0.0",
        "git_commit": science_commit,
        "source_worktree_clean": True,
        "production_claim_allowed": False,
        "executables": {
            "producer": absolute_binding(producer_binary),
            "validator": absolute_binding(validator_binary),
            "governance": absolute_binding(governance_binary),
        },
        "release_build": {
            "build_type": "Release",
            "warnings_as_errors": True,
            "require_exact_htslib": True,
            "htslib_version": "1.18",
        },
        "ctest": {"total": 42, "passed": 42, "failed": 0, "exit_code": 0},
    }
    build_receipt_path = base / "build_receipt.json"
    write_json_document(build_receipt_path, build_receipt)

    bundles = {
        label: load_verified_bundle(repo, run_roots[label]) for label in ("w24", "w40")
    }
    summary_projection = {
        key: bundles["w24"]["summary"][key]
        for key in EXPECTED_SUMMARY_PROJECTION_FIELDS
    }
    audit_artifacts = []
    for artifact_id in EXPECTED_ARTIFACT_IDS:
        row = bundles["w24"]["artifact_records"][artifact_id]
        audit_artifacts.append(
            {
                "artifact_id": artifact_id,
                "schema_name": row["schema_name"],
                "schema_version": row["schema_version"],
                "w24_logical_rows": row["logical_rows"],
                "w40_logical_rows": row["logical_rows"],
                "semantic_sha256": row["semantic_sha256"],
                "semantic_equal": True,
                "physical_equal": True,
                "physical_comparison": "DIAGNOSTIC_ONLY",
            }
        )
    audit_binary = executable_dir / "longlineage-audit"
    audit_binary.write_bytes(b"synthetic independent C++ audit executable\n")
    historical_source_path = base / "synthetic_historical_m1.tsv"
    historical_source_path.write_text(
        "HCC1395\tchrSYN\t1\tA\tC\tevaluable\t1\n", encoding="utf-8"
    )
    m2_replay = {
        "m1_site_rows": EXPECTED_SITE_KEYS,
        "m1_stable_assignments": 2,
        "cooccurrence_site_rows": EXPECTED_SITE_KEYS,
        "unstable_not_run": EXPECTED_SITE_KEYS - 2,
        "m2_eligible": 1,
        "m2_evaluable_ineligible": 0,
        "m2_axis_indeterminate": 1,
        "m2_group_count_gt10": 0,
        "partition_total": 2,
    }
    cooccurrence_replay = {
        "pair_rows": 1,
        "exact_identifiable_pairs": 1,
        "ineligible_m2_screen_pairs": 0,
        "eligible_endpoint_a_not_testable_pairs": 0,
        "eligible_exact_not_identifiable_pairs": 0,
        "eligible_exact_family_pairs": 1,
        "family_partition_total": 1,
        "fdr_family_size": 1,
        "global_bh_discoveries": 1,
        "global_by_discoveries": 1,
        "formal_pair_by_confirmed": 1,
        "cooccurrence_site_rows": EXPECTED_SITE_KEYS,
        "partner_universe_pair_rows": 1,
        "joint_signature_pass_sites": 1,
        "joint_signature_not_testable_sites": EXPECTED_SITE_KEYS - 1,
        "joint_signature_partition_total": EXPECTED_SITE_KEYS,
        "topology_units": 1,
    }
    status_names = [
        "EVALUABLE",
        "INSUFFICIENT_ALT_READS",
        "INCOMPLETE_DISTANCE_BELOW_MINIMUM",
    ]
    status_counts = {
        "EVALUABLE": 78_629,
        "INSUFFICIENT_ALT_READS": 1_044,
        "INCOMPLETE_DISTANCE_BELOW_MINIMUM": 14,
    }
    status_transitions = [
        {
            "from": source_status,
            "to": target_status,
            "count": status_counts[source_status] if source_status == target_status else 0,
        }
        for source_status in status_names
        for target_status in status_names
    ]
    key_sha = hashlib.sha256(b"synthetic ordered keys").hexdigest()
    audit_receipt = {
        "schema_name": "longlineage.hcc1395_determinism_historical_receipt",
        "schema_version": "2.0.0",
        "overall_status": "PASS",
        "production_claim_allowed": False,
        "generator": {
            "language": "C++17",
            "executable_sha256": sha256_file(audit_binary),
            "git_commit": audit_report_commit,
            "independent_of_producer_kernels": True,
            "reads_alignment_inputs": False,
        },
        "runs": {
            label: {
                "run_id": run_ids[label],
                "compute_workers": workers,
                "run_root": str(run_roots[label].resolve()),
                "manifest_path": str(manifest_paths[label].resolve()),
                "manifest_sha256": sha256_file(manifest_paths[label]),
                "run_receipt_sha256": bundles[label]["observed_sha256"]["run_receipt"],
                "validation_receipt_sha256": bundles[label]["observed_sha256"][
                    "validation_receipt"
                ],
            }
            for label, workers in (("w24", 24), ("w40", 40))
        },
        "determinism": {
            "status": "PASS",
            "manifest_comparison": {
                "status": "PASS",
                "allowed_differences": [
                    "run_id",
                    "output_root",
                    "runtime.compute_workers",
                ],
                "normalized_sha256": hashlib.sha256(b"normalized-manifest").hexdigest(),
                "w24_compute_workers": 24,
                "w40_compute_workers": 40,
            },
            "artifacts": audit_artifacts,
            "summary_projection": {
                "status": "PASS",
                "semantic_equal": True,
                "semantic_sha256": canonical_sha256(summary_projection),
                "canonicalization": (
                    "COMPACT_SORTED_JSON_NO_LF over {counts,phase_status,scope}"
                ),
                "scope": summary_projection["scope"],
                "counts": summary_projection["counts"],
                "phase_status": summary_projection["phase_status"],
            },
            "m2_conservation": {
                "status": "PASS",
                "w24": m2_replay,
                "w40": m2_replay,
            },
            "cooccurrence_replay": {
                "status": "PASS",
                "authority": "VALIDATED_SERIALIZED_CENSUS_NOT_PQ_RECOMPUTATION",
                "interpretation": "PAIR_ROWS_ARE_RECORDS_NOT_POSITIVE_DISCOVERIES",
                "w24": cooccurrence_replay,
                "w40": cooccurrence_replay,
            },
        },
        "historical": {
            "source": {
                "path": str(historical_source_path.resolve()),
                "physical_sha256": sha256_file(historical_source_path),
                "authority_profile": "SYNTHETIC_TEST_ONLY",
                "selected_dataset": EXPECTED_DATASET_ID,
                "selected_columns": [
                    "dataset",
                    "chrom",
                    "pos",
                    "ref",
                    "alt",
                    "analysis_status",
                    "stable_null_multigroup",
                ],
                "truth_derived_fields_consumed": 0,
            },
            "site_keys": {
                "status": "PASS",
                "verdict": "EXACT",
                "canonical_rule": (
                    "UTF-8 dataset\\tchrom\\tpos\\tref\\talt\\n; "
                    "no header; LF; original row order"
                ),
                "old_count": EXPECTED_SITE_KEYS,
                "new_count": EXPECTED_SITE_KEYS,
                "old_ordered_sha256": key_sha,
                "new_ordered_sha256": key_sha,
                "old_sorted_set_sha256": key_sha,
                "new_sorted_set_sha256": key_sha,
                "missing": 0,
                "extra": 0,
                "old_duplicates": 0,
                "new_duplicates": 0,
            },
            "m1": {
                "status": "PASS",
                "verdict": "COMPARABLE_DIFFERENT",
                "old_counts": {
                    "evaluable": 78_629,
                    "insufficient_alt_reads": 1_044,
                    "incomplete_distance_below_minimum": 14,
                    "stable": 12_838,
                },
                "new_counts": {
                    "evaluable": 78_629,
                    "insufficient_alt_reads": 1_044,
                    "incomplete_distance_below_minimum": 14,
                    "stable": 2,
                },
                "status_mismatches": 0,
                "status_transitions": status_transitions,
                "stable_transition": {
                    "true_to_true": 1,
                    "true_to_false": 12_837,
                    "false_to_true": 1,
                    "false_to_false": 66_848,
                    "symmetric_difference": 12_838,
                    "jaccard": {"intersection": 1, "union": 12_839},
                },
            },
            "m2": {
                "status": "PASS",
                "verdict": "NOT_COMPARABLE_METHOD_CHANGED",
                "reason_code": "HISTORICAL_SCREENING_AGGREGATE_ONLY",
                "explanation": (
                    "Synthetic /" + "big8_disk" + "/private M2 context at "
                    + "chr" + "1" + ":" + "12345."
                ),
            },
            "cooccurrence": {
                "status": "PASS",
                "verdict": "NOT_COMPARABLE_NO_FORMAL_OLD_RESULT",
                "reason_code": "NO_SUCCESSFUL_FORMAL_HISTORICAL_AUTHORITY",
                "explanation": (
                    "Synthetic /" + "home" + "/alice/private formal result absent."
                ),
                "old_formal_result_exists": False,
                "new_pair_rows": 1,
            },
            "regional_topology": {
                "status": "PASS",
                "verdict": "NOT_COMPARABLE_METHOD_AND_GATE_CHANGED",
                "reason_code": (
                    "HISTORICAL_TOPOLOGY_USED_DIFFERENT_METHOD_CN_LOH_BOUNDARY"
                ),
                "explanation": (
                    "Synthetic /" + "Users" + "/alice/private region "
                    + "chr" + "22" + ":" + "987."
                ),
            },
            "runtime": {
                "status": "PASS",
                "verdict": "NOT_COMPARABLE_PROGRAM_SCOPE_THREAD_OUTPUT_CHANGED",
                "reason_code": "NO_MATCHED_RUNTIME_DENOMINATOR",
                "explanation": (
                    "Synthetic /" + "big7_disk"
                    + "/private runtime is not comparable."
                ),
            },
        },
        "checks": [
            {
                "check_id": check_id,
                "status": "PASS",
                "evidence_sha256": hashlib.sha256(check_id.encode()).hexdigest(),
            }
            for check_id in [
                "FROZEN_W24",
                "FROZEN_W40",
                "MANIFEST_WHITELIST",
                "SCIENCE_ARTIFACT_DETERMINISM",
                "SUMMARY_PROJECTION",
                "M2_MUTUALLY_EXCLUSIVE_CONSERVATION",
                "COOCCURRENCE_SERIALIZED_CENSUS_CONSERVATION",
                "HISTORICAL_SITE_KEY_M1",
            ]
        ],
    }
    audit_receipt_path = base / "audit_receipt.json"
    write_json_document(audit_receipt_path, audit_receipt)

    execution_runs = {}
    for label, workers in (("w24", 24), ("w40", 40)):
        execution_runs[label] = {
            "label": label,
            "compute_workers": workers,
            "run_root": str(run_roots[label].resolve()),
            "manifest": absolute_binding(manifest_paths[label]),
            "producer_execution": write_gnu_time_fixture(
                logs,
                f"{label}.producer",
                "READY_FOR_VALIDATION",
                100.0 + workers,
                200.0,
                10.0,
                1024 + workers,
            ),
            "validator_execution": write_gnu_time_fixture(
                logs,
                f"{label}.validator",
                EXPECTED_TERMINAL_STATE,
                40.0 + workers,
                30.0,
                5.0,
                2048 + workers,
            ),
        }
    ctest_log_path = logs / "audit.ctest.log"
    ctest_log_path.write_text(
        "100% tests passed, 0 tests failed out of 44\n"
        "Total Test time (real) = 1.00 sec\n",
        encoding="utf-8",
    )
    execution_evidence = {
        "schema_name": "longlineage.hcc1395_execution_evidence",
        "schema_version": "2.0.0",
        "evidence_id": "synthetic-hcc1395-dual-run",
        "created_at": "2026-07-20T00:20:00Z",
        "dataset_id": EXPECTED_DATASET_ID,
        "report_scope": EXPECTED_REPORT_SCOPE,
        "production_claim_allowed": False,
        "truth_fields": 0,
        "tagged_bam_persisted": False,
        "latest_tag_join": "EXACT_PROJECTION_NO_FALLBACK",
        "software_provenance": {
            "science_execution": {
                "repository": {
                    "path": str(repo.resolve()),
                    "git_commit": science_commit,
                },
                "executable_build_receipt": absolute_binding(build_receipt_path),
            },
            "audit_report_generation": {
                "repository": {
                    "path": str(repo.resolve()),
                    "git_commit": audit_report_commit,
                },
                "source_worktree_clean": True,
                "audit_executable": absolute_binding(audit_binary),
                "report_builder_source": absolute_binding(builder_source),
                "release_build": {
                    "build_type": "Release",
                    "warnings_as_errors": True,
                    "require_exact_htslib": True,
                    "htslib_version": "1.18",
                },
                "ctest": {
                    "total": 44,
                    "passed": 44,
                    "failed": 0,
                    "exit_code": 0,
                },
                "ctest_log": absolute_binding(ctest_log_path),
            },
        },
        "runs": execution_runs,
        "authorities": {
            "dataset_gate_input": absolute_binding(dataset_authority_path),
            "production_input": absolute_binding(production_authority_path),
        },
        "audit_receipt": absolute_binding(audit_receipt_path),
        "determinism_contract": {
            "artifact_ids": EXPECTED_ARTIFACT_IDS,
            "manifest_difference_pointers": EXPECTED_MANIFEST_DIFFERENCES,
            "summary_projection_fields": EXPECTED_SUMMARY_PROJECTION_FIELDS,
        },
    }
    evidence_path = base / "execution_evidence.json"
    write_json_document(evidence_path, execution_evidence)
    return {
        "repo": repo,
        "w24": run_roots["w24"],
        "w40": run_roots["w40"],
        "evidence": evidence_path,
        "audit": audit_receipt_path,
        "build": build_receipt_path,
        "dataset_authority": dataset_authority_path,
        "production_authority": production_authority_path,
        "builder_source": builder_source,
        "ctest_log": ctest_log_path,
    }


def mutate_json_document(path: Path, mutation: Any) -> None:
    value = read_json(path, f"self-test mutation source {path.name}")
    mutation(value)
    write_json_document(path, value)


def refresh_evidence_binding(paths: Mapping[str, Path], field: str) -> None:
    evidence = read_json(paths["evidence"], "self-test execution evidence")
    if field == "audit_receipt":
        evidence["audit_receipt"] = absolute_binding(paths["audit"])
    elif field == "executable_build_receipt":
        evidence["software_provenance"]["science_execution"][
            "executable_build_receipt"
        ] = absolute_binding(paths["build"])
    else:
        raise EvidenceError(f"unsupported self-test binding refresh: {field}")
    write_json_document(paths["evidence"], evidence)


def require_outputs_absent(output_html: Path, output_json: Path) -> None:
    require(output_html.resolve() != output_json.resolve(),
            "HTML and JSON outputs must be different paths")
    require(
        not output_html.exists() and not output_html.is_symlink(),
        f"refusing to overwrite existing HTML output: {output_html}",
    )
    require(
        not output_json.exists() and not output_json.is_symlink(),
        f"refusing to overwrite existing JSON output: {output_json}",
    )


def run_self_test() -> int:
    negative_results: List[Dict[str, str]] = []
    with tempfile.TemporaryDirectory(prefix="longlineage_hcc_report_positive_") as temporary:
        paths = create_dual_self_test_fixture(Path(temporary))
        chain = validate_dual_evidence_chain(
            paths["repo"],
            {"w24": paths["w24"], "w40": paths["w40"]},
            paths["evidence"],
            paths["audit"],
            paths["builder_source"],
        )
        model = build_dual_report_model(chain)
        rendered = render_html(model)
        require(model["schema_version"] == "2.0.0",
                "positive self-test report schema is not 2.0.0")
        require(
            "phase_status_at_producer_closeout" not in model
            and model["producer_run_local_phase_status"]["scope"]
            == "RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER",
            "positive self-test phase status is not explicitly run-local",
        )
        require("<!doctype html>" in rendered and 'data-partial="true"' in rendered,
                "positive self-test HTML markers missing")
        require("8 / 8 PASS" in rendered, "positive self-test determinism marker missing")
        require("raw BAM + external latest HP/PS sidecar" in rendered,
                "positive self-test input boundary missing")
        require(
            rendered.count('class="table-wrap" tabindex="0"') == 7
            and "width:100%; max-width:100%; min-width:0" in rendered
            and "overflow-wrap:anywhere" in rendered,
            "positive self-test responsive keyboard-scroll table contract missing",
        )
        require(
            "background-color:#96322d" in rendered,
            "positive self-test ribbon contrast fallback missing",
        )
        tiny_bar = bar_row("tiny", 14, 79_687, "blue")
        require(
            'style="width:0.2000%"' in tiny_bar
            and "<small>0.02% of 79,687</small>" in tiny_bar,
            "positive self-test conflated minimum visual width with displayed percent",
        )
        require(
            model["cooccurrence_replay"]["counters"]["pair_rows"] == 1
            and model["cooccurrence_replay"]["counters"][
                "eligible_exact_family_pairs"
            ] == 1
            and model["cooccurrence_replay"]["counters"][
                "global_by_discoveries"
            ] == 1
            and model["cooccurrence_replay"]["counters"][
                "formal_pair_by_confirmed"
            ] == 1,
            "positive self-test co-occurrence counters missing",
        )
        require(
            model["counts"]["topology_primary_hp_units"] == 1
            and model["counts"]["topology_regions"] == 2
            and model["counts"]["topology_fully_complete_regions"]
            + model["counts"]["topology_incomplete_regions"]
            == model["counts"]["topology_regions"],
            "positive self-test did not preserve distinct topology unit/region grains",
        )
        require(
            "Pair rows ≠ positive discoveries" in rendered
            and "BY discoveries 是 BH discoveries 的子集（可相等）" in rendered
            and "BY 是 BH 的嚴格子集" not in rendered
            and "Python 不讀 scientific artifact data rows" in rendered
            and (
                "允許讀 artifact catalog/semantic digest metadata rows 與 "
                "summary/receipts"
            )
            in rendered
            and "舊正式流程在發布前失敗" in rendered
            and "formal comparable authority" in rendered
            and "attempt 6" not in rendered
            and "251,517,047" not in rendered,
            "positive self-test co-occurrence interpretation boundary missing",
        )
        rendered_bytes = rendered.encode("utf-8")
        model_bytes = (
            json.dumps(model, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8")
        assert_tracked_payload_safe(
            model,
            rendered_bytes,
            model_bytes,
            collect_known_external_paths(chain),
        )
        html_sha = hashlib.sha256(rendered_bytes).hexdigest()
        source_tree = ast.parse(Path(__file__).read_text(encoding="utf-8"))
        imported_modules = {
            alias.name.split(".", 1)[0]
            for node in ast.walk(source_tree)
            if isinstance(node, (ast.Import, ast.ImportFrom))
            for alias in (
                node.names
                if isinstance(node, ast.Import)
                else [ast.alias(name=node.module or "")]
            )
        }
        forbidden_science_modules = {
            "pysam",
            "cyvcf2",
            "vcf",
            "pandas",
            "numpy",
            "scipy",
            "statsmodels",
        }
        require(
            imported_modules.isdisjoint(forbidden_science_modules),
            "presentation builder imports a forbidden science/data module",
        )

    def execute_negative(name: str, mutation: Any) -> None:
        with tempfile.TemporaryDirectory(
            prefix=f"longlineage_hcc_report_negative_{name}_"
        ) as temporary:
            paths = create_dual_self_test_fixture(Path(temporary))
            output_html = Path(temporary) / "negative-output.html"
            output_json = Path(temporary) / "negative-output.json"
            mutation(paths, output_html, output_json)
            rejected = False
            error_text = ""
            try:
                require_outputs_absent(output_html, output_json)
                chain = validate_dual_evidence_chain(
                    paths["repo"],
                    {"w24": paths["w24"], "w40": paths["w40"]},
                    paths["evidence"],
                    paths["audit"],
                    paths["builder_source"],
                )
                build_dual_report_model(chain)
            except EvidenceError as error:
                rejected = True
                error_text = str(error)
            require(rejected, f"negative self-test did not fail closed: {name}")
            if name == "output_overwrite":
                require(
                    output_html.read_text(encoding="utf-8") == "pre-existing"
                    and not output_json.exists(),
                    "output-overwrite negative changed the pre-existing sentinel",
                )
            else:
                require(
                    not output_html.exists() and not output_json.exists(),
                    f"negative self-test left a PASS artifact: {name}",
                )
            negative_results.append(
                {"case": name, "status": "PASS", "observed": error_text[:180]}
            )

    def mutate_evidence(paths: Mapping[str, Path], callback: Any) -> None:
        mutate_json_document(paths["evidence"], callback)

    def mutate_audit(paths: Mapping[str, Path], callback: Any) -> None:
        mutate_json_document(paths["audit"], callback)
        refresh_evidence_binding(paths, "audit_receipt")

    def mutate_build(paths: Mapping[str, Path], callback: Any) -> None:
        mutate_json_document(paths["build"], callback)
        refresh_evidence_binding(paths, "executable_build_receipt")

    negative_cases = [
        (
            "execution_evidence_schema_downgrade",
            lambda p, _h, _j: mutate_evidence(
                p, lambda d: d.update({"schema_version": "1.0.0"})
            ),
        ),
        (
            "legacy_single_repo_provenance",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: (
                    d.pop("software_provenance"),
                    d.update(
                        {
                            "repo": {
                                "path": str(p["repo"].resolve()),
                                "git_commit": git_output(
                                    p["repo"], "rev-parse", "HEAD"
                                ),
                            }
                        }
                    ),
                ),
            ),
        ),
        (
            "science_commit_drift",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: d["software_provenance"]["science_execution"][
                    "repository"
                ].update({"git_commit": "2" * 40}),
            ),
        ),
        (
            "audit_report_commit_drift",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: d["software_provenance"]["audit_report_generation"][
                    "repository"
                ].update({"git_commit": "3" * 40}),
            ),
        ),
        (
            "audit_generator_commit_drift",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["generator"].update({"git_commit": "4" * 40}),
            ),
        ),
        (
            "audit_executable_physical_drift",
            lambda p, _h, _j: (
                Path(
                    read_json(p["evidence"], "self-test evidence")[
                        "software_provenance"
                    ]["audit_report_generation"]["audit_executable"]["path"]
                ).write_bytes(b"mutated audit executable\n")
            ),
        ),
        (
            "report_builder_source_drift",
            lambda p, _h, _j: p["builder_source"].write_bytes(
                p["builder_source"].read_bytes() + b"\n# drift\n"
            ),
        ),
        (
            "audit_source_worktree_dirty",
            lambda p, _h, _j: (p["repo"] / "untracked.txt").write_text(
                "dirty\n", encoding="utf-8"
            ),
        ),
        (
            "software_provenance_unknown_field",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: d["software_provenance"].update(
                    {"unexpected": "must reject"}
                ),
            ),
        ),
        (
            "missing_audit_executable_binding",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: d["software_provenance"]["audit_report_generation"].pop(
                    "audit_executable"
                ),
            ),
        ),
        (
            "audit_ctest_log_physical_drift",
            lambda p, _h, _j: p["ctest_log"].write_text(
                "50% tests passed, 22 tests failed out of 44\n",
                encoding="utf-8",
            ),
        ),
        (
            "missing_w40",
            lambda p, _h, _j: mutate_evidence(p, lambda d: d["runs"].pop("w40")),
        ),
        (
            "wrong_w24_worker_count",
            lambda p, _h, _j: mutate_evidence(
                p, lambda d: d["runs"]["w24"].update({"compute_workers": 23})
            ),
        ),
        (
            "swapped_worker_counts",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: (
                    d["runs"]["w24"].update({"compute_workers": 40}),
                    d["runs"]["w40"].update({"compute_workers": 24}),
                ),
            ),
        ),
        (
            "truth_field_in_execution_evidence",
            lambda p, _h, _j: mutate_evidence(p, lambda d: d.update({"truth_fields": 1})),
        ),
        (
            "persisted_tagged_bam_claim",
            lambda p, _h, _j: mutate_evidence(
                p, lambda d: d.update({"tagged_bam_persisted": True})
            ),
        ),
        (
            "sidecar_fallback_permitted",
            lambda p, _h, _j: mutate_evidence(
                p, lambda d: d.update({"latest_tag_join": "ALLOW_BAM_HP_FALLBACK"})
            ),
        ),
        (
            "production_claim_enabled",
            lambda p, _h, _j: mutate_evidence(
                p, lambda d: d.update({"production_claim_allowed": True})
            ),
        ),
        (
            "unknown_execution_field",
            lambda p, _h, _j: mutate_evidence(
                p, lambda d: d.update({"unexpected": "must reject"})
            ),
        ),
        (
            "artifact_contract_missing_member",
            lambda p, _h, _j: mutate_evidence(
                p, lambda d: d["determinism_contract"]["artifact_ids"].pop()
            ),
        ),
        (
            "manifest_allowlist_extra_pointer",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: d["determinism_contract"][
                    "manifest_difference_pointers"
                ].append("/runtime/writer_threads"),
            ),
        ),
        (
            "summary_projection_contract_drift",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: d["determinism_contract"][
                    "summary_projection_fields"
                ].remove("counts"),
            ),
        ),
        (
            "actual_w40_manifest_mutation",
            lambda p, _h, _j: mutate_json_document(
                Path(
                    read_json(p["evidence"], "self-test evidence")["runs"]["w40"][
                        "manifest"
                    ]["path"]
                ),
                lambda d: d["runtime"].update({"writer_threads": 5}),
            ),
        ),
        (
            "non_frozen_w24",
            lambda p, _h, _j: mutate_json_document(
                p["w24"] / "run_receipt.json",
                lambda d: d.update({"state": "VALIDATED"}),
            ),
        ),
        (
            "run_truth_leak",
            lambda p, _h, _j: mutate_json_document(
                p["w24"] / "run_receipt.json",
                lambda d: d.update({"truth_fields_seen": 1}),
            ),
        ),
        (
            "cooccurrence_schema_downgrade",
            lambda p, _h, _j: mutate_json_document(
                p["w24"] / "run_receipt.json",
                lambda d: next(
                    row for row in d["artifacts"]
                    if row["artifact_id"] == "cooccurrence_pairs"
                ).update({"schema_version": "1.0.0"}),
            ),
        ),
        (
            "frozen_artifact_physical_mutation",
            lambda p, _h, _j: (p["w40"] / "summary.json").write_bytes(
                (p["w40"] / "summary.json").read_bytes() + b" "
            ),
        ),
        (
            "audit_overall_fail",
            lambda p, _h, _j: mutate_audit(
                p, lambda d: d.update({"overall_status": "FAIL"})
            ),
        ),
        (
            "audit_unknown_field",
            lambda p, _h, _j: mutate_audit(
                p, lambda d: d.update({"unexpected": "must reject"})
            ),
        ),
        (
            "audit_receipt_v1_rejected",
            lambda p, _h, _j: mutate_audit(
                p, lambda d: d.update({"schema_version": "1.0.0"})
            ),
        ),
        (
            "audit_check_order_swapped",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["checks"].__setitem__(
                    slice(0, 2), list(reversed(d["checks"][:2]))
                ),
            ),
        ),
        (
            "audit_artifact_semantic_mismatch",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["determinism"]["artifacts"][0].update(
                    {"semantic_equal": False}
                ),
            ),
        ),
        (
            "audit_summary_projection_mismatch",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["determinism"]["summary_projection"].update(
                    {"semantic_equal": False}
                ),
            ),
        ),
        (
            "audit_m2_conservation_fail",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["determinism"]["m2_conservation"].update({"w40": False}),
            ),
        ),
        (
            "audit_cooccurrence_replay_missing",
            lambda p, _h, _j: mutate_audit(
                p, lambda d: d["determinism"].pop("cooccurrence_replay")
            ),
        ),
        (
            "audit_cooccurrence_worker_mismatch",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["determinism"]["cooccurrence_replay"]["w40"].update(
                    {"formal_pair_by_confirmed": 0}
                ),
            ),
        ),
        (
            "audit_cooccurrence_hierarchy_fail",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["determinism"]["cooccurrence_replay"]["w24"].update(
                    {"formal_pair_by_confirmed": 2}
                ),
            ),
        ),
        (
            "audit_cooccurrence_pair_binding_mismatch",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: (
                    d["determinism"]["cooccurrence_replay"]["w24"].update(
                        {
                            "pair_rows": 2,
                            "ineligible_m2_screen_pairs": 1,
                            "family_partition_total": 2,
                            "partner_universe_pair_rows": 2,
                        }
                    ),
                    d["determinism"]["cooccurrence_replay"]["w40"].update(
                        {
                            "pair_rows": 2,
                            "ineligible_m2_screen_pairs": 1,
                            "family_partition_total": 2,
                            "partner_universe_pair_rows": 2,
                        }
                    ),
                    d["historical"]["cooccurrence"].update({"new_pair_rows": 2}),
                ),
            ),
        ),
        (
            "audit_cooccurrence_unknown_field",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["determinism"]["cooccurrence_replay"]["w24"].update(
                    {"unexpected": 0}
                ),
            ),
        ),
        (
            "historical_site_keys_not_exact",
            lambda p, _h, _j: mutate_audit(
                p, lambda d: d["historical"]["site_keys"].update({"missing": 1})
            ),
        ),
        (
            "historical_m1_false_parity",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["historical"]["m1"].update({"verdict": "EXACT"}),
            ),
        ),
        (
            "historical_cooccurrence_false_comparison",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["historical"]["cooccurrence"].update(
                    {"verdict": "EXACT_COMPARABLE"}
                ),
            ),
        ),
        (
            "historical_topology_false_comparison",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["historical"]["regional_topology"].update(
                    {"verdict": "EXACT_COMPARABLE"}
                ),
            ),
        ),
        (
            "historical_runtime_false_speedup",
            lambda p, _h, _j: mutate_audit(
                p,
                lambda d: d["historical"]["runtime"].update(
                    {"verdict": "COMPARABLE_SPEEDUP"}
                ),
            ),
        ),
        (
            "build_commit_drift",
            lambda p, _h, _j: mutate_build(
                p, lambda d: d.update({"git_commit": "2" * 40})
            ),
        ),
        (
            "timing_status_marker_missing",
            lambda p, _h, _j: mutate_evidence(
                p,
                lambda d: d["runs"]["w24"]["producer_execution"].update(
                    {"status_marker": "NOT_PRESENT"}
                ),
            ),
        ),
        (
            "timing_log_physical_drift",
            lambda p, _h, _j: Path(
                read_json(p["evidence"], "self-test evidence")["runs"]["w40"][
                    "validator_execution"
                ]["time_log"]["path"]
            ).write_text("Exit status: 7\n", encoding="utf-8"),
        ),
        (
            "dataset_authority_physical_drift",
            lambda p, _h, _j: mutate_json_document(
                p["dataset_authority"],
                lambda d: d.update({"tagged_bam_persisted": True}),
            ),
        ),
        (
            "output_overwrite",
            lambda _p, h, _j: h.write_text("pre-existing", encoding="utf-8"),
        ),
    ]
    for name, mutation in negative_cases:
        execute_negative(name, mutation)

    result = {
        "schema_name": "longlineage.hcc1395_validated_report_self_test",
        "schema_version": "2.0.0",
        "pass": True,
        "positive_fixture": "PASS",
        "negative_case_count": len(negative_results),
        "negative_cases": negative_results,
        "html_bytes": len(rendered.encode("utf-8")),
        "html_sha256": html_sha,
    }
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, help="LongLineage repository root")
    parser.add_argument("--run-root-w24", type=Path, help="Frozen HCC1395 w24 run root")
    parser.add_argument("--run-root-w40", type=Path, help="Frozen HCC1395 w40 run root")
    parser.add_argument("--execution-evidence", type=Path)
    parser.add_argument("--audit-receipt", type=Path)
    parser.add_argument("--output-html", type=Path)
    parser.add_argument("--output-json", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()
    require(args.repo is not None, "--repo is required")
    require(args.run_root_w24 is not None, "--run-root-w24 is required")
    require(args.run_root_w40 is not None, "--run-root-w40 is required")
    require(args.execution_evidence is not None, "--execution-evidence is required")
    require(args.audit_receipt is not None, "--audit-receipt is required")
    require(args.output_html is not None, "--output-html is required")
    require(args.output_json is not None, "--output-json is required")
    require_outputs_absent(args.output_html, args.output_json)

    started = time.monotonic()
    chain = validate_dual_evidence_chain(
        args.repo,
        {"w24": args.run_root_w24, "w40": args.run_root_w40},
        args.execution_evidence,
        args.audit_receipt,
    )
    model = build_dual_report_model(chain)
    builder_path = Path(__file__).resolve(strict=True)
    model["presentation"] = {
        "schema_name": "longlineage.hcc1395_report_build_receipt",
        "schema_version": "2.0.0",
        "status": "PASS",
        "builder": "presentation/build_hcc1395_validated_report.py",
        "builder_sha256": sha256_file(builder_path),
        "science_git_commit": chain["execution"]["science_git_commit"],
        "audit_report_git_commit": chain["execution"]["audit_report_git_commit"],
        "presentation_only": True,
        "scientific_recomputation": False,
        "scientific_inputs_opened_by_python": False,
        "all_declared_artifact_physical_hashes_replayed": True,
        "execution_evidence_sha256": chain["execution"]["sha256"],
        "audit_receipt_sha256": chain["execution"]["audit_receipt"]["sha256"],
        "evidence_replay_and_model_seconds": round(time.monotonic() - started, 6),
    }
    render_started = time.monotonic()
    rendered = render_html(model).encode("utf-8")
    model["presentation"]["html_render_seconds"] = round(
        time.monotonic() - render_started, 6
    )
    model["presentation"]["total_seconds_before_write"] = round(
        time.monotonic() - started, 6
    )
    model["presentation"]["html_size_bytes"] = len(rendered)
    model["presentation"]["html_sha256"] = hashlib.sha256(rendered).hexdigest()
    json_payload = (
        json.dumps(model, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    known_external_paths = collect_known_external_paths(chain)
    known_external_paths.extend(
        [
            str(builder_path),
            str(args.output_html.resolve()),
            str(args.output_json.resolve()),
        ]
    )
    assert_tracked_payload_safe(
        model, rendered, json_payload, sorted(set(known_external_paths))
    )
    atomic_write(args.output_html.resolve(), rendered)
    atomic_write(args.output_json.resolve(), json_payload)
    print(
        json.dumps(
            {
                "status": "PASS",
                "run_ids": model["run_ids"],
                "terminal_state": model["terminal_state"],
                "determinism_artifacts": model["determinism"]["artifact_count"],
                "production_claim_allowed": False,
                "output_html": str(args.output_html.resolve()),
                "output_html_sha256": hashlib.sha256(rendered).hexdigest(),
                "output_json": str(args.output_json.resolve()),
                "output_json_sha256": hashlib.sha256(json_payload).hexdigest(),
            },
            ensure_ascii=False,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except EvidenceError as error:
        raise SystemExit(f"FAIL_CLOSED: {error}") from error
