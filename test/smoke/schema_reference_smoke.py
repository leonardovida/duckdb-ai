#!/usr/bin/env python3
"""Exercise local JSON Schema references through public SQL and a local provider."""

import argparse
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def sql_string(value):
    return "'" + value.replace("'", "''") + "'"


def schema_cases():
    cases = []

    def add(name, schema, value, valid, error=None):
        cases.append((name, schema, value, valid, error))

    def wrapped(field):
        return {"type": "object", "properties": {"x": field}, "required": ["x"]}

    # Inline controls establish that each constraint already works without references.
    for name, constraint, bad, good in [
        ("minimum", {"type": "integer", "minimum": 5}, 4, 5),
        ("minLength", {"type": "string", "minLength": 3}, "ab", "abc"),
        ("type", {"type": "integer"}, "5", 5),
        ("required", {"type": "object", "required": ["a"]}, {}, {"a": 1}),
    ]:
        add("inline_" + name, wrapped(constraint), {"x": bad}, False)
        schema = wrapped({"$ref": "#/$defs/value"})
        schema["$defs"] = {"value": constraint}
        add("referenced_" + name + "_invalid", schema, {"x": bad}, False)
        add("referenced_" + name + "_valid", schema, {"x": good}, True)

    root = {
        "$defs": {"record": wrapped({"type": "integer", "minimum": 5})},
        "$ref": "#/$defs/record",
    }
    add("root_ref_invalid", root, {"x": 4}, False)
    add("root_ref_valid", root, {"x": 5}, True)
    nested = wrapped({"$ref": "#/$defs/alias"})
    nested["$defs"] = {
        "alias": {"$ref": "#/$defs/value"},
        "value": {"type": "integer", "minimum": 5},
    }
    add("nested_refs_invalid", nested, {"x": 4}, False)
    add("nested_refs_valid", nested, {"x": 5}, True)
    escaped = wrapped({"$ref": "#/$defs/a~1b~0c"})
    escaped["$defs"] = {"a/b~c": {"type": "string", "minLength": 3}}
    add("escaped_pointer_invalid", escaped, {"x": "ab"}, False)
    add("escaped_pointer_valid", escaped, {"x": "abc"}, True)
    indexed = wrapped({"$ref": "#/$defs/choices/anyOf/0"})
    indexed["$defs"] = {"choices": {"anyOf": [{"type": "integer", "minimum": 5}]}}
    add("array_pointer_invalid", indexed, {"x": 4}, False)
    add("array_pointer_valid", indexed, {"x": 5}, True)
    for name, reference in [
        ("invalid_pointer_escape", "#/$defs/~2"),
        ("invalid_array_index", "#/$defs/choices/anyOf/00"),
    ]:
        malformed = wrapped({"$ref": reference})
        malformed["$defs"] = {"~2": {}, "choices": {"anyOf": [{}]}}
        add(name, malformed, {"x": 5}, False, "schema $ref")
    boolean_reference = wrapped({"$ref": "#/$defs/forbidden"})
    boolean_reference["$defs"] = {"forbidden": False}
    add("boolean_reference", boolean_reference, {"x": 5}, False)

    for keyword in ("anyOf", "oneOf"):
        nullable = wrapped({keyword: [{"$ref": "#/$defs/value"}, {"type": "null"}]})
        nullable["$defs"] = {"value": {"type": "integer", "minimum": 5}}
        add(keyword + "_invalid", nullable, {"x": 4}, False)
        add(keyword + "_null", nullable, {"x": None}, True)

    placements = [
        ("items", {"type": "array", "items": {"$ref": "#/$defs/value"}}, [4], [5]),
        ("allOf", {"allOf": [{"$ref": "#/$defs/value"}]}, 4, 5),
        ("not", {"not": {"$ref": "#/$defs/value"}}, 5, 4),
        (
            "patternProperties",
            {"patternProperties": {"^a": {"$ref": "#/$defs/value"}}},
            {"a": 4},
            {"a": 5},
        ),
        (
            "additionalProperties",
            {"additionalProperties": {"$ref": "#/$defs/value"}},
            {"a": 4},
            {"a": 5},
        ),
        (
            "contains",
            {"type": "array", "contains": {"$ref": "#/$defs/value"}},
            [4],
            [4, 5],
        ),
    ]
    for name, field, bad, good in placements:
        schema = wrapped(field)
        schema["$defs"] = {"value": {"type": "integer", "minimum": 5}}
        add(name + "_invalid", schema, {"x": bad}, False)
        add(name + "_valid", schema, {"x": good}, True)

    siblings = wrapped({"$ref": "#/$defs/value", "maximum": 10})
    siblings["$defs"] = {"value": {"type": "integer", "minimum": 5}}
    add("ref_siblings_target_invalid", siblings, {"x": 4}, False)
    add("ref_siblings_invalid", siblings, {"x": 11}, False)
    add("ref_siblings_valid", siblings, {"x": 7}, True)

    for name, reference in [
        ("unresolved", "#/$defs/missing"),
        ("external", "https://example.invalid/schema.json"),
    ]:
        add(name + "_ref", wrapped({"$ref": reference}), {"x": 5}, False, "schema $ref")
    add("direct_cycle", {"$ref": "#"}, {}, False, "schema $ref")
    add(
        "mutual_cycle",
        {
            "$defs": {"a": {"$ref": "#/$defs/b"}, "b": {"$ref": "#/$defs/a"}},
            "$ref": "#/$defs/a",
        },
        {},
        False,
        "schema $ref",
    )
    # A matching alternative must not hide a broken reference or invert it through not.
    add(
        "unresolved_anyOf",
        {"anyOf": [{"$ref": "#/$defs/missing"}, {}]},
        {},
        False,
        "schema $ref",
    )
    add("unresolved_not", {"not": {"$ref": "#/$defs/missing"}}, {}, False, "schema $ref")
    recursive = {
        "$defs": {
            "node": {
                "type": "object",
                "properties": {
                    "value": {"type": "integer", "minimum": 5},
                    "next": {"anyOf": [{"$ref": "#/$defs/node"}, {"type": "null"}]},
                },
                "required": ["value", "next"],
            }
        },
        "$ref": "#/$defs/node",
    }
    add(
        "finite_recursive_valid",
        recursive,
        {"value": 5, "next": {"value": 6, "next": None}},
        True,
    )
    add(
        "finite_recursive_invalid",
        recursive,
        {"value": 5, "next": {"value": 4, "next": None}},
        False,
    )
    # Reference limits bound work without rejecting repeated uses or finite descent.
    for count, valid in ((127, True), (128, False)):
        definitions = {f"n{i}": {"$ref": f"#/$defs/n{i + 1}"} for i in range(count)}
        definitions[f"n{count}"] = {"type": "object"}
        add(
            f"reference_chain_{count + 1}",
            {"$defs": definitions, "$ref": "#/$defs/n0"},
            {},
            valid,
            None if valid else "schema $ref",
        )
    names = {
        "propertyNames": {"$ref": "#/$defs/name"},
        "$defs": {"name": {"minLength": 2}},
    }
    add("propertyNames_invalid", names, {"a": 1}, False)
    add("propertyNames_valid", names, {"ab": 1}, True)
    repeated = {
        "properties": {"a": {"$ref": "#/$defs/value"}, "b": {"$ref": "#/$defs/value"}},
        "$defs": {"value": {"type": "integer", "minimum": 5}},
    }
    add("repeated_reference_valid", repeated, {"a": 5, "b": 6}, True)
    return cases


