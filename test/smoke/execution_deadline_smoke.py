#!/usr/bin/env python3
"""Check that model calls stop at DuckDB's max_execution_time instead of waiting for the provider."""

import argparse
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEADLINE_ERROR = "Query exceeded maximum execution time"


class SlowServer:
    def __init__(self):
        self.mode = "slow"
        self.requests = 0
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                self.rfile.read(int(self.headers.get("Content-Length", "0")))
                owner.requests += 1
                if owner.mode == "slow":
                    time.sleep(10)
                    status, payload = 200, b'{"choices":[{"message":{"content":"too late"}}]}'
                else:
                    status, payload = 503, b'{"error":{"message":"busy"}}'
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                except OSError:
                    pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    @property
    def base_url(self):
        return f"http://127.0.0.1:{self.httpd.server_port}"

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def run_until_deadline(duckdb, server, mode, options):
    server.mode = mode
    server.requests = 0
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_") and key != "AI_AGENT"}
    env["OPENAI_API_KEY"] = "deadline-dummy"
    sql = (
        "SET max_execution_time = 1000; "
        f"SELECT ai_complete('deadline input', provider := 'openai', model := 'deadline-model', "
        f"base_url := '{server.base_url}', cache := false, {options});"
    )
    started = time.monotonic()
    result = subprocess.run(
        [str(duckdb), "-batch", "-bail", "-init", os.devnull],
        input=sql,
        text=True,
        capture_output=True,
        env=env,
        timeout=60,
    )
    elapsed = time.monotonic() - started
    assert result.returncode != 0, (mode, result.stdout)
    assert DEADLINE_ERROR in result.stderr, (mode, result.stderr)
    assert elapsed < 6, (mode, elapsed)
    assert server.requests >= 1, (mode, server.requests)
    return elapsed


def run(duckdb):
    server = SlowServer()
    try:
        # A request in flight is aborted by the transfer progress callback.
        in_flight = run_until_deadline(duckdb, server, "slow", "retry_count := 0, timeout_seconds := 60")
        # A retry backoff sleep stops at the deadline instead of finishing every attempt.
        backoff = run_until_deadline(duckdb, server, "busy", "retry_count := 5, retry_backoff_ms := 3000")
    finally:
        server.close()
    print(f"Execution deadline smoke passed (in flight {in_flight:.1f}s, backoff {backoff:.1f}s)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=ROOT / "build/release/duckdb")
    run(parser.parse_args().duckdb)
