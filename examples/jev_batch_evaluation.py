#!/usr/bin/env python3
"""Compare bounded Jev row-evaluation batch sizes on a labeled CSV."""

import argparse
import csv
import hashlib
import io
import json
import math
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


MAX_ROWS = 1000
BATCH_SIZES = (1, 8, 16, 32)


def fail(message):
    raise ValueError(message)


def load_input(path, criteria):
    raw = path.read_bytes()
    rows = []
    seen = set()
    with io.StringIO(raw.decode("utf-8-sig"), newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["id", "text", "label"]:
            fail("input CSV must have exactly id,text,label columns")
        for line, row in enumerate(reader, 2):
            if len(rows) >= MAX_ROWS:
                fail(f"input exceeds the {MAX_ROWS}-row cap")
            if None in row:
                fail(f"row {line} has extra CSV fields")
            identifier = row.get("id")
            text = row.get("text")
            label = row.get("label")
            if identifier is None or not identifier:
                fail(f"row {line} has an empty id")
            if identifier != identifier.strip():
                fail(f"row {line} has whitespace around its id")
            if identifier in seen:
                fail(f"duplicate id at row {line}: {identifier}")
            if text is None or not text.strip():
                fail(f"row {line} has null or empty text")
            if label is None or label not in criteria:
                fail(f"row {line} has an unknown label: {label!r}")
            seen.add(identifier)
            rows.append({"id": identifier, "text": text, "label": label})
    if not rows:
        fail("input CSV has no rows")
    return rows, hashlib.sha256(raw).hexdigest()


def load_criteria(path):
    def reject_duplicate_labels(pairs):
        result = {}
        for label, description in pairs:
            if label in result:
                raise ValueError(f"duplicate criteria label: {label}")
            result[label] = description
        return result

    try:
        criteria = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=reject_duplicate_labels)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        fail(f"invalid criteria JSON: {error}")
    if not isinstance(criteria, dict) or not criteria:
        fail("criteria JSON must be a non-empty label-to-description object")
    if set(criteria) == {"true", "false"}:
        fail("criteria {'true', 'false'} is a Noul; use a Choice label map instead")
    if len(criteria) > 255:
        fail("criteria has more than 255 labels")
    for label, description in criteria.items():
        if not isinstance(label, str) or not label:
            fail("criteria labels must be non-empty strings")
        if not isinstance(description, str) or not description:
            fail(f"criteria description for {label!r} must be a non-empty string")
    canonical = json.dumps(criteria, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return criteria, hashlib.sha256(canonical.encode()).hexdigest()


def sql_string(value):
    return "'" + value.replace("'", "''") + "'"


def criteria_sql(criteria):
    pairs = ", ".join(f"{sql_string(label)}: {sql_string(criteria[label])}" for label in sorted(criteria))
    return "MAP {" + pairs + "}"


def child_env():
    # Keep provider credentials and endpoint configuration, but never inherit
    # the optional outbound usage-log endpoint or payload settings.
    env = os.environ.copy()
    for name in tuple(env):
        if name.startswith("DUCKDB_AI_LOG_"):
            env.pop(name, None)
    return env


def parse_json_stream(stdout):
    decoder = json.JSONDecoder()
    values = []
    remaining = stdout.strip()
    while remaining:
        value, end = decoder.raw_decode(remaining)
        values.append(value)
        remaining = remaining[end:].lstrip()
    return values


def safe_diagnostic(detail):
    for name, value in sorted(os.environ.items(), key=lambda item: len(item[1]), reverse=True):
        if value and any(word in name.upper() for word in ("KEY", "TOKEN", "SECRET", "PASSWORD")):
            detail = detail.replace(value, "[redacted]")
    return next((line.strip()[:500] for line in detail.splitlines() if line.strip()), "no diagnostic available")


def run_batch(duckdb, input_path, criteria, model, batch_size, expected_rows, timeout_seconds):
    map_sql = criteria_sql(criteria)
    source = sql_string(str(input_path))
    query = f"""
LOAD ai;
SET threads=1;
CREATE TEMP TABLE evaluated AS
SELECT row_number() OVER () AS input_order, id::VARCHAR AS id, text::VARCHAR AS text,
       label::VARCHAR AS label,
       ai_jev(text::VARCHAR, {{prediction: {map_sql}}},
              model := {sql_string(model)}, batch_size := {batch_size},
              cache := false, retry_count := 0, max_concurrent_requests := 1,
              on_error := 'null') AS decision
FROM read_csv({source}, auto_detect=false, header=true, delim=',', quote='"', escape='"', columns={{id: 'VARCHAR', text: 'VARCHAR', label: 'VARCHAR'}});
SELECT id, text, label, decision.prediction AS prediction,
       decision IS NULL AS failed
FROM evaluated ORDER BY input_order;
WITH usage AS (SELECT * FROM ai_usage() WHERE function_name = 'ai_jev')
SELECT COALESCE((SELECT sum(batch_count) FROM ai_usage_summary() WHERE provider = 'typesafe'), 0)::BIGINT
           AS request_operations,
       COALESCE((SELECT max(dropped_events) FROM ai_usage_summary()), 0)::BIGINT AS dropped_events,
       count(*)::BIGINT AS usage_events,
       count(*) FILTER (WHERE status = 'ok' AND prompt_tokens >= 0)::BIGINT AS prompt_token_events,
       count(*) FILTER (WHERE status = 'ok' AND completion_tokens >= 0)::BIGINT AS completion_token_events,
       count(*) FILTER (WHERE status = 'ok' AND total_tokens >= 0)::BIGINT AS total_token_events,
       CASE WHEN count(*) FILTER (WHERE status != 'ok' OR prompt_tokens < 0) > 0 THEN NULL
            ELSE sum(prompt_tokens)::BIGINT END AS prompt_tokens,
       CASE WHEN count(*) FILTER (WHERE status != 'ok' OR completion_tokens < 0) > 0 THEN NULL
            ELSE sum(completion_tokens)::BIGINT END AS completion_tokens,
       CASE WHEN count(*) FILTER (WHERE status != 'ok' OR total_tokens < 0) > 0 THEN NULL
            ELSE sum(total_tokens)::BIGINT END AS total_tokens,
       count(*) FILTER (WHERE status = 'error')::BIGINT AS failures,
       count(*) > 0 AND count(*) FILTER (WHERE status != 'ok' OR prompt_tokens < 0 OR completion_tokens < 0 OR total_tokens < 0) = 0
           AS tokens_complete
FROM usage;
"""
    started = time.perf_counter()
    result = subprocess.run(
        [str(duckdb), "-unsigned", "-batch", "-bail", "-json", "-init", os.devnull],
        input=query,
        text=True,
        capture_output=True,
        env=child_env(),
        timeout=timeout_seconds,
    )
    elapsed_ms = round((time.perf_counter() - started) * 1000, 3)
    if result.returncode:
        raise RuntimeError("DuckDB evaluation failed: " + safe_diagnostic(result.stderr))
    values = parse_json_stream(result.stdout)
    if len(values) != 2:
        raise RuntimeError("DuckDB evaluation returned an unexpected result shape")
    rows, usage_rows = values
    observed_rows = [{field: row.get(field) for field in ("id", "text", "label")} for row in rows]
    if observed_rows != expected_rows:
        raise RuntimeError("DuckDB parsed the input differently from the validator")
    usage = usage_rows[0] if usage_rows else {}
    if not usage.get("usage_events") or not usage.get("request_operations"):
        raise RuntimeError("no provider requests were recorded; check TYPESAFE_API_KEY and request size")
    if usage.get("dropped_events", 0):
        usage["tokens_complete"] = False
        for field in ("prompt_tokens", "completion_tokens", "total_tokens"):
            usage[field] = None
    predictions = []
    for row in rows:
        predictions.append(
            {
                "id": row["id"],
                "text": row["text"],
                "label": row["label"],
                "prediction": row["prediction"],
                "failed": bool(row["failed"]),
            }
        )
    correct = sum(row["prediction"] == row["label"] for row in predictions)
    failed = sum(row["failed"] for row in predictions)
    total = len(predictions)
    accuracy = correct / total if total else None
    return {
        "batch_size": batch_size,
        "elapsed_query_wall_ms": elapsed_ms,
        "metrics": {
            "rows": total,
            "correct": correct,
            "failed": failed,
            "accuracy": accuracy,
            "accuracy_denominator": total,
            "failures_in_denominator": failed,
        },
        "usage": {
            "request_operations": usage.get("request_operations"),
            "prompt_tokens": usage.get("prompt_tokens"),
            "completion_tokens": usage.get("completion_tokens"),
            "total_tokens": usage.get("total_tokens"),
            "failures": usage.get("failures"),
            "usage_events": usage.get("usage_events"),
            "dropped_events": usage.get("dropped_events"),
            "prompt_token_events": usage.get("prompt_token_events"),
            "completion_token_events": usage.get("completion_token_events"),
            "total_token_events": usage.get("total_token_events"),
            "tokens_complete": usage.get("tokens_complete"),
        },
        "rows": predictions,
    }


def agreement(baseline, candidate):
    left = {row["id"]: row for row in baseline["rows"]}
    right = {row["id"]: row for row in candidate["rows"]}
    comparable = 0
    agreed = 0
    for identifier in left:
        a, b = left[identifier]["prediction"], right[identifier]["prediction"]
        if a is not None and b is not None:
            comparable += 1
            agreed += a == b
    return {
        "compared_non_null_predictions": comparable,
        "agreed": agreed,
        "agreement": agreed / comparable if comparable else None,
        "baseline_coverage": sum(row["prediction"] is not None for row in baseline["rows"]),
        "candidate_coverage": sum(row["prediction"] is not None for row in candidate["rows"]),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, default=Path("build/release/duckdb"))
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--criteria", required=True, type=Path)
    parser.add_argument("--model", required=True)
    parser.add_argument(
        "--timeout-seconds", type=float, default=3600, help="timeout for each batch-size run (default: 3600)"
    )
    parser.add_argument("--allow-live", action="store_true", help="explicitly permit provider calls")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be a finite positive number")
    if not args.allow_live:
        parser.error("refusing provider calls without --allow-live")
    if not args.model.strip():
        parser.error("--model must not be empty")
    if not args.duckdb.is_file():
        parser.error(f"DuckDB executable not found: {args.duckdb}")
    criteria, criteria_sha256 = load_criteria(args.criteria)
    rows, input_sha256 = load_input(args.input, criteria)
    results = []
    report = {
        "model": args.model,
        "input_sha256": input_sha256,
        "criteria_sha256": criteria_sha256,
        "row_cap": MAX_ROWS,
        "rows": len(rows),
        "batch_sizes": list(BATCH_SIZES),
        "timeout_seconds": args.timeout_seconds,
        "complete": False,
        "runs": results,
    }
    with tempfile.TemporaryDirectory(prefix="duckdb-ai-jev-eval-") as directory:
        # Use a fixed CSV dialect and verify every returned row against this snapshot.
        input_path = Path(directory) / "input.csv"
        with input_path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=["id", "text", "label"])
            writer.writeheader()
            writer.writerows(rows)
        for batch_size in BATCH_SIZES:
            try:
                result = run_batch(
                    args.duckdb.resolve(), input_path, criteria, args.model, batch_size, rows, args.timeout_seconds
                )
                result["agreement_to_batch_1"] = agreement(results[0] if results else result, result)
                results.append(result)
            except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
                detail = "DuckDB evaluation timed out" if isinstance(error, subprocess.TimeoutExpired) else str(error)
                report["error"] = safe_diagnostic(detail)
                json.dump(report, sys.stdout, ensure_ascii=False)
                sys.stdout.write("\n")
                raise RuntimeError(report["error"]) from None
    report["complete"] = True
    json.dump(report, sys.stdout, ensure_ascii=False, separators=(",", ":"))
    sys.stdout.write("\n")


if __name__ == "__main__":
    try:
        main()
    except subprocess.TimeoutExpired:
        print("jev batch evaluation failed: DuckDB evaluation timed out", file=sys.stderr)
        raise SystemExit(2)
    except (OSError, ValueError, RuntimeError, csv.Error) as error:
        print(f"jev batch evaluation failed: {error}", file=sys.stderr)
        raise SystemExit(2)
