#!/usr/bin/env python3
"""Check LIMIT/OFFSET evaluation order with a deterministic stateful provider."""

import argparse
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


def run(duckdb):
    prompts = []

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            prompts.append(request["messages"][-1]["content"])
            # A stateful response makes evaluating skipped rows observable.
            body = json.dumps({"choices": [{"message": {"content": str(len(prompts))}}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = HTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "volatile-offset-dummy"
    projection = f"""
        SELECT i, ai_complete(i::VARCHAR,
            provider := 'openai', model := 'volatile-offset-mock',
            base_url := 'http://127.0.0.1:{server.server_port}',
            max_concurrent_requests := 1, cache := false
        ) AS response
    """

    def execute(source):
        result = subprocess.run(
            [str(duckdb), "-batch", "-bail", "-json", "-init", os.devnull],
            input="SET threads=1;" + projection + source,
            text=True,
            capture_output=True,
            env=env,
            timeout=30,
        )
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)

    try:
        rows = execute("FROM range(10) t(i) LIMIT 1 OFFSET 1;")
        assert rows == [{"i": 1, "response": "2"}], (rows, prompts)
        # Do not prescribe evaluation beyond the rows needed for the result.
        assert prompts[:2] == ["0", "1"], prompts
        prompts.clear()
        rows = execute("FROM (SELECT i FROM range(10) t(i) LIMIT 1 OFFSET 1) input;")
        assert rows == [{"i": 1, "response": "1"}], (rows, prompts)
        assert prompts == ["1"], prompts
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("Volatile LIMIT/OFFSET smoke passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--duckdb",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "build/release/duckdb",
    )
    run(parser.parse_args().duckdb)
