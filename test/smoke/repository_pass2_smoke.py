#!/usr/bin/env python3
"""Second-sweep regressions; local providers only, including interruptible pacing."""

import argparse
import csv
import io
import json
import math
import os
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from decimal import Decimal

from repository_regression_smoke import literal


class Provider(BaseHTTPRequestHandler):
    labels = ["alpha", "beta"]
    requests = []
    received = threading.Event()

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["content-length"])))
        self.requests.append((self.path, request, self.headers.get("Authorization")))
        if self.path == "/embeddings":
            texts = request["input"]
            texts = texts if isinstance(texts, list) else [texts]

            def coordinates(text):
                if "extreme" in text:
                    sign = 1 if "alpha" in text else -1
                    return [sign * sys.float_info.max, sign * sys.float_info.max]
                if "opposite" in text:
                    # The first row is held out. Training means combine opposite
                    # signs without forming an overflowing difference.
                    sign = -1 if text.endswith("2") else 1
                    return [1.0, sign * sys.float_info.max]
                return [1.0, 0.0] if "alpha" in text else [0.0, 1.0]

            response = {"data": [{"index": i, "embedding": coordinates(t)} for i, t in enumerate(texts)]}
        else:
            prompt = request["messages"][-1]["content"]
            if "account SQL" in prompt:
                content = "SELECT " + self.headers["Authorization"].rsplit("-", 1)[-1] + " AS value"
            elif "cache bound" in prompt:
                content = "SELECT 42 AS value"
            else:
                content = json.dumps(self.labels[0 if "alpha" in prompt else 1])
            response = {"choices": [{"message": {"content": content}}]}
        data = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self.received.set()

    def log_message(self, *_):
        pass


