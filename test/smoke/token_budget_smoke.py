#!/usr/bin/env python3
"""Completion budgets stay positive at the BIGINT boundary, without paid calls."""

import argparse
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def run(duckdb):
    requests = []
    received = threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            response = b'{"choices":[{"message":{"content":"ok"}}]}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
            received.set()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "token-budget-mock"
    setup = (
        "SET threads=1; SET duckdb_ai_provider='openai'; SET duckdb_ai_model='budget-mock';"
        f"SET duckdb_ai_base_url='http://127.0.0.1:{server.server_port}';"
    )
    command = [str(duckdb), "-batch", "-bail", "-json", "-init", os.devnull]

    def execute(sql, fail=False):
        result = subprocess.run(command, input=setup + sql, env=env, text=True, capture_output=True, timeout=20)
        assert (result.returncode != 0) == fail, (result.stdout, result.stderr)
        return result

    try:
        # The input, system prompt and schema must all contribute to the guard.
        for prompt, extra in (
            ("x", ""),
            ("abcdefgh", ", system_prompt := 'abcdefgh'"),
            ("x", ", response_schema := '{}'"),
        ):
            for limit in (9223372036854775807, 9223372036854775806, 1000000000):
                result = execute(
                    "CREATE EXTERNAL MODEL budget WITH (provider='openai',model='budget-mock',"
                    "model_type='completion',options='{\"context_size\":32}');"
                    f"SELECT ai_completion_request_json('{prompt}', profile := 'budget', max_tokens := {limit}"
                    f"{extra}, fail_on_error := false) IS NULL AS rejected;"
                )
                assert '"rejected":true' in result.stdout, result.stdout

        result = execute(
            "CREATE EXTERNAL MODEL budget WITH (provider='openai',model='budget-mock',"
            "model_type='completion',options='{\"context_size\":8}');"
            "SELECT ai_completion_request_json('x', profile := 'budget', max_tokens := 7) IS NOT NULL AS valid;"
            "SELECT ai_completion_request_json('x', profile := 'budget', max_tokens := 8, on_error := 'null') IS NULL AS rejected;"
        )
        assert '"valid":true' in result.stdout and '"rejected":true' in result.stdout, result.stdout

        before = len(requests)
        result = execute(
            "CREATE EXTERNAL MODEL budget WITH (provider='openai',model='budget-mock',"
            "model_type='completion',options='{\"context_size\":1}');"
            "SELECT ai_try_complete('x', profile := 'budget', max_tokens := 9223372036854775807) AS result;"
            "SELECT http_status, error FROM ai_usage();"
        )
        assert "input plus output exceeds" in result.stdout and '"http_status":null' in result.stdout, result.stdout
        assert len(requests) == before, requests

        # Without a declared context window, large positive limits remain valid.
        for limit in (9223372036854775807, 9223372036854775806, 1000000000):
            result = execute(f"SELECT ai_complete('x', max_tokens := {limit}) AS response;")
            assert '"response":"ok"' in result.stdout, result.stdout
            assert requests[-1]["max_completion_tokens"] == limit, requests[-1]

        # Saturation must reserve a full window, rather than overflow and reserve zero.
        # The second request would wait a minute. Stop this task-owned process after
        # observing the first request and a short opportunity for an incorrect second.
        received.clear()
        before = len(requests)
        process = subprocess.Popen(
            command, env=env, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        try:
            process.stdin.write(
                setup
                + "SELECT ai_complete('x' || i, max_tokens := 9223372036854775807, token_limit_per_minute := 1) FROM range(2) t(i);"
            )
            process.stdin.close()
            process.stdin = None
            assert received.wait(20), "first paced request was not sent"
            try:
                stdout, stderr = process.communicate(timeout=2)
                raise AssertionError(("pacing query completed without waiting", stdout, stderr))
            except subprocess.TimeoutExpired:
                assert len(requests) - before == 1, requests[before:]
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print("token budget smoke passed (context, error accounting, BIGINT request and pacing boundaries)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path(__file__).resolve().parents[2] / "build/release/duckdb")
    run(parser.parse_args().duckdb)