def run(duckdb_path):
    responses = {}
    calls = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            prompt = body["messages"][-1]["content"]
            calls.append(prompt)
            values = responses[prompt]
            content = values.pop(0) if len(values) > 1 else values[0]
            payload = {
                "choices": [
                    {
                        "message": {"content": json.dumps(content)},
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": 7,
                    "completion_tokens": 3,
                    "total_tokens": 10,
                },
            }
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "schema-reference-dummy-key"
    setup = f"""
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'schema-reference-mock';
        SET duckdb_ai_base_url = 'http://127.0.0.1:{server.server_port}/v1';
        SET duckdb_ai_retry_count = 0;
        SET duckdb_ai_timeout_seconds = 5;
    """

    def execute(sql, strict=False):
        result = subprocess.run(
            [
                str(Path(duckdb_path).resolve()),
                "-init",
                os.devnull,
                "-json",
                "-c",
                setup + sql,
            ],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=20,
        )
        if strict:
            assert result.returncode != 0 and "schema validation failed" in result.stderr, result
            return
        assert result.returncode == 0, result.stdout + result.stderr
        return json.loads(result.stdout)

    def expression(function, prompt, schema, options="fail_on_error := false"):
        arguments = f"{sql_string(prompt)}, "
        if function == "ai_complete_json":
            arguments += "response_schema := "
        arguments += sql_string(json.dumps(schema)) + ", " + options
        return function + "(" + arguments + ")"

    def check(name, schema, value, valid, error=None, function="ai_complete_json"):
        responses[name] = [value]
        call = expression(function, name, schema)
        if function == "ai_extract_record":
            selection = f"SELECT result IS NULL AS record_is_null, result.x AS x FROM (SELECT {call} AS result)"
        elif function == "ai_complete_record":
            selection = f"SELECT * FROM {call}"
        else:
            selection = f"SELECT {call} AS value"
        row = execute(
            f"CREATE TEMP TABLE observed AS {selection}; "
            "SELECT observed.*, (SELECT count(*) FROM ai_usage()) AS usage_events, "
            "(SELECT status FROM ai_usage() LIMIT 1) AS usage_status, "
            "(SELECT error FROM ai_usage() LIMIT 1) AS usage_error, "
            "(SELECT prompt_tokens FROM ai_usage() LIMIT 1) AS usage_prompt_tokens, "
            "(SELECT completion_tokens FROM ai_usage() LIMIT 1) AS usage_completion_tokens, "
            "(SELECT total_tokens FROM ai_usage() LIMIT 1) AS usage_total_tokens FROM observed;"
        )[0]
        actual = {key: value for key, value in row.items() if not key.startswith("usage_")}
        if function == "ai_complete_json" and isinstance(actual["value"], str):
            actual["value"] = json.loads(actual["value"])
        expected = value if valid else None
        if function == "ai_complete_record":
            assert actual == (value if valid else {key: None for key in value}), (
                name,
                actual,
            )
        elif function == "ai_extract_record":
            assert actual["record_is_null"] == (not valid), (name, actual)
            if valid:
                assert actual["x"] == value["x"], (name, actual)
        else:
            assert actual == {"value": expected}, (name, actual, "expected", expected)
        assert row["usage_events"] == 1, (name, row)
        assert (
            row["usage_prompt_tokens"],
            row["usage_completion_tokens"],
            row["usage_total_tokens"],
        ) == (7, 3, 10), (
            name,
            row,
        )
        assert row["usage_status"] == ("ok" if valid else "error"), (name, row)
        assert (row["usage_error"] is None) == valid, (name, row)
        if error:
            assert error in row["usage_error"], (name, row)

    try:
        cases = schema_cases()
        for case in cases:
            check(*case)
        record_schema = {
            "type": "object",
            "$defs": {"score": {"type": "integer", "minimum": 5}},
            "properties": {"x": {"$ref": "#/$defs/score"}},
            "required": ["x"],
        }
        indexed_record_schema = {
            "type": "object",
            "$defs": {"choices": {"anyOf": [{"type": "integer", "minimum": 5}]}},
            "properties": {"x": {"$ref": "#/$defs/choices/anyOf/0"}},
            "required": ["x"],
        }
        for function in ("ai_extract_record", "ai_complete_record"):
            for field_type, expected_value in ((None, "5"), ("integer", 5)):
                schema = {
                    **indexed_record_schema,
                    "properties": {"x": dict(indexed_record_schema["properties"]["x"])},
                }
                if field_type:
                    schema["properties"]["x"]["type"] = field_type
                suffix = field_type or "untyped"
                name = function + "_array_projection_" + suffix
                responses[name] = [{"x": 5}]
                call = expression(function, name, schema)
                selection = "SELECT " + call + " AS r" if function == "ai_extract_record" else "SELECT * FROM " + call
                before = len(calls)
                columns = execute("DESCRIBE " + selection)
                column_type = "BIGINT" if field_type else "VARCHAR"
                expected_type = f"STRUCT(x {column_type})" if function == "ai_extract_record" else column_type
                assert columns[0]["column_type"] == expected_type, (function, columns)
                assert len(calls) == before, "DESCRIBE must not call a provider"
                projection = (
                    "SELECT r.x AS x FROM (" + selection + ")" if function == "ai_extract_record" else selection
                )
                assert execute(projection) == [{"x": expected_value}], function
                check(function + "_array_invalid_" + suffix, schema, {"x": 4}, False, function=function)

        for function in ("ai_extract_record", "ai_complete_record"):
            check(function + "_invalid", record_schema, {"x": 4}, False, function=function)
            check(function + "_valid", record_schema, {"x": 5}, True, function=function)
            prompt = function + "_strict"
            responses[prompt] = [{"x": 4}]
            call = expression(function, prompt, record_schema, "fail_on_error := true")
            execute(
                ("SELECT * FROM " if function == "ai_complete_record" else "SELECT ") + call,
                strict=True,
            )

        # Validation failures must not populate the response cache. A corrected reply
        # for an identical SQL request is fetched, then successfully cached.
        responses["cache_recovery"] = [{"x": 4}, {"x": 5}]
        call = expression(
            "ai_complete_json",
            "cache_recovery",
            record_schema,
            "fail_on_error := false, cache := true",
        )
        cache = execute(
            f"CREATE TEMP TABLE first AS SELECT {call} AS value; "
            f"CREATE TEMP TABLE second AS SELECT {call} AS value; "
            f"CREATE TEMP TABLE third AS SELECT {call} AS value; "
            "SELECT (SELECT value IS NULL FROM first) AS rejected, "
            "(SELECT value FROM second) AS corrected, "
            "(SELECT value FROM third) AS cached, "
            "(SELECT count(*) FROM ai_usage() WHERE cache_hit) AS cache_hits;"
        )[0]
        for key in ("corrected", "cached"):
            if isinstance(cache[key], str):
                cache[key] = json.loads(cache[key])
        assert cache == {
            "rejected": True,
            "corrected": {"x": 5},
            "cached": {"x": 5},
            "cache_hits": 1,
        }, cache
        assert calls.count("cache_recovery") == 2, calls
        print(
            f"schema reference smoke passed: {len(cases)} schema cases, typed records, legacy projections, usage, errors and cache recovery"
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, default=ROOT / "build/release/duckdb")
    run(parser.parse_args().duckdb)