def run(duckdb_path, only=None):
    server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_port}"
    env = {k: v for k, v in os.environ.items() if not k.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "pass2-dummy-1"
    setup = f"""
SET threads = 1;
SET duckdb_ai_provider = 'openai';
SET duckdb_ai_model = 'pass2-model';
SET duckdb_ai_task_model = 'pass2-model';
SET duckdb_ai_embedding_model = 'pass2-embedding';
SET duckdb_ai_base_url = '{base_url}';
SET duckdb_ai_timeout_seconds = 5;
"""
    command = [str(duckdb_path), "-batch", "-bail", "-csv", "-noheader", "-init", os.devnull]
    checks = 0

    def execute(sql):
        result = subprocess.run(command, input=(setup + sql).encode(), env=env, capture_output=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stderr.decode())
        return list(csv.reader(io.StringIO(result.stdout.decode(), newline="")))

    def check(sql, expected):
        nonlocal checks
        rows = execute(sql)
        if rows != expected:
            raise AssertionError(f"expected {expected}, got {rows}")
        checks += 1

    def training(labels, qualifier=""):
        label_sql = "[" + ", ".join(literal(label) for label in labels) + "]"
        return (
            "CREATE TEMP TABLE training AS SELECT "
            "CASE WHEN i < 5 THEN 'alpha ' ELSE 'beta ' END || "
            f"{literal(qualifier)} || (i % 5)::VARCHAR AS text FROM range(10) t(i);"
            f"CREATE TEMP TABLE trained AS SELECT ai_build_classifier(text, {label_sql}, "
            "optimization := 'minimize_cost', quality_threshold := 1) AS artifact FROM training;"
        )

    try:
        if only in (None, "centroids"):
            Provider.labels = ["alpha", "beta"]
            artifact = json.loads(
                execute(training(Provider.labels, "extreme ") + "SELECT artifact FROM trained;")[0][0]
            )
            assert artifact["usable"] and artifact["accuracy"] == 1
            assert artifact["centroids"] == [[sys.float_info.max] * 2, [-sys.float_info.max] * 2]
            checks += 1
            artifact = json.loads(
                execute(training(Provider.labels, "opposite ") + "SELECT artifact FROM trained;")[0][0]
            )
            texts = next(request["input"] for path, request, _ in reversed(Provider.requests) if path == "/embeddings")
            for label, centroid in zip(("alpha", "beta"), artifact["centroids"]):
                rows = [text for text in texts if label in text]
                training_rows = [text for index, text in enumerate(rows) if index % 5 != 0]
                expected = sum(
                    (
                        Decimal(-sys.float_info.max if text.endswith("2") else sys.float_info.max)
                        for text in training_rows
                    ),
                    Decimal(0),
                ) / len(training_rows)
                assert all(math.isfinite(value) for value in centroid)
                assert math.isclose(centroid[1], float(expected), rel_tol=1e-15)
            checks += 1

        if only in (None, "labels"):
            for first in (
                'billing, overdue',
                'say "yes"',
                'path\\name',
                'line\nbreak',
                'tab\tlabel',
                'carriage\rreturn',
                'control\x01label',
                'é😀',
                '"literal quotes"',
                ' padded label ',
                '```json\nlabel\n```',
            ):
                Provider.labels = [first, "support"]
                check(
                    training(Provider.labels) + "SELECT contains(artifact, '\"usable\":true') FROM trained;", [["true"]]
                )
                artifact = {
                    "version": 1,
                    "optimization": "minimize_cost",
                    "usable": False,
                    "accuracy": 0,
                    "confidence_margin": 0.05,
                    "labels": Provider.labels,
                    "centroids": [[1, 0], [0, 1]],
                    "embedding": {"provider": "openai", "model": "pass2-embedding", "base_url": base_url},
                }
                check(
                    "SELECT result.value, result.used_fallback, result.error IS NULL FROM "
                    f"(SELECT ai_classify_optimized('alpha input', {literal(json.dumps(artifact))}) AS result);",
                    [[first, "true", "true"]],
                )
                check(f"SELECT ai_classify('alpha input', [{literal(first)}, 'support']);", [[first]])

        if only in (None, "accounts"):
            query = "SELECT * FROM ai_query_data('account SQL', schema_context := 'SELECT 1', profile := 'account');"
            requests_before = len(Provider.requests)
            create = lambda number: (
                "CREATE OR REPLACE SECRET account (TYPE duckdb_ai, AI_PROVIDER 'openai', "
                f"MODEL 'pass2-model', BASE_URL '{base_url}', API_KEY 'pass2-dummy-{number}');"
            )
            check(
                create(11) + query + query + create(22) + query + query,
                [["true"], ["11"], ["11"], ["true"], ["22"], ["22"]],
            )
            assert len(Provider.requests) - requests_before == 2, "account SQL should cache within each credential"
            checks += 1

            # Tightening the same profile must invalidate its previous SQL entry.
            model = lambda limit: (
                "CREATE OR REPLACE EXTERNAL MODEL capped_sql WITH (provider = 'openai', "
                f"model = 'pass2-model', location = '{base_url}', model_type = 'completion', "
                f"options = '{{\"max_input_tokens\":{limit}}}');"
            )
            query = "SELECT count(*) FROM ai_query_data('cache bound profile', schema_context := 'SELECT 1', profile := 'capped_sql', fail_on_error := false);"
            check(
                model(10000) + query + model(1) + query, [["capped_sql", "true"], ["1"], ["capped_sql", "true"], ["0"]]
            )

        if only in (None, "cache"):
            requests_before = len(Provider.requests)
            queries = [
                f"SELECT * FROM ai_query_data('cache bound {i}', schema_context := repeat('x', 4 * 1024 * 1024));"
                for i in range(9)
            ]
            # A hit refreshes recency, then byte eviction removes earlier keys.
            check("".join(queries[:7]) + queries[0] + "".join(queries[7:]) + queries[0] + queries[1], [["42"]] * 12)
            assert len(Provider.requests) - requests_before == 10, "SQL byte cap and LRU eviction failed"
            checks += 1
            Provider.requests.clear()  # Release captured 4 MiB mock request bodies.

        if only in (None, "pacing"):
            for options in (
                "system_prompt := repeat('x', 1000)",
                "response_schema := '{\"description\":\"' || repeat('x', 1000) || '\"}'",
            ):
                Provider.received.clear()
                requests_before = len(Provider.requests)
                sql = "SET duckdb_ai_token_limit_per_minute = 100;" + (
                    f"SELECT ai_complete('alpha', max_tokens := 1, {options});" * 2
                )
                process = subprocess.Popen(
                    command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, text=True
                )
                try:
                    process.stdin.write(setup + sql)
                    process.stdin.close()
                    assert Provider.received.wait(5), "first paced request never arrived"
                    # Give an incorrectly unpaced second request time to arrive.
                    time.sleep(0.5)
                    assert process.poll() is None, "second request did not wait for its token window"
                    assert len(Provider.requests) - requests_before == 1, "system/schema tokens were not reserved"
                    process.send_signal(signal.SIGINT)
                    process.wait(timeout=5)
                    error = process.stderr.read()
                    assert process.returncode != 0, f"pacing cancellation unexpectedly succeeded: {error}"
                    assert len(list(csv.reader(io.StringIO(process.stdout.read())))) == 1
                    checks += 1
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)
                    process.stdout.close()
                    process.stderr.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print(f"repository pass 2 smoke passed ({checks} checks)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path(__file__).resolve().parents[2] / "build/release/duckdb")
    parser.add_argument("--only", choices=("centroids", "labels", "accounts", "cache", "pacing"))
    args = parser.parse_args()
    run(args.duckdb, args.only)
