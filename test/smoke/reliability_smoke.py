#!/usr/bin/env python3
"""Deterministic sampling, budget, telemetry, schema fuzz and concurrency checks."""

import argparse
import ctypes
import json
import os
import random
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor
from http.server import ThreadingHTTPServer
from pathlib import Path

from repository_regression_smoke import RegressionProvider, literal


def json_stream(value):
    decoder = json.JSONDecoder()
    results = []
    while value.strip():
        value = value.lstrip()
        result, end = decoder.raw_decode(value)
        results.append(result)
        value = value[end:]
    return results


def run(duckdb, library=None):
    server = ThreadingHTTPServer(("127.0.0.1", 0), RegressionProvider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"
    env = {k: v for k, v in os.environ.items() if not k.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "reliability-dummy"
    setup = f"SET threads=4; SET duckdb_ai_provider='openai'; SET duckdb_ai_model='reliability'; SET duckdb_ai_task_model='reliability'; SET duckdb_ai_embedding_model='reliability'; SET duckdb_ai_base_url='{endpoint}';"
    checks = 0

    def execute(sql, prefix=setup):
        result = subprocess.run(
            [str(duckdb), "-batch", "-bail", "-json", "-init", os.devnull],
            input=prefix + sql,
            text=True,
            capture_output=True,
            env=env,
            timeout=45,
        )
        if result.returncode:
            raise AssertionError(result.stderr)
        return json_stream(result.stdout)

    try:

        def artifact(order):
            return json.loads(
                execute(
                    "CREATE TEMP TABLE input AS SELECT CASE WHEN i % 200 < 100 THEN 'alpha item ' ELSE 'beta item ' END || (i % 100)::VARCHAR AS text FROM range(4000) t(i);"
                    f"SELECT ai_build_classifier(text, ['alpha', 'beta'], sample_size := 16, quality_threshold := 1, optimization := 'minimize_cost') AS artifact FROM (SELECT text FROM input ORDER BY text {order});"
                )[-1][0]["artifact"]
            )

        forward, reverse = artifact("ASC"), artifact("DESC")
        assert forward == reverse and forward["usable"] and forward["sample_count"] == 16
        assert forward["total_count"] == 4000 and forward["sampling"] == "distinct_text_bottom_k"
        checks += 1
        duplicate = json.loads(
            execute(
                "SELECT ai_build_classifier(CASE WHEN i % 2=0 THEN 'alpha item' ELSE 'beta item' END, ['alpha', 'beta'], sample_size := 16, quality_threshold := 0, optimization := 'minimize_cost') AS artifact FROM range(2000) t(i);"
            )[-1][0]["artifact"]
        )
        assert duplicate["sample_count"] == 2 and duplicate["validation_count"] == 0 and not duplicate["usable"]
        checks += 1
        # Verify actual parallel aggregate state merging against a single worker.
        query = "SELECT ai_build_classifier(CASE WHEN i % 2=0 THEN 'alpha item ' ELSE 'beta item ' END || i::VARCHAR, ['alpha', 'beta'], sample_size := 16, optimization := 'minimize_cost') AS artifact FROM range(200000) t(i);"
        single = execute("SET threads=1;" + query)[-1]
        parallel = execute(query)[-1]
        assert single == parallel
        checks += 1

        for options, prompt, max_tokens, expected in (
            ({"context_size": 8}, "x", 7, True),
            ({"context_size": 8}, "x", 8, False),
            ({"max_input_tokens": 1}, "x", 8, True),
            ({"max_input_tokens": 3, "token_estimate_multiplier": 2}, "abcdef", 1, False),
            ({"context_size": 100, "max_input_tokens": 1}, "abcdef", 1, False),
            ({"max_input_tokens": 3, "token_estimate_multiplier": 0.5}, "x", 1, False),
            ({"token_estimate_multiplier": "bad"}, "x", 1, False),
            ({"max_input_tokens": "1"}, "x", 1, False),
            ({"context_size": 1.5}, "x", 1, False),
        ):
            create = f"CREATE EXTERNAL MODEL budget WITH (provider='openai',model='reliability', location='{endpoint}', model_type='completion', options={literal(json.dumps(options))});"
            rows = execute(
                create
                + f"SELECT ai_completion_request_json({literal(prompt)}, profile := 'budget', max_tokens := {max_tokens}, fail_on_error := false) IS NOT NULL AS valid;"
            )
            assert rows[-1][0]["valid"] == expected, (options, rows)
            checks += 1
        create = f"CREATE EXTERNAL MODEL budget WITH (provider='openai',model='reliability', location='{endpoint}', model_type='embedding', options='{{\"max_batch_tokens\":3,\"token_estimate_multiplier\":2}}');"
        assert execute(
            create + "SELECT ai_embed('abcdef', profile := 'budget', fail_on_error := false) IS NULL AS rejected;"
        )[-1][0]["rejected"]
        checks += 1

        stats = {key: int(value) for key, value in execute("SELECT * FROM ai_query_cache_stats();")[-1][0].items()}
        assert stats["entries"] == stats["bytes"] == stats["hits"] == stats["misses"] == 0
        assert stats["max_entries"] == 1024 and stats["max_bytes"] == 64 * 1024 * 1024
        checks += 1
        before = len(RegressionProvider.requests)
        result = execute(
            "SELECT sum(ai_embed('alpha item ' || i)[1]) FROM range(1500) t(i); SELECT count(*) AS retained FROM ai_usage(); SELECT * FROM ai_usage_totals();"
        )
        totals = {key: int(value) for key, value in result[-1][0].items()}
        assert result[-2][0]["retained"] == 1024
        assert (
            totals["provider_events"] == 1500
            and totals["request_attempts"] == len(RegressionProvider.requests) - before
        )
        assert totals["known_total_tokens"] == 1500 and totals["unknown_token_events"] == totals["failures"] == 0
        checks += 1
        result = execute(
            "SELECT ai_embed('alpha item', cache := true); SELECT ai_embed('alpha item', cache := true); SELECT * FROM ai_usage_totals(); SELECT * FROM ai_clear_usage(); SELECT * FROM ai_usage_totals();"
        )
        assert (
            int(result[2][0]["provider_events"]) == 2
            and int(result[2][0]["request_attempts"]) == 1
            and int(result[2][0]["cache_hits"]) == 1
        )
        assert not any(int(value) for value in result[-1][0].values())
        checks += 1

        generator = random.Random(20261001)
        for _ in range(100):
            value = generator.choice(
                [
                    None,
                    True,
                    generator.randrange(-30, 30),
                    "".join(generator.choices("abcé中😀", k=generator.randrange(12))),
                ]
            )
            types = generator.choice([["string", "null"], ["number", "boolean"], ["integer"], ["string"]])
            minimum, maximum = generator.randrange(4), generator.randrange(4, 10)
            actual_type = (
                "null"
                if value is None
                else "boolean" if isinstance(value, bool) else "integer" if isinstance(value, int) else "string"
            )
            valid = actual_type in types or (actual_type == "integer" and "number" in types)
            if isinstance(value, str):
                valid = valid and minimum <= len(value) <= maximum
            schema = {"properties": {"value": {"type": types, "minLength": minimum, "maxLength": maximum}}}
            sql = f"SELECT ai_complete_json({literal('return JSON:' + json.dumps({'value': value}))}, response_schema := {literal(json.dumps(schema))}, fail_on_error := false) IS NOT NULL AS valid;"
            assert execute(sql)[-1][0]["valid"] == valid, (value, schema)
            checks += 1
        for _ in range(40):
            text = "".join(generator.choices("abcé中😀\n\f", k=generator.randrange(1, 200)))
            chunks = execute(
                f"SELECT start_offset,end_offset,chunk AS text FROM ai_generate_chunks({literal(text)}, chunk_size := {generator.randrange(1, 35)}, overlap_percent := 0, strategy := 'fixed') ORDER BY chunk_index;"
            )[-1]
            cursor = 0
            for chunk in chunks:
                assert int(chunk["start_offset"]) == cursor
                cursor = int(chunk["end_offset"])
                assert chunk["text"] == text[int(chunk["start_offset"]) : cursor]
            assert cursor == len(text) and "".join(c["text"] for c in chunks) == text
            checks += 1

        before = len(RegressionProvider.requests)
        sql = "SELECT CASE WHEN bool_and(ai_complete('return JSON:ok', cache := true, max_concurrent_requests := 4)='ok') THEN 1 ELSE error('bad concurrent result') END FROM range(128);"
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: execute(sql), range(4)))
        assert len(results) == 4 and len(RegressionProvider.requests) - before == 4
        checks += 1
        if library:
            api = ctypes.CDLL(str(library.resolve()))
            handle = ctypes.c_void_p()
            api.duckdb_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
            api.duckdb_connect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
            api.duckdb_query.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
            api.duckdb_disconnect.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
            api.duckdb_close.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
            assert api.duckdb_open(None, ctypes.byref(handle)) == 0
            connections = []
            try:
                for _ in range(4):
                    connection = ctypes.c_void_p()
                    assert api.duckdb_connect(handle, ctypes.byref(connection)) == 0
                    connections.append(connection)
                create = f"LOAD ai; CREATE SECRET shared_ai (TYPE duckdb_ai, AI_PROVIDER 'openai', MODEL 'reliability', BASE_URL '{endpoint}', API_KEY 'dummy-key');"
                assert api.duckdb_query(connections[0], create.encode(), None) == 0
                shared = "SELECT CASE WHEN bool_and(ai_complete('return JSON:shared', profile := 'shared_ai', cache := true, max_concurrent_requests := 4)='shared') THEN 1 ELSE error('bad shared result') END FROM range(128);"
                before = len(RegressionProvider.requests)
                with ThreadPoolExecutor(max_workers=4) as pool:
                    statuses = list(
                        pool.map(lambda connection: api.duckdb_query(connection, shared.encode(), None), connections)
                    )
                assert statuses == [0] * 4 and len(RegressionProvider.requests) - before == 1
                checks += 1
            finally:
                for connection in connections:
                    api.duckdb_disconnect(ctypes.byref(connection))
                api.duckdb_close(ctypes.byref(handle))
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print(f"reliability smoke passed ({checks} checks)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path(__file__).resolve().parents[2] / "build/release/duckdb")
    parser.add_argument("--library", type=Path)
    args = parser.parse_args()
    run(args.duckdb, args.library)
