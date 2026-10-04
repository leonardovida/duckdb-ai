#!/usr/bin/env python3
"""Characterize response lookup precedence through local HTTP and public SQL."""

import argparse
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


def run(duckdb):
    body = ""
    status = 200
    requests = []

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            payload = body.encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = HTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "response-lookup-dummy"
    options = f"""
        model := 'lookup-mock', base_url := 'http://127.0.0.1:{server.server_port}',
        cache := false, retry_count := 0, fail_on_error := false
    """

    def execute(sql):
        requests.clear()
        result = subprocess.run(
            [str(duckdb), "-batch", "-bail", "-json", "-init", os.devnull],
            input="SET threads=1;" + sql,
            text=True,
            capture_output=True,
            env=env,
            timeout=30,
        )
        assert result.returncode == 0, result.stderr
        assert len(requests) == 1, requests
        return json.loads(result.stdout)

    def assert_io_error(error, message):
        assert json.loads(error) == {"exception_type": "IO", "exception_message": message}, error

    checks = 0
    try:
        # All five diagnostic fields must keep independent depth-first lookup.
        status = 400
        error_cases = [
            (
                '{"nested":[{"message":"first","type":"nested"}],"message":"later","code":"bad"}',
                " [type=nested, code=bad]: first",
            ),
            ('{"message":"","nested":{"message":"ignored"},"error":"fallback","detail":"last"}', ": fallback"),
            ('{"message":17,"error":{"detail":"nested detail"}}', ": nested detail"),
            ('{"message":"first","message":"second","type":"","code":"bad"}', " [code=bad]: first"),
            ('{"message":"quoted \\"value\\"\\nresponse-lookup-dummy"}', ': quoted "value"\n[redacted]'),
            ('{"message":"partial"', ': {"message":"partial"'),
            ('{"message":false}', ': {"message":false}'),
            ('[null,{"error":"array error"}]', ": array error"),
        ]
        prefix = 'AI provider "openai" (openai_chat, model "lookup-mock") returned HTTP 400'
        for body, suffix in error_cases:
            rows = execute(
                f"""
                CREATE TEMP TABLE result AS SELECT ai_try_complete('lookup', provider := 'openai', {options}) r;
                SELECT r.response, r.error, (SELECT error FROM ai_usage()) usage_error FROM result;
            """
            )
            expected = prefix + suffix
            assert len(rows) == 1 and rows[0]["response"] is None, rows
            assert_io_error(rows[0]["error"], expected)
            assert rows[0]["usage_error"] == expected, rows
            checks += 1

        status = 200
        scalar_cases = [
            ('{"nested":{"embedding":[1,2]},"embedding":[3,4]}', [1, 2], -1, -1),
            ('{"embedding":{"other":[3,4],"embedding":[5,6]}}', [3, 4], -1, -1),
            ('{"embedding":[0,"bad",{"embedding":[5,6]}]}', [5, 6], -1, -1),
            ('{"embedding":[],"embedding":[3,4],"embeddings":[[7,8]]}', [7, 8], -1, -1),
            ('{"embedding":[],"embedding":[3,4]}', None, -1, -1),
            ('{"embeddings":[[7,8]]}', [7, 8], -1, -1),
            ('{"embedding":[1,2],"usage":{"prompt_tokens":5,"total_tokens":7},"prompt_tokens":9}', [1, 2], 5, 7),
            ('{"embedding":[1,2],"prompt_tokens":9223372036854775808,"usage":{"prompt_tokens":5}}', [1, 2], -1, -1),
            ('{"embedding":[1,2],"usage":{"prompt_tokens":9223372036854775808},"prompt_tokens":5}', [1, 2], 5, -1),
            ('{"embedding":[1,2],"prompt_tokens":2.5,"usage":{"prompt_tokens":5,"total_tokens":7}}', [1, 2], 5, 7),
            ('{"embedding":[1,2]', None, -1, -1),
            ('null', None, -1, -1),
        ]
        for body, values, prompt_tokens, total_tokens in scalar_cases:
            rows = execute(
                f"""
                CREATE TEMP TABLE result AS SELECT ai_embed('lookup', provider := 'openai', {options}) embedding;
                SELECT embedding, prompt_tokens, total_tokens, error FROM result CROSS JOIN ai_usage();
            """
            )
            error = rows[0]["error"]
            if values is None:
                assert_io_error(error, "AI provider embedding response contained an empty embedding: " + body)
            else:
                assert error is None, rows
            assert rows == [
                {"embedding": values, "prompt_tokens": prompt_tokens, "total_tokens": total_tokens, "error": error}
            ], (body, rows)
            checks += 1

        for provider in ("openai", "ollama"):
            field = (
                '"data":[{"embedding":[3,4],"index":1},{"embedding":[1,2],"index":0}]'
                if provider == "openai"
                else '"embeddings":[[1,2],[3,4]]'
            )
            for usage, expected_prompt in (
                ('"usage":{"prompt_tokens":5,"total_tokens":7}', [3, 2]),
                ('"prompt_tokens":9223372036854775808,"usage":{"prompt_tokens":5,"total_tokens":7}', [-1, -1]),
            ):
                body = "{" + field + "," + usage + "}"
                rows = execute(
                    f"""
                    CREATE TEMP TABLE result AS
                        SELECT i, ai_embed('lookup ' || i, provider := '{provider}', {options}) embedding FROM range(2) t(i);
                    SELECT (SELECT list(embedding ORDER BY i) FROM result) embeddings,
                           list(prompt_tokens ORDER BY event_id) prompt_tokens,
                           list(total_tokens ORDER BY event_id) total_tokens,
                           count(error) errors FROM ai_usage();
                """
                )
                assert rows == [
                    {
                        "embeddings": [[1, 2], [3, 4]],
                        "prompt_tokens": expected_prompt,
                        "total_tokens": [4, 3],
                        "errors": 0,
                    }
                ], (body, rows)
                assert requests[0]["input"] == ["lookup 0", "lookup 1"], requests
                checks += 1
            for body in ('{"embeddings":', 'null'):
                rows = execute(
                    f"""
                    CREATE TEMP TABLE result AS
                        SELECT ai_embed('lookup ' || i, provider := '{provider}', {options}) embedding FROM range(2) t(i);
                    SELECT (SELECT count(embedding) FROM result) successes, error FROM ai_usage() ORDER BY event_id;
                """
                )
                assert len(rows) == 2 and all(row["successes"] == 0 for row in rows), rows
                for row in rows:
                    assert_io_error(row["error"], "AI provider embedding response returned 0 embeddings for 2 inputs")
                checks += 1

        # Ollama scalar lookup never falls back to the singular field.
        body = '{"embeddings":[[],[3,4]],"embedding":[5,6]}'
        rows = execute(
            f"""
            CREATE TEMP TABLE result AS SELECT ai_embed('lookup', provider := 'ollama', {options}) embedding;
            SELECT embedding, error FROM result CROSS JOIN ai_usage();
        """
        )
        assert len(rows) == 1 and rows[0]["embedding"] is None, rows
        assert_io_error(rows[0]["error"], "AI provider embedding response contained an empty embedding: " + body)
        checks += 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print(f"response lookup smoke passed ({checks} cases)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path(__file__).resolve().parents[2] / "build/release/duckdb")
    run(parser.parse_args().duckdb)
