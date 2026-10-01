#!/usr/bin/env python3
"""Deterministic regressions for schema accuracy, typed failures, and runtime bounds."""

import argparse
import csv
import io
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def literal(value):
    return "'" + value.replace("'", "''") + "'"


class RegressionProvider(BaseHTTPRequestHandler):
    requests = []

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["content-length"])))
        self.requests.append(request)
        if self.path == "/chat/completions":
            prompt = request["messages"][-1]["content"]
            if "return JSON:" in prompt:
                content = prompt.split("return JSON:", 1)[1]
            elif "alpha item" in prompt:
                content = "alpha"
            elif "beta item" in prompt:
                content = "beta"
            else:
                content = "invalid typed response"
            response = {
                "choices": [{"message": {"content": content}}],
                "usage": {"prompt_tokens": 2, "completion_tokens": 1, "total_tokens": 3},
            }
        elif self.path == "/embeddings":
            inputs = request["input"]
            inputs = inputs if isinstance(inputs, list) else [inputs]

            def coordinates(text):
                if "huge positive" in text:
                    return [1e308, 1e308]
                if "huge negative" in text:
                    return [-1e308, -1e308]
                if "tiny" in text:
                    return [1e-300, 1e-300]
                if "zero" in text:
                    return [0.0, 0.0]
                if "precision" in text:
                    return [0.12345678901234566, -0.9876543210987654]
                return [1.0, 0.0] if "alpha" in text else [0.0, 1.0]

            response = {
                "data": [{"index": i, "embedding": coordinates(text)} for i, text in enumerate(inputs)],
                "usage": {"prompt_tokens": len(inputs), "total_tokens": len(inputs)},
            }
        else:
            self.send_error(404)
            return
        encoded = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, *_):
        pass


