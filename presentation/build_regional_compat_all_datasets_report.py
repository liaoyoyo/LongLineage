#!/usr/bin/env python3
"""Render the validated seven-dataset regional cohort as a portable report artifact.

This program is intentionally presentation-only. It reads the C++ cohort receipt,
wrapper timing logs and a frozen historical Python lifecycle. It does not open BAM,
VCF or sidecar inputs and does not recompute scientific classifications.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


TITLE = "七樣本 Python 相容 sSNV 區域拓撲：C++ indexed-I/O 全量驗證"
PROFILE = "PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET"
DATASETS = [
    "HCC1395",
    "HCC1395_DORADO",
    "COLO829",
    "H1437",
    "H2009",
    "HCC1937",
    "HCC1954",
]
UNIT_CLASSES = [
    "ambiguous_order",
    "ambiguous_structure",
    "capped",
    "determined",
    "recurrence_required",
    "underdetermined",
]
REGION_CLASSES = [
    "all_determined",
    "has_ambiguous",
    "has_capped",
    "has_recurrence",
    "no_primary_lineage",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--cohort-receipt", type=Path, required=True)
    parser.add_argument("--batch-log", type=Path, required=True)
    parser.add_argument("--crosswalk-log", type=Path, required=True)
    parser.add_argument("--input-hash-root", type=Path, required=True)
    parser.add_argument("--historical-python-root", type=Path, required=True)
    parser.add_argument("--generated-at", required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


class JsonSchemaValidationError(ValueError):
    """The instance does not satisfy the locally supported JSON Schema."""


class JsonSchemaDefinitionError(ValueError):
    """The pinned schema is malformed or uses an unsupported keyword."""


SUPPORTED_SCHEMA_KEYWORDS = {
    "$schema",
    "$id",
    "title",
    "$defs",
    "$ref",
    "type",
    "required",
    "properties",
    "additionalProperties",
    "const",
    "pattern",
    "minLength",
    "minItems",
    "maxItems",
    "prefixItems",
    "items",
    "allOf",
    "oneOf",
    "minimum",
    "maximum",
    "exclusiveMinimum",
}
JSON_TYPES = {"object", "array", "string", "integer", "number", "boolean", "null"}


def reject_json_constant(value: str) -> None:
    raise ValueError(f"non-standard JSON numeric constant is forbidden: {value}")


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for key, value in pairs:
        require(key not in output, f"duplicate JSON object key: {key}")
        output[key] = value
    return output


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=unique_object,
        parse_constant=reject_json_constant,
    )
    require(isinstance(value, dict), f"expected JSON object: {path}")
    return value


def schema_definition_require(condition: bool, message: str) -> None:
    if not condition:
        raise JsonSchemaDefinitionError(message)


def audit_schema_definition(schema: Any, path: str = "#") -> None:
    if isinstance(schema, bool):
        return
    schema_definition_require(isinstance(schema, dict), f"{path}: schema node must be an object or boolean")
    unknown = sorted(set(schema) - SUPPORTED_SCHEMA_KEYWORDS)
    schema_definition_require(not unknown, f"{path}: unsupported JSON Schema keywords: {unknown}")

    if "$schema" in schema:
        schema_definition_require(isinstance(schema["$schema"], str), f"{path}.$schema must be a string")
    if "$id" in schema:
        schema_definition_require(isinstance(schema["$id"], str), f"{path}.$id must be a string")
    if "title" in schema:
        schema_definition_require(isinstance(schema["title"], str), f"{path}.title must be a string")
    if "$ref" in schema:
        schema_definition_require(
            isinstance(schema["$ref"], str) and schema["$ref"].startswith("#/"),
            f"{path}.$ref must be a nonempty local JSON Pointer",
        )
    if "type" in schema:
        declared = schema["type"]
        if isinstance(declared, str):
            declared_types = [declared]
        else:
            schema_definition_require(
                isinstance(declared, list) and declared and
                all(isinstance(item, str) for item in declared) and
                len(set(declared)) == len(declared),
                f"{path}.type must be a string or a unique nonempty string array",
            )
            declared_types = declared
        schema_definition_require(
            all(item in JSON_TYPES for item in declared_types), f"{path}.type contains an unknown JSON type"
        )

    if "required" in schema:
        required = schema["required"]
        schema_definition_require(
            isinstance(required, list) and all(isinstance(item, str) for item in required) and
            len(set(required)) == len(required),
            f"{path}.required must be a unique string array",
        )

    for keyword in ("minLength", "minItems", "maxItems"):
        if keyword in schema:
            value = schema[keyword]
            schema_definition_require(
                isinstance(value, int) and not isinstance(value, bool) and value >= 0,
                f"{path}.{keyword} must be a nonnegative integer",
            )
    if "minItems" in schema and "maxItems" in schema:
        schema_definition_require(schema["minItems"] <= schema["maxItems"],
                                  f"{path}.minItems must not exceed maxItems")
    for keyword in ("minimum", "maximum", "exclusiveMinimum"):
        if keyword in schema:
            value = schema[keyword]
            schema_definition_require(
                isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value)),
                f"{path}.{keyword} must be a finite JSON number",
            )
    if "minimum" in schema and "maximum" in schema:
        schema_definition_require(schema["minimum"] <= schema["maximum"],
                                  f"{path}.minimum must not exceed maximum")
    if "pattern" in schema:
        schema_definition_require(isinstance(schema["pattern"], str), f"{path}.pattern must be a string")
        try:
            re.compile(schema["pattern"])
        except re.error as error:
            raise JsonSchemaDefinitionError(f"{path}.pattern is invalid: {error}") from error

    definitions = schema.get("$defs", {})
    schema_definition_require(isinstance(definitions, dict), f"{path}.$defs must be an object")
    for name, child in definitions.items():
        schema_definition_require(isinstance(name, str), f"{path}.$defs contains a non-string key")
        audit_schema_definition(child, f"{path}.$defs.{name}")

    properties = schema.get("properties", {})
    schema_definition_require(isinstance(properties, dict), f"{path}.properties must be an object")
    for name, child in properties.items():
        schema_definition_require(isinstance(name, str), f"{path}.properties contains a non-string key")
        audit_schema_definition(child, f"{path}.properties.{name}")

    if "additionalProperties" in schema:
        additional = schema["additionalProperties"]
        schema_definition_require(isinstance(additional, (bool, dict)),
                                  f"{path}.additionalProperties must be a schema or boolean")
        audit_schema_definition(additional, f"{path}.additionalProperties")
    if "items" in schema:
        items = schema["items"]
        schema_definition_require(isinstance(items, (bool, dict)), f"{path}.items must be a schema or boolean")
        audit_schema_definition(items, f"{path}.items")
    if "prefixItems" in schema:
        prefix = schema["prefixItems"]
        schema_definition_require(isinstance(prefix, list), f"{path}.prefixItems must be an array")
        for index, child in enumerate(prefix):
            audit_schema_definition(child, f"{path}.prefixItems[{index}]")
    for keyword in ("allOf", "oneOf"):
        if keyword in schema:
            branches = schema[keyword]
            schema_definition_require(isinstance(branches, list) and branches,
                                      f"{path}.{keyword} must be a nonempty schema array")
            for index, child in enumerate(branches):
                audit_schema_definition(child, f"{path}.{keyword}[{index}]")


def resolve_local_ref(root_schema: dict[str, Any], reference: str) -> Any:
    schema_definition_require(reference.startswith("#/"), f"external or empty $ref is forbidden: {reference}")
    current: Any = root_schema
    for encoded in reference[2:].split("/"):
        token = encoded.replace("~1", "/").replace("~0", "~")
        schema_definition_require(isinstance(current, dict) and token in current,
                                  f"unresolved local $ref: {reference}")
        current = current[token]
    return current


def json_semantic_equal(left: Any, right: Any) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return type(left) is type(right) and left == right
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return math.isfinite(float(left)) and math.isfinite(float(right)) and left == right
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return set(left) == set(right) and all(json_semantic_equal(left[key], right[key]) for key in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(json_semantic_equal(a, b) for a, b in zip(left, right))
    return left == right


def instance_has_type(instance: Any, declared: str) -> bool:
    if declared == "object":
        return isinstance(instance, dict)
    if declared == "array":
        return isinstance(instance, list)
    if declared == "string":
        return isinstance(instance, str)
    if declared == "boolean":
        return isinstance(instance, bool)
    if declared == "null":
        return instance is None
    if declared == "number":
        return (isinstance(instance, (int, float)) and not isinstance(instance, bool) and
                math.isfinite(float(instance)))
    if declared == "integer":
        return (isinstance(instance, int) and not isinstance(instance, bool)) or (
            isinstance(instance, float) and math.isfinite(instance) and instance.is_integer()
        )
    raise JsonSchemaDefinitionError(f"unsupported JSON type during evaluation: {declared}")


def validate_schema_instance(
    instance: Any,
    schema: Any,
    root_schema: dict[str, Any],
    path: str = "$",
    depth: int = 0,
) -> None:
    schema_definition_require(depth <= 256, "local JSON Schema reference depth exceeds 256")
    if schema is False:
        raise JsonSchemaValidationError(f"{path}: forbidden by false schema")
    if schema is True:
        return
    schema_definition_require(isinstance(schema, dict), f"{path}: evaluated schema must be an object or boolean")

    if "$ref" in schema:
        validate_schema_instance(instance, resolve_local_ref(root_schema, schema["$ref"]), root_schema,
                                 path, depth + 1)
    for index, branch in enumerate(schema.get("allOf", [])):
        try:
            validate_schema_instance(instance, branch, root_schema, path, depth + 1)
        except JsonSchemaValidationError as error:
            raise JsonSchemaValidationError(f"{path}: allOf[{index}] failed: {error}") from error
    if "oneOf" in schema:
        matches = 0
        branch_errors: list[str] = []
        for index, branch in enumerate(schema["oneOf"]):
            try:
                validate_schema_instance(instance, branch, root_schema, path, depth + 1)
                matches += 1
            except JsonSchemaValidationError as error:
                branch_errors.append(f"{index}: {error}")
        if matches != 1:
            detail = "; ".join(branch_errors[:2])
            raise JsonSchemaValidationError(f"{path}: oneOf matched {matches} branches (expected 1); {detail}")

    if "type" in schema:
        declared = schema["type"]
        declared_types = [declared] if isinstance(declared, str) else declared
        if not any(instance_has_type(instance, item) for item in declared_types):
            raise JsonSchemaValidationError(f"{path}: expected type {declared_types}, got {type(instance).__name__}")
    if "const" in schema and not json_semantic_equal(instance, schema["const"]):
        raise JsonSchemaValidationError(f"{path}: value differs from const")

    if isinstance(instance, dict):
        required = schema.get("required", [])
        missing = [key for key in required if key not in instance]
        if missing:
            raise JsonSchemaValidationError(f"{path}: missing required fields {missing}")
        properties = schema.get("properties", {})
        for key, child in properties.items():
            if key in instance:
                validate_schema_instance(instance[key], child, root_schema, f"{path}.{key}", depth + 1)
        if "additionalProperties" in schema:
            additional = schema["additionalProperties"]
            extras = sorted(set(instance) - set(properties))
            if additional is False and extras:
                raise JsonSchemaValidationError(f"{path}: additional properties are forbidden: {extras}")
            if isinstance(additional, dict):
                for key in extras:
                    validate_schema_instance(instance[key], additional, root_schema, f"{path}.{key}", depth + 1)

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            raise JsonSchemaValidationError(f"{path}: item count is below minItems")
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            raise JsonSchemaValidationError(f"{path}: item count exceeds maxItems")
        prefix = schema.get("prefixItems", [])
        for index, child in enumerate(prefix[:len(instance)]):
            validate_schema_instance(instance[index], child, root_schema, f"{path}[{index}]", depth + 1)
        if "items" in schema:
            items = schema["items"]
            trailing = range(len(prefix), len(instance))
            if items is False and len(instance) > len(prefix):
                raise JsonSchemaValidationError(f"{path}: trailing items are forbidden")
            if isinstance(items, dict):
                for index in trailing:
                    validate_schema_instance(instance[index], items, root_schema, f"{path}[{index}]", depth + 1)

    if isinstance(instance, str):
        if "minLength" in schema and len(instance) < schema["minLength"]:
            raise JsonSchemaValidationError(f"{path}: string length is below minLength")
        if "pattern" in schema and re.search(schema["pattern"], instance) is None:
            raise JsonSchemaValidationError(f"{path}: string does not match pattern")

    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        numeric = float(instance)
        if not math.isfinite(numeric):
            raise JsonSchemaValidationError(f"{path}: number must be finite")
        if "minimum" in schema and instance < schema["minimum"]:
            raise JsonSchemaValidationError(f"{path}: number is below minimum")
        if "maximum" in schema and instance > schema["maximum"]:
            raise JsonSchemaValidationError(f"{path}: number exceeds maximum")
        if "exclusiveMinimum" in schema and instance <= schema["exclusiveMinimum"]:
            raise JsonSchemaValidationError(f"{path}: number is not above exclusiveMinimum")


def validate_with_pinned_schema(instance: Path, schema: Path) -> dict[str, Any]:
    require_canonical_file(schema, "pinned cohort JSON Schema")
    schema_document = load_json(schema)
    require(schema_document.get("$schema") == "https://json-schema.org/draft/2020-12/schema",
            "pinned cohort schema is not Draft 2020-12")
    catalog_path = schema.parent / "catalog.json"
    require_canonical_file(catalog_path, "compatibility schema catalog")
    catalog = load_json(catalog_path)
    schema_rows = [
        row for row in catalog.get("schemas", [])
        if isinstance(row, dict) and row.get("path") == "schema/compat/regional_compat_cohort_receipt.schema.json"
    ]
    require(catalog.get("unknown_schema_policy") == "FAIL_CLOSED" and len(schema_rows) == 1 and
            schema_rows[0].get("id") == schema_document.get("$id") and
            schema_rows[0].get("sha256") == sha256(schema),
            "cohort schema does not match the fail-closed compatibility catalog pin")
    audit_schema_definition(schema_document)
    instance_document = load_json(instance)
    validate_schema_instance(instance_document, schema_document, schema_document)
    return instance_document


def require_positive_finite(value: float, role: str) -> float:
    require(math.isfinite(value) and value > 0.0, f"{role} must be finite and positive")
    return value


def write_new_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = Path(str(path) + ".partial")
    require(not path.exists() and not partial.exists(),
            f"report output and partial must both be new: {path}")
    descriptor = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(partial, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except Exception:
        raise


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_digest(paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda item: item.name):
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def public_path(path: Path) -> str:
    configured_root = os.environ.get("LONGLINEAGE_PUBLIC_OUTPUT_ROOT")
    if configured_root:
        root = Path(configured_root)
        require(root.is_absolute(), "LONGLINEAGE_PUBLIC_OUTPUT_ROOT must be absolute")
        try:
            return "<EXTERNAL_OUTPUT>/" + path.relative_to(root).as_posix()
        except ValueError:
            pass
    return f"<EXTERNAL_PATH>/{path.name}"


def require_canonical_file(path: Path, role: str) -> None:
    require(path.is_absolute() and path.is_file() and not path.is_symlink(),
            f"{role} must be an absolute regular non-symlink file: {path}")
    require(path.resolve() == path, f"{role} must use its canonical path: {path}")


def parse_key_value_log(path: Path) -> dict[str, str]:
    output: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        require(bool(separator) and bool(key), f"malformed key/value run-log row: {path}")
        require(key not in output, f"duplicate run-log key {key}: {path}")
        output[key] = value
    return output


def parse_gnu_elapsed(path: Path) -> float:
    pattern = re.compile(
        r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*"
        r"([0-9]+(?::[0-9]+){1,2}(?:\.[0-9]+)?)$"
    )
    matches = [pattern.search(line) for line in path.read_text(encoding="utf-8").splitlines()]
    values = [match.group(1) for match in matches if match]
    require(len(values) == 1, f"GNU time elapsed field is not unique: {path}")
    parts = [float(value) for value in values[0].split(":")]
    require(len(parts) in (2, 3), f"unsupported GNU time duration: {values[0]}")
    if len(parts) == 2:
        elapsed = parts[0] * 60.0 + parts[1]
    else:
        elapsed = parts[0] * 3600.0 + parts[1] * 60.0 + parts[2]
    return require_positive_finite(elapsed, f"GNU time elapsed: {path}")


def seconds_between(start: str, end: str) -> float:
    return (datetime.fromisoformat(end.replace("Z", "+00:00")) -
            datetime.fromisoformat(start.replace("Z", "+00:00"))).total_seconds()


def source(source_id: str, label: str, path: Path, digest: str, description: str) -> dict[str, Any]:
    return {
        "id": source_id,
        "label": f"{label} · SHA-256 {digest}",
        "path": public_path(path),
        "description": description,
    }


def digest_utf8(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def cpp_hexfloat(value: Any, role: str) -> str:
    require(isinstance(value, (int, float)) and not isinstance(value, bool), f"{role} must be numeric")
    numeric = require_positive_finite(float(value), role)
    mantissa, exponent = numeric.hex().split("p", maxsplit=1)
    if "." in mantissa:
        mantissa = mantissa.rstrip("0").rstrip(".")
    return f"{mantissa}p{exponent}"


def replay_cohort_canonical_digests(cohort: dict[str, Any]) -> None:
    source_lines = [f"{PROFILE}\t1.0.0\n"]
    chart_lines = [f"{PROFILE}\tchart_payload\t1.0.0\n"]
    timing_order = (
        "total_wall_seconds",
        "input_sha256_seconds",
        "science_wall_seconds",
        "worker_open_seconds",
        "summed_input_seconds",
        "summed_solver_seconds",
    )
    for sample in cohort["samples"]:
        bindings = sample["source_bindings"]
        source_fields = (
            sample["dataset_order"],
            sample["dataset_id"],
            bindings["source_manifest_path"],
            bindings["source_manifest_run_id"],
            bindings["source_manifest_sha256"],
            bindings["validator_executable_sha256"],
            bindings["summary_sha256"],
            bindings["producer_receipt_sha256"],
            bindings["validation_receipt_sha256"],
            bindings["frozen_marker_sha256"],
            bindings["checksum_manifest_sha256"],
            bindings["regions_tsv_sha256"],
            bindings["units_tsv_sha256"],
            bindings["patterns_tsv_sha256"],
            bindings["crosswalk_receipt_sha256"],
            bindings["python_authority_sha256"],
        )
        source_lines.append("\t".join(str(field) for field in source_fields) + "\n")

        rows = sample["row_counts"]
        census = sample["class_census"]
        chart_fields: list[str] = [
            str(sample["dataset_order"]),
            sample["dataset_id"],
            str(rows["regions"]),
            str(rows["units"]),
            str(rows["patterns"]),
        ]
        chart_fields.extend(str(census["unit_classes"][key]) for key in UNIT_CLASSES)
        chart_fields.extend(str(census["primary_classes"][key]) for key in UNIT_CLASSES)
        chart_fields.extend(str(census["region_determinacy"][key]) for key in REGION_CLASSES)
        chart_fields.extend(cpp_hexfloat(sample["timing"][key],
                                         f"{sample['dataset_id']} timing.{key}")
                            for key in timing_order)
        peak = sample["peak_rss"]
        chart_fields.extend([
            "1" if peak["available"] else "0",
            str(peak["bytes"] if peak["available"] else 0),
            peak["source"] if peak["available"] else "",
        ])
        chart_lines.append("\t".join(chart_fields) + "\n")

    observed_source_set = digest_utf8("".join(source_lines))
    observed_chart_payload = digest_utf8("".join(chart_lines))
    require(observed_source_set == cohort["source_set_sha256"],
            "cohort source_set_sha256 canonical replay mismatch")
    require(observed_chart_payload == cohort["chart_payload_sha256"],
            "cohort chart_payload_sha256 canonical replay mismatch")


def materialized_query_source(
    dataset_id: str,
    rows: list[dict[str, Any]],
    generated_at: str,
    description: str,
) -> dict[str, Any]:
    require(rows, f"presentation dataset is empty: {dataset_id}")
    require(dataset_id.replace("_", "").isalnum(), f"unsafe dataset id: {dataset_id}")
    columns = list(rows[0])
    require(all(list(row) == columns for row in rows), f"ragged rows: {dataset_id}")
    require(all(column.replace("_", "").isalnum() for column in columns),
            f"unsafe presentation column: {dataset_id}")
    table = f"presentation_{dataset_id}"

    def sql_type(column: str) -> str:
        values = [row[column] for row in rows if row[column] is not None]
        if any(isinstance(value, float) for value in values):
            return "REAL"
        if values and all(isinstance(value, (bool, int)) for value in values):
            return "INTEGER"
        return "TEXT"

    connection = sqlite3.connect(":memory:")
    definitions = ", ".join(f'"{column}" {sql_type(column)}' for column in columns)
    connection.execute(f'CREATE TABLE "{table}" ({definitions})')
    placeholders = ", ".join("?" for _ in columns)
    connection.executemany(
        f'INSERT INTO "{table}" VALUES ({placeholders})',
        [[row[column] for column in columns] for row in rows],
    )
    selection = ", ".join(f'"{column}"' for column in columns)
    query = f'SELECT {selection} FROM "{table}" ORDER BY rowid'
    replayed = connection.execute(query).fetchall()
    expected = [tuple(row[column] for column in columns) for row in rows]
    connection.close()
    require(replayed == expected, f"presentation SQL replay mismatch: {dataset_id}")
    return {
        "id": f"query_{dataset_id}",
        "label": f"Presentation SELECT for {dataset_id}",
        "path": "presentation/build_regional_compat_all_datasets_report.py",
        "query": {
            "engine": "sqlite",
            "language": "sql",
            "sql": query,
            "description": description,
            "tables_used": [table],
            "executed_at": generated_at,
            "filters": ["Rows are presentation projections of validated C++ receipts and timing logs."],
        },
    }


def validate_cohort(cohort: dict[str, Any]) -> None:
    require(cohort.get("schema_name") == "longlineage.regional_compat_cohort_receipt",
            "cohort schema mismatch")
    require(cohort.get("schema_version") == "1.0.0", "cohort schema version mismatch")
    require(cohort.get("profile_id") == PROFILE, "cohort profile mismatch")
    require(cohort.get("state") ==
            "VALIDATED_FROZEN_BUNDLES_WITH_EXACT_CROSSWALK_RECEIPTS_AGGREGATED",
            "cohort state mismatch")
    require(cohort.get("dataset_count") == 7, "cohort dataset count mismatch")
    require(cohort.get("source_authority_profile") == "PRODUCTION_7_DATASET",
            "cohort source authority mismatch")
    require(cohort.get("all_sources_validated_frozen") is True,
            "not all C++ sources are validated frozen")
    require(cohort.get("all_crosswalk_receipts_exact") is True,
            "not all crosswalk receipts report exact")
    require(cohort.get("crosswalk_receipts_independently_frozen") is False,
            "crosswalk evidence scope changed unexpectedly")
    for key in ("source_authority_sha256", "python_authority_sha256",
                "source_set_sha256", "chart_payload_sha256"):
        require(re.fullmatch(r"[0-9a-f]{64}", str(cohort.get(key, ""))) is not None,
                f"cohort {key} is not a SHA-256")
    ceiling = cohort.get("claim_ceiling", {})
    require(ceiling == {
        "descriptive_python_compatibility_only": True,
        "formal_m2_topology": False,
        "production_release": False,
        "clone_ancestor_time_order": False,
        "crosswalk_mismatch_tolerance": False,
    }, "claim ceiling mismatch")
    samples = cohort.get("samples")
    require(isinstance(samples, list) and len(samples) == 7, "cohort samples mismatch")
    require([sample.get("dataset_id") for sample in samples] == DATASETS,
            "cohort dataset order mismatch")
    require([sample.get("dataset_order") for sample in samples] == list(range(7)),
            "cohort dataset ordinal mismatch")

    total_rows = {key: 0 for key in ("regions", "units", "patterns")}
    total_regions = {key: 0 for key in REGION_CLASSES}
    total_units = {key: 0 for key in UNIT_CLASSES}
    total_primary = {key: 0 for key in UNIT_CLASSES}
    for sample in samples:
        require(sample.get("workers") == 24,
                f"worker count is not the frozen 24-worker contract: {sample.get('dataset_id')}")
        require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,159}",
                             str(sample.get("run_id", ""))) is not None,
                f"unsafe sample run id: {sample.get('dataset_id')}")
        bindings = sample["source_bindings"]
        require(sample["run_id"] == f"{bindings['source_manifest_run_id']}-{sample['dataset_id']}",
                f"sample run-id/source-manifest mapping mismatch: {sample['dataset_id']}")
        rows = sample["row_counts"]
        require(all(isinstance(rows[key], int) and not isinstance(rows[key], bool) and rows[key] > 0
                    for key in total_rows),
                f"sample row counts must be positive integers: {sample['dataset_id']}")
        for key, value in sample["timing"].items():
            require_positive_finite(float(value), f"{sample['dataset_id']} timing.{key}")
        for key in total_rows:
            total_rows[key] += rows[key]
        census = sample["class_census"]
        require(list(census["unit_classes"].keys()) == sorted(UNIT_CLASSES),
                f"unit class key set mismatch: {sample['dataset_id']}")
        require(list(census["primary_classes"].keys()) == sorted(UNIT_CLASSES),
                f"primary class key set mismatch: {sample['dataset_id']}")
        require(list(census["region_determinacy"].keys()) == sorted(REGION_CLASSES),
                f"region class key set mismatch: {sample['dataset_id']}")
        require(sum(census["unit_classes"].values()) == rows["units"],
                f"unit census conservation failed: {sample['dataset_id']}")
        require(sum(census["region_determinacy"].values()) == rows["regions"],
                f"region census conservation failed: {sample['dataset_id']}")
        for key in UNIT_CLASSES:
            total_units[key] += census["unit_classes"][key]
            total_primary[key] += census["primary_classes"][key]
        for key in REGION_CLASSES:
            total_regions[key] += census["region_determinacy"][key]
        crosswalk = sample["crosswalk"]
        require(crosswalk["receipt_reports_all_exact"] is True and
                crosswalk["receipt_independently_frozen"] is False and
                sum(crosswalk[key] for key in (
                    "regions_mismatches", "units_mismatches", "patterns_mismatches")) == 0,
                f"crosswalk mismatch: {sample['dataset_id']}")

    totals = cohort["cohort_totals"]
    require(totals["row_counts"] == total_rows, "cohort row total replay mismatch")
    require(totals["class_census"]["unit_classes"] == dict(sorted(total_units.items())),
            "cohort unit census replay mismatch")
    require(totals["class_census"]["primary_classes"] == dict(sorted(total_primary.items())),
            "cohort primary census replay mismatch")
    require(totals["class_census"]["region_determinacy"] == dict(sorted(total_regions.items())),
            "cohort region census replay mismatch")
    for key, value in totals["sequential_timing"].items():
        require_positive_finite(float(value), f"cohort sequential_timing.{key}")
    replay_cohort_canonical_digests(cohort)


def main() -> int:
    args = parse_args()
    require(args.repo_root.is_absolute() and args.repo_root.is_dir(), "repo-root must be an absolute directory")
    require(args.repo_root.resolve() == args.repo_root, "repo-root must be canonical")
    require(args.output.is_absolute() and not args.output.exists() and
            not Path(str(args.output) + ".partial").exists(),
            "report output and partial must be new absolute paths")
    require_canonical_file(args.cohort_receipt, "cohort receipt")
    cohort = validate_with_pinned_schema(
        args.cohort_receipt,
        args.repo_root / "schema/compat/regional_compat_cohort_receipt.schema.json",
    )
    validate_cohort(cohort)

    require(args.historical_python_root.is_absolute() and args.historical_python_root.is_dir(),
            "historical-python-root must be an absolute directory")
    require(args.historical_python_root.resolve() == args.historical_python_root,
            "historical-python-root must be canonical")
    historical_run_id = args.historical_python_root.name
    event_paths = sorted((args.historical_python_root / "state_events").glob("*.json"))
    require(len(event_paths) == 6, "historical Python lifecycle must contain six events")
    events = [load_json(path) for path in event_paths]
    require([event["state"] for event in events] ==
            ["CREATING", "PREFLIGHT", "READY", "RUNNING", "VERIFYING", "SUCCEEDED"],
            "historical Python lifecycle state order mismatch")
    require([event.get("sequence") for event in events] == list(range(1, 7)),
            "historical Python lifecycle sequence mismatch")
    require(all(event.get("run_id") == historical_run_id for event in events),
            "historical Python lifecycle run-id binding mismatch")
    require(events[0].get("previous_state_sha256") == "0" * 64 and
            all(events[index].get("previous_state_sha256") == events[index - 1].get("state_sha256")
                for index in range(1, len(events))),
            "historical Python lifecycle digest chain mismatch")

    launch_receipt_path = args.historical_python_root / "launch_receipt.json"
    output_manifests_path = args.historical_python_root / "output_manifests.json"
    verification_summary_path = args.historical_python_root / "verification_summary.json"
    success_marker_path = args.historical_python_root / "_SUCCESS"
    for role, path in (
        ("historical launch receipt", launch_receipt_path),
        ("historical output manifests", output_manifests_path),
        ("historical verification summary", verification_summary_path),
        ("historical success marker", success_marker_path),
    ):
        require_canonical_file(path, role)
    launch_receipt = load_json(launch_receipt_path)
    output_manifests = load_json(output_manifests_path)
    verification_summary = load_json(verification_summary_path)
    success_marker = load_json(success_marker_path)
    analysis_params = launch_receipt.get("extra", {}).get("analysis_params", {})
    python_parallel_samples = analysis_params.get("parallel_samples")
    require(launch_receipt.get("schema_name") == "intersubmod.layered_run_receipt" and
            launch_receipt.get("schema_version") == "1.0.0" and
            launch_receipt.get("run_id") == historical_run_id,
            "historical Python launch receipt identity mismatch")
    require(analysis_params.get("dataset_count") == 7 and
            analysis_params.get("scope") == "chr1-22" and
            analysis_params.get("parallel_parts_per_sample") == 1 and
            isinstance(python_parallel_samples, int) and python_parallel_samples > 0,
            "historical Python launch scope/parallelism mismatch")
    require(output_manifests.get("dataset_count") == 7 and
            {row.get("sample") for row in output_manifests.get("manifests", [])} == set(DATASETS),
            "historical Python output-manifest set mismatch")
    require(verification_summary.get("all_pass") is True and
            verification_summary.get("run_id") == historical_run_id and
            verification_summary.get("dataset_count") == 7,
            "historical Python verification summary mismatch")
    require(success_marker.get("schema_name") == "intersubmod.layered_success_marker" and
            success_marker.get("schema_version") == "1.0.0" and
            success_marker.get("run_id") == historical_run_id and
            success_marker.get("verification_path") == str(verification_summary_path) and
            success_marker.get("verification_sha256") == sha256(verification_summary_path) and
            success_marker.get("launch_receipt_sha256") == sha256(launch_receipt_path) and
            success_marker.get("extra") == {
                "biological_sample_count": 6,
                "dataset_count": 7,
                "mode": "full",
                "scope": "chr1-22",
            }, "historical Python success marker binding mismatch")
    python_parallelism_label = f"up to {python_parallel_samples} samples"
    python_preflight = seconds_between(events[0]["timestamp_utc"], events[2]["timestamp_utc"])
    python_science = seconds_between(events[3]["timestamp_utc"], events[4]["timestamp_utc"])
    python_validation = seconds_between(events[4]["timestamp_utc"], events[5]["timestamp_utc"])
    python_end_to_end = seconds_between(events[0]["timestamp_utc"], events[5]["timestamp_utc"])
    for role, seconds in (
        ("historical Python preflight", python_preflight),
        ("historical Python science", python_science),
        ("historical Python validation", python_validation),
        ("historical Python end-to-end", python_end_to_end),
    ):
        require_positive_finite(seconds, role)

    require(args.input_hash_root.is_absolute() and args.input_hash_root.is_dir(),
            "input-hash-root must be an absolute directory")
    require(args.input_hash_root.resolve() == args.input_hash_root,
            "input-hash-root must be canonical")
    regional_authority = load_json(args.repo_root / "oracle/regional_compat_all_datasets_input_authority.json")
    require([row.get("dataset_id") for row in regional_authority.get("datasets", [])] == DATASETS,
            "regional authority dataset order mismatch")
    authority_hash_rows: list[dict[str, Any]] = []
    hash_sources: list[tuple[str, Path, str]] = []
    authority_hash_active_seconds = 0.0
    authority_hash_start_epochs: list[float] = []
    authority_hash_end_epochs: list[float] = []
    authority_raw_digests: dict[str, str] = {}
    hash_batch_log = args.input_hash_root / "raw_bam_hash_batch.run.log"
    require(hash_batch_log.is_file(), "raw BAM hash batch log is missing")
    hash_sources.append(("raw_bam_hash_batch", hash_batch_log,
                         "Resource-gated raw BAM authority hash lifecycle; final three scans may overlap."))
    for authority_dataset in regional_authority["datasets"]:
        dataset = authority_dataset["dataset_id"]
        raw_rows = [row for row in authority_dataset["files"] if row["role"] == "raw_bam"]
        require(len(raw_rows) == 1, f"regional authority raw BAM row mismatch: {dataset}")
        raw_row = raw_rows[0]
        authority_raw_digests[dataset] = raw_row["sha256"]
        if dataset == "HCC1395":
            authority_hash_rows.append({
                "dataset": dataset,
                "size_gb_decimal": raw_row["size_bytes"] / 1_000_000_000.0,
                "minutes": None,
                "mbps_decimal": None,
                "authority_source": "previous frozen full-content HCC dataset authority",
            })
            continue
        digest_path = args.input_hash_root / f"{dataset}.raw_bam.sha256.txt"
        time_path = args.input_hash_root / f"{dataset}.raw_bam.sha256.time.txt"
        require(digest_path.is_file() and time_path.is_file(), f"raw BAM hash evidence missing: {dataset}")
        lines = digest_path.read_text(encoding="utf-8").splitlines()
        require(len(lines) == 1, f"raw BAM hash receipt malformed: {dataset}")
        fields = lines[0].split(maxsplit=1)
        require(len(fields) == 2 and fields[0] == raw_row["sha256"],
                f"raw BAM hash receipt differs from authority: {dataset}")
        elapsed = parse_gnu_elapsed(time_path)
        authority_hash_active_seconds += elapsed
        completed_epoch = max(digest_path.stat().st_mtime, time_path.stat().st_mtime)
        authority_hash_start_epochs.append(completed_epoch - elapsed)
        authority_hash_end_epochs.append(completed_epoch)
        completed_at = datetime.fromtimestamp(completed_epoch, timezone.utc).isoformat()
        authority_mode = (
            "concurrent final-three full-content sha256sum"
            if dataset in ("HCC1954", "H2009", "HCC1937")
            else "sequential full-content sha256sum"
        )
        authority_hash_rows.append({
            "dataset": dataset,
            "size_gb_decimal": raw_row["size_bytes"] / 1_000_000_000.0,
            "minutes": elapsed / 60.0,
            "mbps_decimal": raw_row["size_bytes"] / elapsed / 1_000_000.0,
            "authority_source": f"{authority_mode} completed {completed_at}",
        })
        hash_sources.extend([
            (f"{dataset}_raw_bam_sha256", digest_path, f"{dataset} full-content raw BAM digest receipt."),
            (f"{dataset}_raw_bam_hash_time", time_path, f"{dataset} GNU time raw BAM hash observation."),
        ])
    hash_log_text = hash_batch_log.read_text(encoding="utf-8")
    require(" HASH_FAIL " not in hash_log_text and "HASH_PARALLEL_BATCH_FAIL" not in hash_log_text,
            "raw BAM authority hash lifecycle contains a failure marker")
    require(hash_log_text.count("HASH_PARALLEL_BATCH_BEGIN datasets=3 concurrency=3") == 1 and
            hash_log_text.count("HASH_PARALLEL_BATCH_DONE datasets=3") == 1,
            "raw BAM authority hash lifecycle does not bind one completed final-three parallel batch")
    for dataset in ("HCC1954", "H2009", "HCC1937"):
        marker = (f"HASH_PARALLEL_DONE dataset={dataset} sha256={authority_raw_digests[dataset]} "
                  "identity_stable=1")
        require(hash_log_text.count(marker) == 1,
                f"raw BAM authority lifecycle completion/identity marker mismatch: {dataset}")
    for dataset in ("H1437", "HCC1395_DORADO"):
        marker = f"HASH_DONE dataset={dataset} sha256={authority_raw_digests[dataset]}"
        require(hash_log_text.count(marker) == 1,
                f"raw BAM authority lifecycle completion marker mismatch: {dataset}")
    require(len(authority_hash_start_epochs) == len(authority_hash_end_epochs) == 6,
            "raw BAM authority timing interval set is incomplete")
    authority_hash_elapsed_seconds = require_positive_finite(
        max(authority_hash_end_epochs) - min(authority_hash_start_epochs),
        "raw BAM authority elapsed wall",
    )

    sample_counts: list[dict[str, Any]] = []
    sample_timing: list[dict[str, Any]] = []
    region_rows: list[dict[str, Any]] = []
    unit_rows: list[dict[str, Any]] = []
    provenance_rows: list[dict[str, Any]] = []
    operational_sources: list[tuple[str, Path, str]] = []
    cpp_wrapper_seconds = 0.0
    cpp_validator_seconds = 0.0
    cpp_producer_elapsed_seconds = 0.0

    for sample in cohort["samples"]:
        dataset = sample["dataset_id"]
        rows = sample["row_counts"]
        sample_counts.append({
            "dataset": dataset,
            "regions": rows["regions"],
            "units": rows["units"],
            "patterns": rows["patterns"],
            "crosswalk_mismatches": 0,
        })

        bundle = Path(sample["source_bindings"]["frozen_bundle"])
        run_log = Path(str(bundle) + ".run.log")
        producer_time = Path(str(bundle) + ".producer.time.txt")
        validator_time = Path(str(bundle) + ".validator.time.txt")
        require(all(path.is_file() for path in (run_log, producer_time, validator_time)),
                f"operational timing evidence is incomplete: {dataset}")
        run_fields = parse_key_value_log(run_log)
        require(run_fields.get("RUN_STATE") == "VALIDATED_FROZEN", f"run log not frozen: {dataset}")
        require(run_fields.get("INPUT_DATASET_ID") == dataset, f"run log dataset mismatch: {dataset}")
        wrapper_seconds = require_positive_finite(
            float(run_fields["END_TO_END_WALL_SECONDS"]),
            f"{dataset} wrapper elapsed",
        )
        producer_elapsed = parse_gnu_elapsed(producer_time)
        validator_elapsed = parse_gnu_elapsed(validator_time)
        cpp_wrapper_seconds += wrapper_seconds
        cpp_producer_elapsed_seconds += producer_elapsed
        cpp_validator_seconds += validator_elapsed
        timing = sample["timing"]
        sample_timing.append({
            "dataset": dataset,
            "workers": sample["workers"],
            "wrapper_minutes": wrapper_seconds / 60.0,
            "producer_minutes": producer_elapsed / 60.0,
            "input_sha256_minutes": timing["input_sha256_seconds"] / 60.0,
            "science_minutes": timing["science_wall_seconds"] / 60.0,
            "validator_minutes": validator_elapsed / 60.0,
            "summed_input_worker_minutes": timing["summed_input_seconds"] / 60.0,
            "summed_solver_worker_minutes": timing["summed_solver_seconds"] / 60.0,
        })
        operational_sources.extend([
            (f"{dataset}_run_log", run_log, f"{dataset} wrapper lifecycle and end-to-end timing."),
            (f"{dataset}_producer_time", producer_time, f"{dataset} GNU time producer observation."),
            (f"{dataset}_validator_time", validator_time, f"{dataset} GNU time validator observation."),
        ])

        for classification in REGION_CLASSES:
            region_rows.append({
                "dataset": dataset,
                "class": classification,
                "count": sample["class_census"]["region_determinacy"][classification],
                "denominator": rows["regions"],
            })
        for cohort_name, key in (("all_units", "unit_classes"), ("primary_units", "primary_classes")):
            for classification in UNIT_CLASSES:
                unit_rows.append({
                    "dataset": dataset,
                    "cohort": cohort_name,
                    "class": classification,
                    "count": sample["class_census"][key][classification],
                })
        bindings = sample["source_bindings"]
        provenance_rows.append({
            "dataset": dataset,
            "source_manifest_sha256": bindings["source_manifest_sha256"],
            "validator_executable_sha256": bindings["validator_executable_sha256"],
            "validation_receipt_sha256": bindings["validation_receipt_sha256"],
            "frozen_marker_sha256": bindings["frozen_marker_sha256"],
            "crosswalk_receipt_sha256": bindings["crosswalk_receipt_sha256"],
            "python_manifest_sha256": bindings["python_output_manifest_sha256"],
            "python_region_view_sha256": bindings["python_region_view_sha256"],
        })

    require_canonical_file(args.batch_log, "C++ cohort batch log")
    batch_fields = parse_key_value_log(args.batch_log)
    expected_batch_keys = {
        "BATCH_STARTED_AT",
        "INPUT_REPOSITORY_ROOT",
        "INPUT_MANIFEST",
        "INPUT_MANIFEST_SHA256",
        "INPUT_DATASET_ORDER",
        "PRODUCER_EXECUTABLE",
        "PRODUCER_EXECUTABLE_SHA256",
        "VALIDATOR_EXECUTABLE",
        "VALIDATOR_EXECUTABLE_SHA256",
        "EXECUTABLES_STABLE",
        "SOURCE_MANIFEST_STABLE",
        "RESOURCE_WORKERS_PER_DATASET",
        "RESOURCE_EXECUTION_MODE",
        "OUTPUT_ROOT",
        "BATCH_FINISHED_AT",
        "COHORT_ELAPSED_WALL_SECONDS",
        "ALL_DATASET_BUNDLES_FROZEN",
        "OUTPUT_BATCH_LOG",
    }
    for dataset in DATASETS:
        expected_batch_keys.update({
            f"DATASET_{dataset}_BEGIN_AT",
            f"DATASET_{dataset}_FROZEN_AT",
            f"DATASET_{dataset}_FROZEN_PATH",
        })
    require(set(batch_fields) == expected_batch_keys, "batch log does not have the exact full-run key inventory")
    expected_bundle_root = str(Path(cohort["samples"][0]["source_bindings"]["frozen_bundle"]).parent)
    require(all(str(Path(sample["source_bindings"]["frozen_bundle"]).parent) == expected_bundle_root
                for sample in cohort["samples"]),
            "cohort sample bundles do not share one batch root")
    manifest_paths = {sample["source_bindings"]["source_manifest_path"] for sample in cohort["samples"]}
    manifest_digests = {sample["source_bindings"]["source_manifest_sha256"] for sample in cohort["samples"]}
    manifest_run_ids = {sample["source_bindings"]["source_manifest_run_id"] for sample in cohort["samples"]}
    validator_binding_digests = {
        sample["source_bindings"]["validator_executable_sha256"] for sample in cohort["samples"]
    }
    require(len(manifest_paths) == len(manifest_digests) == len(manifest_run_ids) == 1,
            "seven cohort samples do not share one source manifest path/SHA/run-id")
    require(len(validator_binding_digests) == 1,
            "seven cohort samples do not share one validator executable SHA")
    source_manifest_path = Path(next(iter(manifest_paths)))
    source_manifest_digest = next(iter(manifest_digests))
    source_manifest_run_id = next(iter(manifest_run_ids))
    require_canonical_file(source_manifest_path, "seven-dataset source manifest")
    require(sha256(source_manifest_path) == source_manifest_digest,
            "current source-manifest bytes differ from all seven sample bindings")
    source_manifest = load_json(source_manifest_path)
    require(source_manifest.get("run_id") == source_manifest_run_id and
            source_manifest.get("output_root") == expected_bundle_root and
            source_manifest.get("runtime", {}).get("compute_workers") == 24,
            "current source-manifest run/output/worker contract differs from cohort bindings")
    require(batch_fields.get("ALL_DATASET_BUNDLES_FROZEN") == "7" and
            batch_fields.get("OUTPUT_ROOT") == expected_bundle_root and
            batch_fields.get("OUTPUT_BATCH_LOG") == str(args.batch_log),
            "batch log completion/output-root binding mismatch")
    require(batch_fields.get("INPUT_REPOSITORY_ROOT") == str(args.repo_root) and
            batch_fields.get("INPUT_MANIFEST") == str(source_manifest_path) and
            batch_fields.get("INPUT_MANIFEST_SHA256") == source_manifest_digest,
            "batch log input repository/source-manifest path/SHA binding mismatch")
    require(batch_fields.get("INPUT_DATASET_ORDER") == ",".join(DATASETS),
            "batch log dataset order differs from the seven-dataset contract")
    require(batch_fields.get("RESOURCE_WORKERS_PER_DATASET") == "24" and
            batch_fields.get("RESOURCE_EXECUTION_MODE") == "SEQUENTIAL_DATASETS",
            "batch log workers/execution mode differs from 24-worker sequential contract")
    producer_executable = Path(batch_fields["PRODUCER_EXECUTABLE"])
    validator_executable = Path(batch_fields["VALIDATOR_EXECUTABLE"])
    require_canonical_file(producer_executable, "batch producer executable")
    require_canonical_file(validator_executable, "batch validator executable")
    require(os.access(producer_executable, os.X_OK) and os.access(validator_executable, os.X_OK),
            "batch producer and validator files must remain executable")
    producer_executable_digest = sha256(producer_executable)
    validator_executable_digest = sha256(validator_executable)
    require(batch_fields["PRODUCER_EXECUTABLE_SHA256"] == producer_executable_digest and
            batch_fields["VALIDATOR_EXECUTABLE_SHA256"] == validator_executable_digest,
            "batch executable SHA differs from current canonical binary bytes")
    require(batch_fields["EXECUTABLES_STABLE"] == "1",
            "batch wrapper did not attest stable producer/validator executable bytes")
    require(batch_fields["SOURCE_MANIFEST_STABLE"] == "1",
            "batch wrapper did not attest stable source-manifest bytes and identity")
    require(validator_executable_digest == next(iter(validator_binding_digests)),
            "batch validator SHA differs from all seven sample receipt bindings")
    require(all(
        f"DATASET_{dataset}_BEGIN_AT" in batch_fields and
        f"DATASET_{dataset}_FROZEN_AT" in batch_fields and
        batch_fields.get(f"DATASET_{dataset}_FROZEN_PATH") ==
        str(Path(expected_bundle_root) / dataset / "FROZEN")
        for dataset in DATASETS
    ), "batch log does not bind all seven dataset lifecycle rows")
    previous_timestamp = batch_fields["BATCH_STARTED_AT"]
    for dataset in DATASETS:
        begin_at = batch_fields[f"DATASET_{dataset}_BEGIN_AT"]
        frozen_at = batch_fields[f"DATASET_{dataset}_FROZEN_AT"]
        require(seconds_between(previous_timestamp, begin_at) >= 0.0 and
                seconds_between(begin_at, frozen_at) > 0.0,
                f"batch log is not sequential/positive-duration at dataset {dataset}")
        previous_timestamp = frozen_at
    require(seconds_between(previous_timestamp, batch_fields["BATCH_FINISHED_AT"]) >= 0.0,
            "batch completion timestamp precedes the final frozen dataset")
    cohort_elapsed_seconds = require_positive_finite(
        float(batch_fields["COHORT_ELAPSED_WALL_SECONDS"]),
        "C++ cohort elapsed wall",
    )
    observed_batch_interval = seconds_between(
        batch_fields["BATCH_STARTED_AT"], batch_fields["BATCH_FINISHED_AT"]
    )
    require(abs(observed_batch_interval - cohort_elapsed_seconds) <= 2.0,
            "batch integer elapsed differs from timestamp interval")
    operational_sources.append((
        "cpp_batch_log", args.batch_log,
        "Seven-dataset outer lifecycle, source-manifest binding and true cohort elapsed wall.",
    ))
    operational_sources.extend([
        (
            "cpp_batch_producer_executable",
            producer_executable,
            "Producer binary full SHA for this batch execution; not sample-receipt-bound.",
        ),
        (
            "cpp_batch_validator_executable",
            validator_executable,
            "Validator binary full SHA, equal to all seven sample receipt bindings.",
        ),
    ])

    require_canonical_file(args.crosswalk_log, "C++ crosswalk/cohort batch log")
    crosswalk_fields = parse_key_value_log(args.crosswalk_log)
    expected_crosswalk_keys = {
        "CROSSWALK_BATCH_STARTED_AT",
        "INPUT_REPOSITORY_ROOT",
        "INPUT_BUNDLE_ROOT",
        "INPUT_PYTHON_ROOT",
        "COMPAT_EXECUTABLE",
        "COMPAT_EXECUTABLE_SHA256",
        "OUTPUT_DIRECTORY",
        "OUTPUT_COHORT_RECEIPT",
        "ALL_DATASET_CROSSWALKS_EXACT",
        "COHORT_COMMAND",
        "OUTPUT_COHORT_SHA256",
        "CROSSWALK_BATCH_FINISHED_AT",
        "CROSSWALK_ELAPSED_WALL_SECONDS",
        "EXECUTABLE_STABLE",
        "OUTPUT_BATCH_LOG",
    }
    for dataset in DATASETS:
        expected_crosswalk_keys.update({
            f"DATASET_{dataset}_BEGIN_AT",
            f"DATASET_{dataset}_CPP_BUNDLE",
            f"DATASET_{dataset}_PYTHON_MANIFEST",
            f"DATASET_{dataset}_COMMAND",
            f"DATASET_{dataset}_EXACT_AT",
            f"DATASET_{dataset}_OUTPUT",
            f"DATASET_{dataset}_OUTPUT_SHA256",
        })
    require(set(crosswalk_fields) == expected_crosswalk_keys,
            "crosswalk/cohort log does not have the exact full-run key inventory")
    crosswalk_paths = {
        Path(sample["source_bindings"]["crosswalk_receipt"]) for sample in cohort["samples"]
    }
    crosswalk_directories = {path.parent for path in crosswalk_paths}
    require(len(crosswalk_directories) == 1,
            "seven crosswalk receipts do not share one output directory")
    expected_crosswalk_dir = next(iter(crosswalk_directories))
    require(crosswalk_fields["INPUT_REPOSITORY_ROOT"] == str(args.repo_root) and
            crosswalk_fields["INPUT_BUNDLE_ROOT"] == expected_bundle_root and
            crosswalk_fields["INPUT_PYTHON_ROOT"] == str(args.historical_python_root) and
            crosswalk_fields["OUTPUT_DIRECTORY"] == str(expected_crosswalk_dir) and
            crosswalk_fields["OUTPUT_COHORT_RECEIPT"] == str(args.cohort_receipt) and
            crosswalk_fields["OUTPUT_BATCH_LOG"] == str(args.crosswalk_log),
            "crosswalk/cohort log input/output path binding mismatch")
    require(crosswalk_fields["COMPAT_EXECUTABLE"] == str(producer_executable) and
            crosswalk_fields["COMPAT_EXECUTABLE_SHA256"] == producer_executable_digest and
            crosswalk_fields["EXECUTABLE_STABLE"] == "1",
            "crosswalk/cohort executable is not the stable batch producer binary")
    require(crosswalk_fields["ALL_DATASET_CROSSWALKS_EXACT"] == "7" and
            crosswalk_fields["OUTPUT_COHORT_SHA256"] == sha256(args.cohort_receipt),
            "crosswalk/cohort completion or cohort SHA binding mismatch")
    expected_cohort_command = (
        f"{producer_executable} cohort --bundle-root {expected_bundle_root} "
        f"--crosswalk-dir {expected_crosswalk_dir} --output {args.cohort_receipt}"
    )
    require(crosswalk_fields["COHORT_COMMAND"] == expected_cohort_command,
            "crosswalk/cohort log command binding mismatch")
    previous_timestamp = crosswalk_fields["CROSSWALK_BATCH_STARTED_AT"]
    samples_by_id = {sample["dataset_id"]: sample for sample in cohort["samples"]}
    for dataset in DATASETS:
        sample = samples_by_id[dataset]
        bindings = sample["source_bindings"]
        expected_bundle = str(Path(expected_bundle_root) / dataset)
        expected_python_manifest = str(
            args.historical_python_root / "samples" / dataset / "output_manifest.json"
        )
        expected_crosswalk = Path(bindings["crosswalk_receipt"])
        expected_command = (
            f"{producer_executable} crosswalk --repo-root {args.repo_root} "
            f"--bundle {expected_bundle} --python-manifest {expected_python_manifest} "
            f"--output {expected_crosswalk}"
        )
        begin_at = crosswalk_fields[f"DATASET_{dataset}_BEGIN_AT"]
        exact_at = crosswalk_fields[f"DATASET_{dataset}_EXACT_AT"]
        require(seconds_between(previous_timestamp, begin_at) >= 0.0 and
                seconds_between(begin_at, exact_at) >= 0.0,
                f"crosswalk log is not sequential at dataset {dataset}")
        require(crosswalk_fields[f"DATASET_{dataset}_CPP_BUNDLE"] == expected_bundle and
                crosswalk_fields[f"DATASET_{dataset}_PYTHON_MANIFEST"] == expected_python_manifest and
                crosswalk_fields[f"DATASET_{dataset}_COMMAND"] == expected_command and
                crosswalk_fields[f"DATASET_{dataset}_OUTPUT"] == str(expected_crosswalk) and
                crosswalk_fields[f"DATASET_{dataset}_OUTPUT_SHA256"] ==
                bindings["crosswalk_receipt_sha256"] == sha256(expected_crosswalk),
                f"crosswalk log source/command/output binding mismatch: {dataset}")
        previous_timestamp = exact_at
    require(seconds_between(previous_timestamp,
                            crosswalk_fields["CROSSWALK_BATCH_FINISHED_AT"]) >= 0.0,
            "crosswalk batch completion precedes the final exact receipt")
    crosswalk_elapsed_seconds = require_positive_finite(
        float(crosswalk_fields["CROSSWALK_ELAPSED_WALL_SECONDS"]),
        "crosswalk/cohort elapsed wall",
    )
    observed_crosswalk_interval = seconds_between(
        crosswalk_fields["CROSSWALK_BATCH_STARTED_AT"],
        crosswalk_fields["CROSSWALK_BATCH_FINISHED_AT"],
    )
    require(abs(observed_crosswalk_interval - crosswalk_elapsed_seconds) <= 2.0,
            "crosswalk batch integer elapsed differs from timestamp interval")
    operational_sources.append((
        "cpp_crosswalk_batch_log",
        args.crosswalk_log,
        "Seven exact crosswalks, cohort aggregation and compatibility-executable stability.",
    ))

    totals = cohort["cohort_totals"]["row_counts"]
    cpp_science_seconds = cohort["cohort_totals"]["sequential_timing"]["science_wall_seconds"]
    observed_science_ratio = python_science / cpp_science_seconds
    cpp_parallelism_label = "7 sequential datasets × 24 workers"
    comparison_rows = [
        {"implementation": "Historical Python", "stage": "preflight", "minutes": python_preflight / 60.0,
         "parallelism": python_parallelism_label, "scope_note": f"Frozen run {historical_run_id}"},
        {"implementation": "Historical Python", "stage": "science", "minutes": python_science / 60.0,
         "parallelism": python_parallelism_label, "scope_note": "RUNNING→VERIFYING"},
        {"implementation": "Historical Python", "stage": "validation", "minutes": python_validation / 60.0,
         "parallelism": "verifier", "scope_note": "VERIFYING→SUCCEEDED"},
        {"implementation": "Historical Python", "stage": "end-to-end", "minutes": python_end_to_end / 60.0,
         "parallelism": python_parallelism_label, "scope_note": "CREATING→SUCCEEDED"},
        {"implementation": "C++ indexed-I/O", "stage": "one-time authority hash active sum",
         "minutes": authority_hash_active_seconds / 60.0,
         "parallelism": "mixed: initial sequential; final 3 concurrent nice/ionice",
         "scope_note": "Sum of six per-file GNU-time observations; excluded from producer wall"},
        {"implementation": "C++ indexed-I/O", "stage": "one-time authority hash elapsed wall",
         "minutes": authority_hash_elapsed_seconds / 60.0,
         "parallelism": "mixed: initial sequential; final 3 concurrent nice/ionice",
         "scope_note": "Earliest measured scan start to latest receipt completion, including scheduling gaps"},
        {"implementation": "C++ indexed-I/O", "stage": "producer", "minutes": cpp_producer_elapsed_seconds / 60.0,
         "parallelism": cpp_parallelism_label, "scope_note": "Sum of GNU time producer wall"},
        {"implementation": "C++ indexed-I/O", "stage": "science", "minutes": cpp_science_seconds / 60.0,
         "parallelism": cpp_parallelism_label, "scope_note": "C++ receipt science wall sum"},
        {"implementation": "C++ indexed-I/O", "stage": "validation", "minutes": cpp_validator_seconds / 60.0,
         "parallelism": "7 sequential", "scope_note": "Sum of GNU time validator wall"},
        {"implementation": "C++ indexed-I/O", "stage": "active end-to-end sum",
         "minutes": cpp_wrapper_seconds / 60.0, "parallelism": cpp_parallelism_label,
         "scope_note": "Sum of seven wrapper wall logs"},
        {"implementation": "C++ indexed-I/O", "stage": "cohort elapsed wall",
         "minutes": cohort_elapsed_seconds / 60.0, "parallelism": cpp_parallelism_label,
         "scope_note": "Outer BATCH_STARTED_AT→BATCH_FINISHED_AT including inter-sample scheduling"},
    ]

    method_steps = [
        {"stage": "1. Authority lock", "decision": "七樣本固定順序；每樣本恰好八個 truth-free 角色",
         "verification": "raw BAM/BAI、PASS VCF/index、HP/PS sidecar/index、reference/FAI 全內容 SHA-256"},
        {"stage": "2. Region grouping", "decision": "chr1–22；相鄰 sSNV gap ≤50 kb 的 transitive component",
         "verification": "singleton、多位點 pre-cap、MAX8 retained/excluded 守恆"},
        {"stage": "3. Parallel extraction", "decision": "每樣本 24 workers；每 worker 獨立 indexed genomic handles",
         "verification": "有序 publication；輸入 before/after identity 不變"},
        {"stage": "4. Read evidence", "decision": "MAPQ≥20、BQ≥0；exact alignment identity join；R/A/O/X",
         "verification": "truth fields=0；fallback=0；identity/allele conflict 由 C++ census 明示計數"},
        {"stage": "5. HP family/pattern", "decision": "HP prefix family；full/subread pattern 各自 MINREAD≥3",
         "verification": "patterns.tsv row/key/foreign-key/physical SHA 重播"},
        {"stage": "6. Legacy topology", "decision": "minimum hidden nodes；extra cap=4；level budget=150,000",
         "verification": "region/unit/pattern shared map 逐鍵 mismatch=0"},
        {"stage": "7. Classification", "decision": "determined/ambiguity/recurrence/capped/underdetermined closed census",
         "verification": "C++ cohort 重算 sample 與 cohort 守恆"},
        {"stage": "8. Freeze", "decision": "producer 完成後由不連 solver 的 validator 重播 18 checks",
         "verification": "每樣本 VALIDATED_FROZEN；七份後才產 cohort receipt"},
    ]
    claim_rows = [
        {"claim": "七樣本 C++ bundle", "status": "PASS", "scope": "7/7 independently validated and frozen"},
        {"claim": "Python shared-field crosswalk", "status": "EXACT_RECEIPTS",
         "scope": "regions/units/patterns mismatch=0; crosswalk receipts are not independently frozen"},
        {"claim": "Formal M2 topology", "status": "NOT_CLAIMED", "scope": "Descriptive compatibility endpoint only"},
        {"claim": "CN/LOH parity", "status": "NOT_COMPARED", "scope": "Historical post-tree annotation excluded"},
        {"claim": "Controlled language speedup", "status": "NOT_CLAIMED",
         "scope": f"Python used {python_parallelism_label}; C++ used sequential samples and current cache state"},
    ]

    sources = [
        source("cohort_receipt", "C++ seven-dataset cohort receipt", args.cohort_receipt,
               sha256(args.cohort_receipt), "Validated chart-ready row, class, timing and source bindings."),
        source("regional_input_authority", "Regional physical input authority",
               args.repo_root / "oracle/regional_compat_all_datasets_input_authority.json",
               sha256(args.repo_root / "oracle/regional_compat_all_datasets_input_authority.json"),
               "Full-content SHA-256 lock for seven datasets and eight roles per dataset."),
        source("python_output_authority", "Frozen Python output corpus authority",
               args.repo_root / "oracle/regional_compat_python_v2_output_authority.json",
               sha256(args.repo_root / "oracle/regional_compat_python_v2_output_authority.json"),
               "Exact Python output_manifest and region-view hashes for seven datasets."),
        source("source_port", "Python-to-C++ source provenance",
               args.repo_root / "provenance/source_to_target_manifest.json",
               sha256(args.repo_root / "provenance/source_to_target_manifest.json"),
               "Frozen source digests, C++ targets and transformation descriptions."),
        source("python_lifecycle", "Historical Python lifecycle events",
               args.historical_python_root / "state_events", tree_digest(event_paths),
               f"Frozen six-event timing lifecycle bound to {historical_run_id}; {python_parallelism_label}."),
        source("python_launch_receipt", "Historical Python launch receipt", launch_receipt_path,
               sha256(launch_receipt_path), "Binds full seven-dataset chr1-22 scope and parallelism."),
        source("python_output_manifests", "Historical Python output manifest set", output_manifests_path,
               sha256(output_manifests_path), "Binds the exact seven historical sample manifests."),
        source("python_verification", "Historical Python verification summary", verification_summary_path,
               sha256(verification_summary_path), "Reports all-pass verification for the bound run."),
        source("python_success", "Historical Python success marker", success_marker_path,
               sha256(success_marker_path), "Binds launch and verification digests to the run ID."),
    ]
    for source_id, path, description in hash_sources:
        sources.append(source(source_id, source_id.replace("_", " "), path, sha256(path), description))
    for source_id, path, description in operational_sources:
        sources.append(source(source_id, source_id.replace("_", " "), path, sha256(path), description))

    datasets = {
        "headline": [{
            "datasets": 7,
            "regions": totals["regions"],
            "units": totals["units"],
            "patterns": totals["patterns"],
            "crosswalk_mismatches": 0,
            "cpp_science_minutes": cpp_science_seconds / 60.0,
            "python_science_minutes": python_science / 60.0,
            "observed_science_ratio": observed_science_ratio,
            "one_time_authority_hash_active_sum_minutes": authority_hash_active_seconds / 60.0,
            "one_time_authority_hash_elapsed_minutes": authority_hash_elapsed_seconds / 60.0,
            "cpp_active_wrapper_minutes": cpp_wrapper_seconds / 60.0,
            "cpp_cohort_elapsed_minutes": cohort_elapsed_seconds / 60.0,
            "cohort_source_set_sha256": cohort["source_set_sha256"],
            "cohort_chart_payload_sha256": cohort["chart_payload_sha256"],
        }],
        "authority_hash": authority_hash_rows,
        "sample_counts": sample_counts,
        "sample_timing": sample_timing,
        "region_determinacy": region_rows,
        "unit_classes": unit_rows,
        "provenance": provenance_rows,
        "comparison": comparison_rows,
        "method_steps": method_steps,
        "claim_scope": claim_rows,
    }
    for dataset_id, rows in datasets.items():
        sources.append(materialized_query_source(
            dataset_id,
            rows,
            args.generated_at,
            f"Selects reviewed {dataset_id} rows without BAM/VCF/sidecar access.",
        ))

    charts = [
        {
            "id": "regions_by_sample",
            "title": "各樣本 Python-compatible regions",
            "subtitle": "全 chr1–22；七樣本定義相同，樣本分母不同。",
            "type": "bar",
            "dataset": "sample_counts",
            "sourceId": "query_sample_counts",
            "encodings": {
                "x": {"field": "dataset", "type": "ordinal", "label": "Dataset"},
                "y": {"field": "regions", "type": "quantitative", "label": "Regions", "format": "number"},
                "tooltip": [
                    {"field": "regions", "type": "quantitative", "label": "Regions", "format": "number"},
                    {"field": "units", "type": "quantitative", "label": "Units", "format": "number"},
                    {"field": "patterns", "type": "quantitative", "label": "Patterns", "format": "number"},
                ],
            },
            "yAxisTitle": "Regions",
            "valueFormat": "number",
            "layout": "full",
        },
        {
            "id": "science_time_by_sample",
            "title": "各樣本 C++ science wall time",
            "subtitle": "每樣本 24 workers、樣本間序列執行；不含獨立 validator。",
            "type": "bar",
            "dataset": "sample_timing",
            "sourceId": "query_sample_timing",
            "encodings": {
                "x": {"field": "dataset", "type": "ordinal", "label": "Dataset"},
                "y": {"field": "science_minutes", "type": "quantitative", "label": "Minutes", "format": "number"},
                "tooltip": [
                    {"field": "science_minutes", "type": "quantitative", "label": "Science min", "format": "number"},
                    {"field": "input_sha256_minutes", "type": "quantitative", "label": "Input hash min", "format": "number"},
                    {"field": "validator_minutes", "type": "quantitative", "label": "Validator min", "format": "number"},
                ],
            },
            "yAxisTitle": "Minutes",
            "valueFormat": "number",
            "layout": "full",
        },
    ]

    def columns(*rows: tuple[str, str, str]) -> list[dict[str, str]]:
        output = []
        for field, label, kind in rows:
            column = {"field": field, "label": label}
            if kind == "text":
                column["type"] = "text"
            else:
                column["format"] = kind
            output.append(column)
        return output

    tables = [
        {"id": "counts", "title": "七樣本輸出計數", "subtitle": "Mismatch 是三層 crosswalk 合計。",
         "dataset": "sample_counts", "sourceId": "query_sample_counts",
         "defaultSort": {"field": "dataset", "direction": "asc"},
         "columns": columns(("dataset", "Dataset", "text"), ("regions", "Regions", "number"),
                            ("units", "Units", "number"), ("patterns", "Patterns", "number"),
                            ("crosswalk_mismatches", "Mismatch", "number"))},
        {"id": "authority_hash", "title": "一次性 raw BAM authority hash",
         "subtitle": "HCC1395沿用既有full-content authority；其餘六份本輪實算，最後三份依低負載 telemetry 並行。",
         "dataset": "authority_hash", "sourceId": "query_authority_hash",
         "defaultSort": {"field": "dataset", "direction": "asc"},
         "columns": columns(("dataset", "Dataset", "text"),
                            ("size_gb_decimal", "Raw BAM GB", "number"),
                            ("minutes", "Hash min", "number"),
                            ("mbps_decimal", "MB/s", "number"),
                            ("authority_source", "Authority source", "text"))},
        {"id": "timing", "title": "逐樣本時間與 bottleneck 指標",
         "subtitle": "summed worker time 不是 wall time；可用來判斷 I/O 與 solver 相對成本。",
         "dataset": "sample_timing", "sourceId": "query_sample_timing",
         "defaultSort": {"field": "science_minutes", "direction": "desc"},
         "columns": columns(("dataset", "Dataset", "text"), ("workers", "Workers", "number"),
                            ("wrapper_minutes", "E2E min", "number"),
                            ("input_sha256_minutes", "Hash min", "number"),
                            ("science_minutes", "Science min", "number"),
                            ("validator_minutes", "Validator min", "number"),
                            ("summed_input_worker_minutes", "Σ input worker min", "number"),
                            ("summed_solver_worker_minutes", "Σ solver worker min", "number"))},
        {"id": "comparison", "title": "歷史 Python 與本次 C++ lifecycle 時間",
         "subtitle": "並行策略與 cache 條件不同；只展示實測 interval，不宣稱 controlled speedup。",
         "dataset": "comparison", "sourceId": "query_comparison",
         "defaultSort": {"field": "implementation", "direction": "asc"},
         "columns": columns(("implementation", "Implementation", "text"), ("stage", "Stage", "text"),
                            ("minutes", "Minutes", "number"), ("parallelism", "Parallelism", "text"),
                            ("scope_note", "Scope", "text"))},
        {"id": "regions", "title": "Region determinacy closed census",
         "subtitle": "每個樣本五類互斥且總和等於 regions。",
         "dataset": "region_determinacy", "sourceId": "query_region_determinacy",
         "defaultSort": {"field": "dataset", "direction": "asc"},
         "columns": columns(("dataset", "Dataset", "text"), ("class", "Class", "text"),
                            ("count", "Count", "number"), ("denominator", "Regions", "number"))},
        {"id": "units", "title": "Unit class closed census",
         "subtitle": "all units 與 primary mutation-bearing units 分列。",
         "dataset": "unit_classes", "sourceId": "query_unit_classes",
         "defaultSort": {"field": "dataset", "direction": "asc"},
         "columns": columns(("dataset", "Dataset", "text"), ("cohort", "Cohort", "text"),
                            ("class", "Class", "text"), ("count", "Count", "number"))},
        {"id": "method", "title": "從 authority 到 FROZEN 的完整流程",
         "subtitle": "每一步都有具體決策與可重播驗證。", "dataset": "method_steps",
         "sourceId": "query_method_steps", "defaultSort": {"field": "stage", "direction": "asc"},
         "columns": columns(("stage", "Stage", "text"), ("decision", "Decision", "text"),
                            ("verification", "Verification", "text"))},
        {"id": "provenance", "title": "逐樣本 frozen provenance",
         "subtitle": "完整 SHA-256；報告未隱藏或以檔名代替內容鎖。", "dataset": "provenance",
         "sourceId": "query_provenance", "defaultSort": {"field": "dataset", "direction": "asc"},
         "columns": columns(("dataset", "Dataset", "text"),
                            ("source_manifest_sha256", "Source manifest", "text"),
                            ("validator_executable_sha256", "Validator binary", "text"),
                            ("validation_receipt_sha256", "Validator receipt", "text"),
                            ("frozen_marker_sha256", "FROZEN", "text"),
                            ("crosswalk_receipt_sha256", "Crosswalk", "text"),
                            ("python_manifest_sha256", "Python manifest", "text"),
                            ("python_region_view_sha256", "Python region view", "text"))},
        {"id": "claims", "title": "可以與不可以下的結論", "subtitle": "避免把 descriptive endpoint 擴張為 formal topology。",
         "dataset": "claim_scope", "sourceId": "query_claim_scope",
         "defaultSort": {"field": "claim", "direction": "asc"},
         "columns": columns(("claim", "Claim", "text"), ("status", "Status", "text"),
                            ("scope", "Scope", "text"))},
    ]

    blocks = [
        {"id": "answer", "type": "markdown", "sourceId": "cohort_receipt",
         "body": f"## 七樣本已完整跑完並各自 freeze；共同欄位逐鍵 mismatch=0\n\n"
                 f"C++ indexed-I/O 共輸出 **{totals['regions']:,} regions、{totals['units']:,} units、"
                 f"{totals['patterns']:,} patterns**。七份 bundle 都經獨立 fail-closed validator 後才產生 FROZEN；"
                 "七份 crosswalk receipt 均回報 region/unit/pattern shared fields exact。"
                 "Crosswalk receipt 本身尚未由第二套獨立程式 freeze，因此結論限定為 descriptive Python compatibility，"
                 "不提升為 formal M2 topology 或 production release。"},
        {"id": "counts_chart", "type": "chart", "chartId": "regions_by_sample", "layout": "full"},
        {"id": "counts_table", "type": "table", "tableId": "counts", "layout": "full"},
        {"id": "authority_hash_answer", "type": "markdown", "sourceId": "raw_bam_hash_batch",
         "body": f"## 首次 frozen provenance：active sum {authority_hash_active_seconds / 60.0:.2f} 分鐘，"
                 f"實際 elapsed {authority_hash_elapsed_seconds / 60.0:.2f} 分鐘\n\n"
                 "HCC1395已有全內容authority；其餘六份raw BAM以低I/O優先權建立SHA-256。前段序列執行；"
                 "telemetry確認CPU idle約95%、I/O wait約2%後，最後三份改為3-way nice/ionice，並保留identity gate。"
                 "這是首次建立authority的成本，不是grouping、pileup或solver時間；正式producer仍會再從file descriptor"
                 "重算所選八個input SHA，防止只信任外部文字receipt。"},
        {"id": "authority_hash_table", "type": "table", "tableId": "authority_hash", "layout": "full"},
        {"id": "time_answer", "type": "markdown", "sourceId": "python_lifecycle",
         "body": f"## C++ science 實測較短，但不是嚴格 controlled speedup\n\n"
                 f"歷史 Python RUNNING→VERIFYING 為 **{python_science / 60.0:.2f} 分鐘**，本次七樣本 C++ "
                 f"sequential science wall 合計 **{cpp_science_seconds / 60.0:.2f} 分鐘**，表面比值 "
                 f"**{observed_science_ratio:.3f}×**。Python authority 記錄 **{python_parallelism_label}**；C++ 是樣本間序列、"
                 f"樣本內24 workers。七份wrapper active sum為 **{cpp_wrapper_seconds / 60.0:.2f}分鐘**，outer batch 真實"
                 f"elapsed為 **{cohort_elapsed_seconds / 60.0:.2f}分鐘**。NFS cache/系統負載不同，因此以上是"
                 "operational observation，不是語言本身的受控加速倍數。"},
        {"id": "time_chart", "type": "chart", "chartId": "science_time_by_sample", "layout": "full"},
        {"id": "timing_table", "type": "table", "tableId": "timing", "layout": "full"},
        {"id": "comparison_table", "type": "table", "tableId": "comparison", "layout": "full"},
        {"id": "method_text", "type": "markdown", "sourceId": "source_port",
         "body": "## 科學判斷核心全部在 C++；Python 只做本頁呈現\n\n"
                 "50 kb transitive grouping、MAX8、compiled indexed pileup、exact HP/PS sidecar join、R/A/O/X allele state、"
                 "HP family、MINREAD=3、hidden-node solver、closed classification、cohort totals 與 chart payload digest 都由 C++ 產生。"
                 "本報告程式先以本地 fail-closed Draft 2020-12 engine 驗完整 repo-pinned cohort schema，並精確重播 C++ 的"
                 "source-set/chart-payload canonical digest，再只建立表格/圖表；它不開 BAM、VCF 或 sidecar，"
                 "也不重算任何科學分類。"},
        {"id": "method_table", "type": "table", "tableId": "method", "layout": "full"},
        {"id": "region_table", "type": "table", "tableId": "regions", "layout": "full"},
        {"id": "unit_table", "type": "table", "tableId": "units", "layout": "full"},
        {"id": "trust", "type": "markdown", "sourceId": "regional_input_authority",
         "body": "## Frozen provenance 綁定輸入、source contract 與輸出\n\n"
                 "輸入 authority 鎖定七樣本各八個角色的 size 與 full-content SHA-256；manifest 同時綁定 science parameters、"
                 "schema、registry、release attestation 與 Python-to-C++ source manifest。Batch wrapper 鎖定 producer/validator"
                 "可執行檔的 canonical path、執行前後穩定 SHA，並重驗 source manifest bytes/identity；其中 validator SHA 也逐樣本 receipt-bound。"
                 "Crosswalk wrapper 另證明前後使用同一份 producer executable。Producer 完成後 validator 另外鎖定並"
                 "獨立重播 source manifest，再執行 closed shape、physical checksum、summary/receipt contract、TSV census、"
                 "foreign key、authority 與 TOCTOU stable 等18項checks；"
                 "cohort receipt 再重播七份 FROZEN/crosswalk/source authority。"},
        {"id": "provenance_table", "type": "table", "tableId": "provenance", "layout": "full"},
        {"id": "claims_table", "type": "table", "tableId": "claims", "layout": "full"},
        {"id": "limitations", "type": "markdown",
         "body": "## 限制與後續 gate\n\n"
                 "- CN/LOH 是舊 Python post-tree annotation，本次只比較 region/unit/pattern shared fields；unavailable 不等於 neutral。\n"
                 "- Crosswalk 是同一 C++ compatibility executable 產出的 receipt；雖有 frozen Python corpus SHA 綁定與 mismatch=0，"
                 "wrapper 會重驗 executable SHA，但它沒有第二支獨立 validator，故仍誠實標記 `independently_frozen=false`。\n"
                 "- `summed_input_seconds`/`summed_solver_seconds` 是 worker 累積時間，不可當 wall time。\n"
                 "- Validator binary SHA 同時由 batch 與七份 sample receipt 綁定；producer binary SHA 僅由 batch wrapper 前後穩定性綁定，"
                 "未寫入各 sample receipt，因此只稱 batch-execution provenance，不稱 sample-receipt-bound 或獨立freeze。\n"
                 "- 歷史 Python 與本次 C++ 的並行、cache、系統負載不同；正式 controlled benchmark 仍需隔離重跑。\n"
                 "- 此端點保留 evaluation/descriptive claim ceiling，不取代 LongLineage formal production pipeline。"},
    ]

    artifact = {
        "surface": "report",
        "manifest": {
            "version": 1,
            "surface": "report",
            "title": TITLE,
            "description": "Seven-dataset full C++ indexed-I/O Python-compatible regional sSNV topology validation report.",
            "generatedAt": args.generated_at,
            "cards": [],
            "charts": charts,
            "tables": tables,
            "sources": sources,
            "blocks": blocks,
        },
        "snapshot": {
            "version": 1,
            "generatedAt": args.generated_at,
            "status": "ready",
            "datasets": datasets | {
                "report_notes": [{
                    "audience": "technical",
                    "delivery_mode": "portable_html",
                    "chart_map": "sample/regions and sample/science-minutes; zero-based bars",
                    "python_boundary": "Presentation only; no scientific input is opened or recomputed.",
                    "comparison_ceiling": "Operational timing observation; controlled speedup claim prohibited.",
                }],
            },
        },
        "sources": sources,
        "package_info": {
            "originUrl": "artifact://longlineage/seven-dataset-python-compatible-regional-topology",
            "controls": {"edit": False, "refresh": False, "share": False},
        },
    }
    write_new_atomic(args.output, json.dumps(artifact, ensure_ascii=False, indent=2) + "\n")
    print(f"PASS REGIONAL_COMPAT_COHORT_REPORT output={args.output}")
    print(f"datasets=7 regions={totals['regions']} units={totals['units']} patterns={totals['patterns']} mismatches=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
