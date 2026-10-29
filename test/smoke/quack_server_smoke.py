#!/usr/bin/env python3
"""Run AI functions on a Quack server and check that remote clients cannot redirect its credentials."""

import argparse
import json
import os
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOKEN = "quack-smoke-token"
OWNER_KEY = "owner-key"
CREDENTIAL_ERROR = "is not the host the API key from the duckdb_ai secret is configured for"


class MockProvider:
    """OpenAI-compatible chat endpoint that records the Authorization header of every request."""

    def __init__(self):
        self.authorizations = []
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                outer.authorizations.append(self.headers.get("Authorization"))
                payload = json.dumps(
                    {
                        "choices": [{"message": {"content": "served remotely"}, "finish_reason": "stop"}],
                        "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
                    }
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def hermetic_env(**extra):
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("DUCKDB_AI_") and not key.endswith("_API_KEY") and key not in ("AI_AGENT", "CLAUDECODE")
    }
    env.update(extra)
    return env


def wait_for_port(port, server):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise AssertionError("Quack server exited early:\n" + server.stderr.read())
        try:
            with socket.create_connection(("localhost", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.2)
    raise AssertionError("Quack server did not start listening")


def run_sql(duckdb, sql, env):
    return subprocess.run(
        [str(duckdb), "-batch", "-bail", "-json", "-init", os.devnull],
        input=sql,
        text=True,
        capture_output=True,
        env=env,
        timeout=60,
    )


def last_result(stdout):
    """Decode the result of the final statement; earlier statements print their own JSON."""
    decoder = json.JSONDecoder()
    text = stdout.strip()
    position = 0
    result = None
    while position < len(text):
        result, position = decoder.raw_decode(text, position)
        while position < len(text) and text[position].isspace():
            position += 1
    return result


def check_quack(duckdb):
    provider = MockProvider()
    quack_port = free_port()
    # Only the server knows the key.
    server_script = f"""
        INSTALL httpfs;
        LOAD httpfs;
        INSTALL quack;
        LOAD quack;
        CREATE SECRET owner (TYPE duckdb_ai, AI_PROVIDER 'openai', API_KEY '{OWNER_KEY}',
            BASE_URL 'http://127.0.0.1:{provider.port}/v1');
        CALL quack_serve('quack:localhost:{quack_port}', token = '{TOKEN}');
    """
    server = subprocess.Popen(
        [str(duckdb), "-batch", "-bail", "-init", os.devnull, "-cmd", server_script],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=hermetic_env(),
    )
    client_env = hermetic_env()
    try:
        # The CLI keeps serving while it waits for more input on stdin.
        wait_for_port(quack_port, server)

        def client(statement):
            # The server step installed httpfs; Quack's HTTP client and crypto come from it.
            connect = f"LOAD httpfs; ATTACH 'quack:localhost:{quack_port}' AS srv (TOKEN '{TOKEN}'); CONNECT srv;"
            return run_sql(duckdb, connect + statement, client_env)

        # The call runs on the server with the server's secret; the client holds no key.
        result = client(
            "SELECT ai_complete('hello', provider := 'openai', model := 'mock-model', cache := false, retry_count := 0) AS r;"
        )
        assert result.returncode == 0, result.stderr
        assert last_result(result.stdout) == [{"r": "served remotely"}], result.stdout
        assert provider.authorizations == [f"Bearer {OWNER_KEY}"], provider.authorizations

        # A client cannot send the server's key to a host it chooses, even after widening its own allowlist.
        provider.authorizations.clear()
        result = client(
            "SET duckdb_ai_allowed_hosts = '*';"
            f"SELECT ai_complete('hello', provider := 'openai', model := 'mock-model', base_url := 'http://localhost:{provider.port}/v1',"
            " cache := false, retry_count := 0);"
        )
        assert result.returncode != 0 and CREDENTIAL_ERROR in result.stderr, (
            result.returncode,
            result.stdout,
            result.stderr,
        )
        assert provider.authorizations == [], provider.authorizations

    finally:
        server.stdin.close()
        try:
            server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            server.kill()
        provider.close()
    return 2


def check_local_guards(duckdb):
    provider = MockProvider()
    base_url = f"http://127.0.0.1:{provider.port}/v1"
    # Persistent secrets in the home directory would be read after file access is disabled.
    secret = (
        "SET allow_persistent_secrets = false;"
        f"CREATE SECRET owner (TYPE duckdb_ai, AI_PROVIDER 'openai', API_KEY '{OWNER_KEY}', BASE_URL '{base_url}');"
    )
    call = "SELECT ai_complete('hello', provider := 'openai', model := 'mock-model', cache := false, retry_count := 0) AS r;"
    try:
        # A locked-down instance reaches a provider only through allowed_directories.
        result = run_sql(
            duckdb,
            secret + f"SET allowed_directories = ['{base_url}/']; SET enable_external_access = false;" + call,
            hermetic_env(),
        )
        assert result.returncode == 0, result.stderr
        assert last_result(result.stdout) == [{"r": "served remotely"}], result.stdout

        provider.authorizations.clear()
        result = run_sql(duckdb, secret + "SET enable_external_access = false;" + call, hermetic_env())
        assert result.returncode != 0 and "enable_external_access is false" in result.stderr, result.stderr
        assert provider.authorizations == [], provider.authorizations

        # A session cannot widen the DUCKDB_AI_ALLOWED_HOSTS environment variable.
        result = run_sql(
            duckdb,
            "SET duckdb_ai_allowed_hosts = '*';"
            f"SELECT ai_complete('hello', provider := 'ollama', model := 'm', base_url := 'http://localhost:{provider.port}',"
            " cache := false, retry_count := 0);",
            hermetic_env(DUCKDB_AI_ALLOWED_HOSTS="127.0.0.1"),
        )
        assert result.returncode != 0, result.stdout
        assert "not allowed by the DUCKDB_AI_ALLOWED_HOSTS environment variable" in result.stderr, result.stderr
        assert provider.authorizations == [], provider.authorizations
    finally:
        provider.close()
    return 3


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=ROOT / "build/release/duckdb")
    args = parser.parse_args()
    checks = check_local_guards(args.duckdb)
    checks += check_quack(args.duckdb)
    print(f"Quack server smoke passed ({checks} checks)")


if __name__ == "__main__":
    main()
