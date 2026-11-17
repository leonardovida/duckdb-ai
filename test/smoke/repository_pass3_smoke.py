#!/usr/bin/env python3
"""Third-sweep regressions against a local mock provider: batch isolation, truncated replies, bounded errors."""

import argparse
import csv
import io
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# Keep the DuckDB CLI on its default box output when a coding agent runs these tests.
os.environ["DUCKDB_AGENT_MODE"] = "0"

OPENAI_KEY = "pass3-openai-key"
SECRET_KEY = "pass3-openai-secret-key"
ANTHROPIC_KEY = "pass3-anthropic-key"


class Provider(BaseHTTPRequestHandler):
    requests = []
    lock = threading.Lock()

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["content-length"])))
        with self.lock:
            self.requests.append((self.path, request, dict(self.headers.items())))
        body = None
        if self.path.endswith("/embeddings"):
            texts = request["input"]
            texts = texts if isinstance(texts, list) else [texts]
            response = {
                "data": [
                    {"index": i, "embedding": [] if text == "empty vector" else [1.0, float(len(text))]}
                    for i, text in enumerate(texts)
                ]
            }
        elif self.path.endswith("/messages"):
            # Anthropic: extended thinking used the whole token limit, so no text block was produced.
            response = {
                "content": [{"type": "thinking", "thinking": "still thinking", "signature": "x"}],
                "stop_reason": "max_tokens",
                "usage": {"input_tokens": 10, "output_tokens": 4096},
            }
        else:
            prompt = request["messages"][-1]["content"]
            if "NULLLENGTH" in prompt:
                response = {
                    "choices": [
                        {"message": {"content": None, "reasoning_content": "thinking"}, "finish_reason": "length"}
                    ],
                    "usage": {"prompt_tokens": 10, "completion_tokens": 20, "total_tokens": 30},
                }
            elif "RECURSIVE" in prompt:
                response = {"choices": [{"message": {"content": json.dumps({"a": 1, "child": {"a": 2}})}}]}
            elif "BIGBODY" in prompt:
                response = None
                body = ("x" * 100000 + self.headers.get("Authorization", "")).encode()
            else:
                response = {"choices": [{"message": {"content": "ok"}}]}
        data = body if body is not None else json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_):
        pass


