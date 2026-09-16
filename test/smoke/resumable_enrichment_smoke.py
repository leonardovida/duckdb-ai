#!/usr/bin/env python3
"""Deterministic restart, invalidation and boundedness checks for the example."""

import argparse
import csv
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, default=ROOT / "build/release/duckdb")
    args = parser.parse_args()
    calls = []
    interrupted_request = threading.Event()
    release_request = threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            prompt = body["messages"][-1]["content"]
            calls.append(prompt)
            if prompt.startswith("interrupt ") and sum(p.startswith("interrupt ") for p in calls) == 2:
                interrupted_request.set()
                release_request.wait(timeout=30)
                return
            fail = prompt == "transient" and calls.count(prompt) == 1
            payload = (
                {"error": {"message": "temporary failure"}}
                if fail
                else {
                    "choices": [{"message": {"content": "ok:" + prompt}, "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 1, "completion_tokens": 1},
                }
            )
            data = json.dumps(payload).encode()
            self.send_response(503 if fail else 200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source = folder / "input.csv"
            database = folder / "job.duckdb"
            rows = [
                {"source_id": "1", "prompt": "success'quote"},
                {"source_id": "2", "prompt": "transient"},
                {"source_id": "3", "prompt": "untouched"},
            ]

            def run(version="v1", valid=True, batch_size=2, interrupt=False, reverse_columns=False):
                with source.open("w", newline="") as handle:
                    columns = ["prompt", "source_id"] if reverse_columns else ["source_id", "prompt"]
                    writer = csv.DictWriter(handle, fieldnames=columns)
                    writer.writeheader()
                    writer.writerows(rows)
                command = [
                    sys.executable,
                    str(ROOT / "examples/resumable_enrichment.py"),
                    "--duckdb",
                    str(args.duckdb.resolve()),
                    "--database",
                    str(database),
                    "--input",
                    str(source),
                    "--provider",
                    "llama_cpp",
                    "--model",
                    "mock",
                    "--base-url",
                    f"http://127.0.0.1:{server.server_port}/v1",
                    "--config-version",
                    version,
                    "--batch-size",
                    str(batch_size),
                ]
                if interrupt:
                    # The CI smoke runs on POSIX. Kill both the wrapper and DuckDB,
                    # after one response but before the second response/commit.
                    process = subprocess.Popen(
                        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True
                    )
                    try:
                        assert interrupted_request.wait(timeout=30), "second request did not arrive"
                    finally:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.communicate(timeout=10)
                        release_request.set()
                    return
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                assert (result.returncode == 0) == valid, result.stdout + result.stderr

            run()
            assert sorted(calls) == ["success'quote", "transient"], calls
            run(batch_size=1)  # Untouched work must precede a lower-ID failed row.
            assert len(calls) == 3 and calls[-1] == "untouched", calls
            run(batch_size=1)  # New process and connection retries only the failure.
            assert len(calls) == 4 and calls.count("success'quote") == 1, calls
            assert calls.count("transient") == 2 and calls.count("untouched") == 1, calls
            run()
            assert len(calls) == 4, calls
            rows[0]["prompt"] = "changed"
            run()
            assert calls[-1] == "changed" and len(calls) == 5, calls
            run("v2")
            assert len(calls) == 7, calls
            run("v2")
            assert len(calls) == 8, calls
            run("v2")
            assert len(calls) == 8, calls
            rows[0]["prompt"] = "success'quote"
            run(reverse_columns=True)  # Header order and old successful identities must not trigger new calls.
            assert len(calls) == 8, calls
            rows.append(rows[0])
            run(valid=False)
            assert len(calls) == 8, calls
            rows.pop()
            rows[0]["prompt"] = None
            run(valid=False)
            assert len(calls) == 8, calls
            rows.clear()
            run()  # A header-only input is a valid no-op.
            assert len(calls) == 8, calls
            rows.extend(
                [
                    {"source_id": "4", "prompt": "interrupt first"},
                    {"source_id": "5", "prompt": "interrupt second"},
                ]
            )
            run(interrupt=True)
            assert sorted(calls[-2:]) == ["interrupt first", "interrupt second"], calls
            count = subprocess.check_output(
                [
                    str(args.duckdb.resolve()),
                    "-csv",
                    "-noheader",
                    str(database),
                    "SELECT count(*) FROM enrichment_results;",
                ],
                text=True,
            )
            assert count.strip() == "7", count  # Prior commits survive, partial batch does not.
            run()
            assert calls.count("interrupt first") == 2 and calls.count("interrupt second") == 2, calls
            run()
            assert len(calls) == 12, calls
            output = subprocess.check_output(
                [
                    str(args.duckdb.resolve()),
                    "-csv",
                    "-noheader",
                    str(database),
                    "SELECT count(*), sum(attempts), count(*) FILTER (WHERE error IS NOT NULL) FROM enrichment_results;",
                ],
                text=True,
            )
            assert output.strip() == "9,10,0", output
        print(
            "resumable enrichment smoke passed: 12 calls, reopen/retry/invalidation/bounds/input validation/crash rollback"
        )
    finally:
        release_request.set()
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
