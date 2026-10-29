#!/usr/bin/env python3
"""Pin JSON bytes at the SQL-value and provider-payload boundaries without HTTP."""

import argparse
import json
import os
import subprocess
from pathlib import Path


def run(duckdb):
    # NUL through DEL, UTF-8, and combined escapes exercise both signed-char builds
    # and the two historical spellings of backspace/form feed.
    cases = [(f"chr({code})", chr(code)) for code in range(128)]
    cases += [
        ("'é中😀'", "é中😀"),
        ("''", ""),
        ("chr(8) || chr(12) || chr(92) || chr(34) || 'é'", '\b\f\\"é'),
    ]
    cases += [("chr(92) || 'b'", r"\b"), ("chr(92) || 'f'", r"\f")]
    statements = []
    for index, (expression, _) in enumerate(cases):
        statements.append(
            f"""SELECT {index} AS id,
                ai_embedding_request_json({expression}, provider := 'openai', model := 'escape-mock') AS embedding,
                ai_completion_request_json({expression}, provider := 'openai', model := 'escape-mock',
                    request_options := {{'probe': {expression}, 'nested': [{expression}]}}) AS completion,
                ai_completion_request_json('key', provider := 'openai', model := 'escape-mock',
                    request_options := map([{expression}], [{expression}])) AS map_request"""
        )
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_")}
    result = subprocess.run(
        [str(duckdb), "-batch", "-bail", "-json", "-init", os.devnull],
        input=" UNION ALL ".join(statements) + "; SELECT count(*) AS usage_rows FROM ai_usage();",
        text=True,
        capture_output=True,
        env=env,
        timeout=60,
    )
    assert result.returncode == 0, result.stderr
    decoder = json.JSONDecoder()
    rows, end = decoder.raw_decode(result.stdout)
    usage = json.loads(result.stdout[end:])
    assert usage == [{"usage_rows": 0}], usage
    assert len(rows) == len(cases), len(rows)
    sql_controls = {"\b": r"\u0008", "\f": r"\u000c"}
    for row in rows:
        text = cases[row["id"]][1]
        provider_string = json.dumps(text, ensure_ascii=False)
        sql_string = (
            '"' + "".join(sql_controls.get(char, json.dumps(char, ensure_ascii=False)[1:-1]) for char in text) + '"'
        )
        assert row["embedding"] == '{"model":"escape-mock","input":' + provider_string + "}", row
        expected_options = '"probe":' + sql_string + ',"nested":[' + sql_string + "]"
        assert expected_options in row["completion"], row
        completion = json.loads(row["completion"])
        assert completion["messages"][-1]["content"] == text, row
        assert completion["probe"] == text and completion["nested"] == [text], row
        assert "," + sql_string + ":" + sql_string + "}" in row["map_request"], row
        assert json.loads(row["map_request"])[text] == text, row
    print(f"JSON escaping smoke passed ({len(cases)} byte/UTF-8 cases, no usage events)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--duckdb",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "build/release/duckdb",
    )
    run(parser.parse_args().duckdb)