def run(duckdb_path):
    server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_port}"
    provider_keys = ("OPENAI_", "ANTHROPIC_", "DUCKDB_AI_")
    base_env = {k: v for k, v in os.environ.items() if not k.startswith(provider_keys)}
    base_env["DUCKDB_AI_ALLOWED_HOSTS"] = "127.0.0.1"
    openai_env = dict(base_env, OPENAI_API_KEY=OPENAI_KEY, OPENAI_BASE_URL=base_url)
    setup = """
SET threads = 1;
SET duckdb_ai_provider = 'openai';
SET duckdb_ai_model = 'pass3-model';
SET duckdb_ai_task_model = 'pass3-model';
SET duckdb_ai_embedding_model = 'pass3-embedding';
SET duckdb_ai_timeout_seconds = 5;
SET duckdb_ai_retry_count = 0;
"""
    command = [str(duckdb_path), "-batch", "-bail", "-csv", "-noheader", "-init", os.devnull]
    checks = 0

    def execute(sql, env=openai_env):
        Provider.requests.clear()
        result = subprocess.run(command, input=(setup + sql).encode(), env=env, capture_output=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stderr.decode())
        return list(csv.reader(io.StringIO(result.stdout.decode(), newline="")))

    def embedding_inputs():
        return [request["input"] for path, request, _ in Provider.requests if path.endswith("/embeddings")]

    try:
        # An empty input is rejected before any request. It must not take the rows that share its batch with it.
        rows = execute(
            "SELECT ai_embed(t, on_error := 'null') IS NULL "
            "FROM (VALUES (1, ''), (2, 'hello'), (3, 'world')) v(i, t) ORDER BY i;"
        )
        assert rows == [["true"], ["false"], ["false"]], rows
        assert embedding_inputs() == [["hello", "world"]], embedding_inputs()
        checks += 1

        rows = execute(
            "SELECT ai_similarity(a, b, on_error := 'null') IS NULL "
            "FROM (VALUES (1, '', 'x'), (2, 'hello', 'world')) v(i, a, b) ORDER BY i;"
        )
        assert rows == [["true"], ["false"]], rows
        assert embedding_inputs() == [["hello", "world"]], embedding_inputs()
        checks += 1

        # Without on_error := 'null' the empty input still fails the statement, and nothing is sent.
        Provider.requests.clear()
        failed = subprocess.run(
            command,
            input=(setup + "SELECT ai_embed(t) FROM (VALUES (''), ('hello')) v(t);").encode(),
            env=openai_env,
            capture_output=True,
            timeout=30,
        )
        assert failed.returncode != 0 and b"ai_embed input must not be empty" in failed.stderr, failed.stderr
        assert embedding_inputs() == [], embedding_inputs()
        checks += 1

        # A batch may not return an empty vector for one input; the single-input parser already rejects it.
        rows = execute(
            "SELECT ai_embed(t, on_error := 'null') IS NULL FROM (VALUES ('empty vector'), ('hello')) v(t);"
            "SELECT count(*) FROM ai_usage() WHERE error LIKE '%empty embedding%';"
        )
        assert rows[:2] == [["true"], ["true"]] and int(rows[2][0]) >= 1, rows
        checks += 1

        # A reply cut at the token limit with no text block reports the limit and keeps the billed tokens.
        rows = execute(
            "SELECT (ai_try_complete('NULLLENGTH')).error;"
            "SELECT prompt_tokens, completion_tokens FROM ai_usage() WHERE function_name = 'ai_try_complete';"
        )
        assert "max_tokens was reached" in rows[0][0], rows
        assert rows[1] == ["10", "20"], rows
        checks += 1

        anthropic_env = dict(base_env, ANTHROPIC_API_KEY=ANTHROPIC_KEY, ANTHROPIC_BASE_URL=base_url)
        rows = execute(
            "SELECT (ai_try_complete('think', provider := 'anthropic', model := 'pass3-claude')).error;"
            "SELECT prompt_tokens, completion_tokens FROM ai_usage() WHERE function_name = 'ai_try_complete';",
            anthropic_env,
        )
        assert "max_tokens was reached" in rows[0][0], rows
        assert rows[1] == ["10", "4096"], rows
        checks += 1

        # An unreadable 200 body is quoted in the error bounded and without the provider key.
        rows = execute(
            "SELECT (ai_try_complete('BIGBODY')).error;"
            "SELECT length(error), contains(error, '" + OPENAI_KEY + "') FROM ai_usage();"
        )
        assert len(rows[0][0]) < 8192 and OPENAI_KEY not in rows[0][0] and "more bytes" in rows[0][0], rows[0][0][-200:]
        assert int(rows[1][0]) < 8192 and rows[1][1] == "false", rows[1]
        checks += 1

        # A lateral ai_prep_search names its provider per row. The key of the session's provider secret must not be
        # sent to the provider a row selects.
        lateral_env = dict(base_env, ANTHROPIC_BASE_URL=base_url)
        Provider.requests.clear()
        result = subprocess.run(
            command,
            input=(
                setup
                + f"CREATE SECRET session_openai (TYPE duckdb_ai, AI_PROVIDER 'openai', API_KEY '{SECRET_KEY}');"
                + "SELECT error FROM (SELECT 'hello world' AS body) d, "
                + "ai_prep_search(d.body, enrich := true, provider := 'anthropic', model := 'pass3-claude', "
                + "on_error := 'null');"
            ).encode(),
            env=lateral_env,
            capture_output=True,
            timeout=30,
        )
        assert result.returncode == 0, result.stderr.decode()
        assert "requires an API key" in result.stdout.decode(), result.stdout.decode()
        sent = json.dumps([headers for _, _, headers in Provider.requests])
        assert SECRET_KEY not in sent, sent
        checks += 1

        # A named secret is honored per row as well: a missing one is reported, not replaced by another key.
        result = subprocess.run(
            command,
            input=(
                setup
                + "SELECT * FROM (SELECT 'hello world' AS body) d, "
                + "ai_prep_search(d.body, enrich := true, secret := 'missing_secret');"
            ).encode(),
            env=openai_env,
            capture_output=True,
            timeout=30,
        )
        assert result.returncode != 0 and b'AI secret "missing_secret" was not found' in result.stderr, result.stderr
        checks += 1

        # A context limit smaller than one character must not split it: the request would carry invalid UTF-8,
        # which this mock refuses to parse.
        rows = execute(
            "SELECT ai_agg(x, 'summarize', max_context_chars := 1, on_error := 'null') FROM (VALUES ('é')) v(x);"
            "SELECT count(*) FROM ai_usage() WHERE status = 'error';"
        )
        chat = [request for path, request, _ in Provider.requests if path.endswith("/chat/completions")]
        assert rows == [["ok"], ["0"]] and len(chat) == 1, (rows, chat)
        checks += 1

        # The per-row path still completes: the row's secret supplies the key and the context is returned.
        rows = execute(
            f"CREATE SECRET lateral_secret (TYPE duckdb_ai, AI_PROVIDER 'openai', API_KEY '{SECRET_KEY}', "
            f"BASE_URL '{base_url}');"
            "SELECT chunk_to_embed, error FROM (SELECT 'hello world' AS body) d, "
            "ai_prep_search(d.body, enrich := true, secret := 'lateral_secret');",
            base_env,
        )
        assert rows[-1] == ["Document context: ok\nContent:\nhello world", "NULL"], rows
        assert [headers.get("Authorization") for _, _, headers in Provider.requests] == ["Bearer " + SECRET_KEY]
        checks += 1

        # A recursive response schema binds, and the recursive property comes back as JSON text.
        schema = '{"type":"object","properties":{"a":{"type":"integer"},"child":{"$ref":"#"}}}'
        rows = execute(f"SELECT a, child FROM ai_complete_record('RECURSIVE', '{schema}');")
        assert rows[0][0] == "1" and json.loads(rows[0][1]) == {"a": 2}, rows
        checks += 1

        # The Cloudflare decision model is a URL path segment; a name that changes the route is rejected.
        cloudflare_env = dict(base_env, CLOUDFLARE_API_KEY="pass3-cf-key", CLOUDFLARE_ACCOUNT_ID="acct")
        for model in ("clef/../../x", "clef?x=1", "clef#x", "clef%2e"):
            Provider.requests.clear()
            result = subprocess.run(
                command,
                input=(
                    setup + "SELECT ai_decide('state', {team: MAP {'a': 'A', 'b': 'B'}}, provider := 'cloudflare', "
                    f"model := '{model}');"
                ).encode(),
                env=cloudflare_env,
                capture_output=True,
                timeout=30,
            )
            assert result.returncode != 0 and b"is not a valid model name" in result.stderr, result.stderr
            assert not Provider.requests
        checks += 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print(f"repository pass 3 smoke passed ({checks} checks)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path(__file__).resolve().parents[2] / "build/release/duckdb")
    args = parser.parse_args()
    run(args.duckdb)