def run(duckdb_path):
    server = ThreadingHTTPServer(("127.0.0.1", 0), RegressionProvider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_port}"
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "regression-key"
    setup = f"""
        SET threads = 1;
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'regression-model';
        SET duckdb_ai_task_model = 'regression-model';
        SET duckdb_ai_embedding_model = 'regression-embedding';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
    """
    checks = 0

    def check(sql, expected, name):
        nonlocal checks
        result = subprocess.run(
            [str(duckdb_path), "-batch", "-bail", "-csv", "-noheader", "-init", os.devnull],
            input=setup + sql,
            env=env,
            text=True,
            capture_output=True,
            timeout=30,
        )
        rows = list(csv.reader(io.StringIO(result.stdout)))
        if result.returncode or rows != expected:
            raise AssertionError(f"{name}: expected {expected}, got {rows}\n{result.stderr}")
        checks += 1

    def schema_check(response, schema, valid, name):
        payload = json.dumps(schema, ensure_ascii=False, separators=(",", ":"))
        check(
            f"SELECT ai_complete_json({literal('return JSON:' + response)}, "
            f"response_schema := {literal(payload)}, fail_on_error := false) IS NOT NULL;",
            [[str(valid).lower()]],
            name,
        )

    try:
        for value, valid in (("null", True), ('"allowed"', True), ("42", False), ("false", False)):
            schema_check(
                '{"value":' + value + "}",
                {"properties": {"value": {"type": ["string", "null"]}}},
                valid,
                "type union " + value,
            )
        for text in ("é", "😀", "中"):
            schema_check(
                json.dumps({"value": text}),
                {"properties": {"value": {"minLength": 1, "maxLength": 1}}},
                True,
                "Unicode code point " + text,
            )
            schema_check(
                json.dumps({"value": text}),
                {"properties": {"value": {"minLength": 2}}},
                False,
                "Unicode minimum " + text,
            )
        schema_check(
            '{"value":"é"}',
            {"properties": {"value": {"minLength": 2, "maxLength": 2}}},
            True,
            "combining mark counts separately",
        )
        schema_check(
            '{"value":"short"}',
            {"properties": {"value": {"minLength": 9223372036854775807}}},
            False,
            "largest signed schema length",
        )
        for schema in (
            {"properties": {"value": False}},
            {"not": True},
            {"propertyNames": False},
            {"allOf": [False]},
        ):
            schema_check('{"value":1}', schema, False, "nested false schema")
        schema_check("[1]", {"items": False}, False, "false items schema")
        schema_check("[]", {"items": False}, True, "empty array with false items")
        schema_check("[1]", {"contains": False}, False, "false contains schema")
        schema_check("[1]", {"contains": True}, True, "true contains schema")
        schema_check("[9007199254740992,9007199254740993]", {"uniqueItems": True}, True, "distinct large integers")
        schema_check("[9007199254740992,9007199254740992.0]", {"uniqueItems": True}, False, "equal integer and real")
        schema_check(
            '{"value":9007199254740993}',
            {"properties": {"value": {"enum": [9007199254740992]}}},
            False,
            "exact large integer enum",
        )
        for keyword, boundary, valid in (
            ("minimum", 9007199254740993, False),
            ("maximum", 9007199254740991, False),
            ("exclusiveMinimum", 9007199254740991, True),
            ("exclusiveMaximum", 9007199254740993, True),
        ):
            schema_check(
                '{"value":9007199254740992}',
                {"properties": {"value": {keyword: boundary}}},
                valid,
                "exact bound " + keyword,
            )
        schema_check(
            '{"value":18446744073709551615}',
            {"properties": {"value": {"exclusiveMaximum": 18446744073709551616.0}}},
            True,
            "unsigned integer versus real boundary",
        )
        for number, valid in ((9007199254740993, False), (-9007199254740993, False), (18446744073709551614, True)):
            schema_check(
                '{"value":' + str(number) + "}",
                {"properties": {"value": {"multipleOf": 2}}},
                valid,
                "exact integer multipleOf",
            )

        record_schema = literal('{"type":"object","properties":{"value":{"type":"integer"}}}')
        for number in (9007199254740993, 9223372036854775807, -9223372036854775808):
            prompt = literal('return JSON:{"value":' + str(number) + "}")
            check(
                f"SELECT value FROM ai_complete_record({prompt}, {record_schema});",
                [[str(number)]],
                "exact table BIGINT",
            )
            check(f"SELECT ai_extract_record({prompt}, {record_schema}).value;", [[str(number)]], "exact scalar BIGINT")
        for number in (9223372036854775808, 18446744073709551615, -9223372036854775809):
            prompt = literal('return JSON:{"value":' + str(number) + "}")
            check(
                f"SELECT value IS NULL FROM ai_complete_record({prompt}, {record_schema}, fail_on_error := false);"
                f"SELECT ai_extract_record({prompt}, {record_schema}, fail_on_error := false) IS NULL;",
                [["true"], ["true"]],
                "out of range record obeys error policy",
            )

        invalid_functions = (
            "ai_complete_json('bad response', fail_on_error := false, cache := true)",
            f"ai_extract_record('bad response', {record_schema}, fail_on_error := false, cache := true)",
            "ai_classify('bad response', ['alpha', 'beta'], fail_on_error := false, cache := true)",
            "ai_classify_labels('bad response', ['alpha', 'beta'], fail_on_error := false, cache := true)",
            "ai_classify_result('bad response', ['alpha', 'beta'], cache := true).error",
            "ai_filter('bad response', 'is good', fail_on_error := false, cache := true)",
            "ai_rerank('bad response', 'candidate', fail_on_error := false, cache := true)",
            "ai_score('bad response', 'criteria', fail_on_error := false, cache := true)",
        )
        for function in invalid_functions:
            before = len(RegressionProvider.requests)
            expected_null = "false" if function.endswith(".error") else "true"
            check(
                f"SELECT {function} IS NULL; SELECT {function} IS NULL;"
                "SELECT count(*), count(*) FILTER (WHERE status = 'error') FROM ai_usage();",
                [[expected_null], [expected_null], ["2", "2"]],
                "invalid response usage and eviction: " + function,
            )
            if len(RegressionProvider.requests) != before + 2:
                raise AssertionError("invalid typed response was cached: " + function)
        before = len(RegressionProvider.requests)
        check(
            f"SELECT value IS NULL FROM ai_complete_record('bad response', {record_schema}, "
            "fail_on_error := false, cache := true);"
            f"SELECT value IS NULL FROM ai_complete_record('bad response', {record_schema}, "
            "fail_on_error := false, cache := true);"
            "SELECT count(*), count(*) FILTER (WHERE status = 'error') FROM ai_usage();",
            [["true"], ["true"], ["2", "2"]],
            "invalid table response usage and eviction",
        )
        if len(RegressionProvider.requests) != before + 2:
            raise AssertionError("invalid table response was cached")

        for left, right, expected in (
            ("huge positive", "huge positive other", 1),
            ("huge positive", "huge negative", -1),
            ("tiny", "tiny other", 1),
            ("huge positive", "tiny", 1),
        ):
            check(
                f"SELECT abs(ai_similarity({literal(left)}, {literal(right)}) - ({expected})) < 1e-14;",
                [["true"]],
                "stable cosine " + left + "/" + right,
            )
        check("SELECT ai_similarity('zero', 'tiny', fail_on_error := false) IS NULL;", [["true"]], "zero norm")
        check(
            "CREATE TEMP TABLE packed AS SELECT ai_embed(text, cache := true) AS embedding "
            "FROM (VALUES ('precision one'), ('precision two')) t(text);"
            "SELECT ai_embed('precision one', cache := true) = [0.12345678901234566, -0.9876543210987654];"
            "SELECT count(*) FROM ai_usage() WHERE cache_hit;",
            [["true"], ["1"]],
            "cached embedding precision",
        )

        check(
            "CREATE TEMP TABLE training AS SELECT CASE WHEN i IN (0, 5) THEN 'alpha item ' ELSE 'beta item ' END "
            "|| i AS text FROM range(10) t(i);"
            "CREATE TEMP TABLE classifier AS SELECT ai_build_classifier(text, ['alpha', 'beta'], "
            "optimization := 'minimize_cost', quality_threshold := 1) AS artifact FROM training;"
            "SELECT regexp_extract(artifact, '\"usable\":(true|false)', 1), regexp_extract(artifact, '\"accuracy\":([0-9.]+)', 1) FROM classifier;",
            [["true", "1"]],
            "sparse class retains training examples",
        )
        check(
            "CREATE TEMP TABLE training AS SELECT text FROM (VALUES ('alpha item 1'), ('beta item 1')) t(text);"
            "CREATE TEMP TABLE classifier AS SELECT ai_build_classifier(text, ['alpha', 'beta'], "
            "optimization := 'minimize_cost', quality_threshold := 0) AS artifact FROM training;"
            "SELECT regexp_extract(artifact, '\"usable\":(true|false)', 1), regexp_extract(artifact, '\"validation_count\":([0-9]+)', 1) "
            "FROM classifier;",
            [["false", "0"]],
            "unvalidated artifact cannot be usable",
        )
        check(
            "SELECT sum(ai_embed('alpha item ' || i)[1]) FROM range(1500) t(i);"
            "SELECT count(*), min(event_id), max(event_id), bool_and(status = 'ok') FROM ai_usage();"
            "SELECT max(retained_events), max(dropped_events) FROM ai_usage_summary();",
            [["1500.0"], ["1024", "477", "1500", "true"], ["1024", "476"]],
            "bounded usage FIFO",
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print(f"repository regression smoke passed ({checks} checks)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, default=Path(__file__).resolve().parents[2] / "build/release/duckdb")
    run(parser.parse_args().duckdb)
