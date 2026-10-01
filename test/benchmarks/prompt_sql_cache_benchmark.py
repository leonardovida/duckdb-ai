#!/usr/bin/env python3
"""Compare cached SQL binding overhead; only a local mock provider is used."""

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Provider(BaseHTTPRequestHandler):
    requests = 0

    def do_POST(self):
        self.rfile.read(int(self.headers["content-length"]))
        type(self).requests += 1
        body = b'{"choices":[{"message":{"content":"SELECT 42 AS value"}}]}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def measure(binary, base_url, key_chars, entries, hits):
    env = {k: v for k, v in os.environ.items() if not k.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "benchmark-dummy"
    prefix = "cache bound " + "x" * key_chars

    def query(index):
        return f"SELECT * FROM ai_query_data('{prefix}{index:04d}', schema_context := 'SELECT 1');"

    sql = (
        "SET threads=1; SET duckdb_ai_provider='openai'; SET duckdb_ai_model='benchmark-model';"
        f"SET duckdb_ai_base_url='{base_url}';"
        + "".join(query(i) for i in range(entries))
        + "CREATE TEMP TABLE timing AS SELECT epoch_ns(get_current_timestamp()) AS started;"
        + query(entries - 1) * hits
        + "SELECT (epoch_ns(get_current_timestamp()) - started) / 1000000.0 FROM timing;"
    )
    before = Provider.requests
    result = subprocess.run(
        [str(binary), "-batch", "-bail", "-csv", "-noheader", "-init", os.devnull],
        input=sql,
        env=env,
        text=True,
        capture_output=True,
        timeout=60,
    )
    if result.returncode:
        raise RuntimeError(result.stderr)
    requests = Provider.requests - before
    if requests != entries:
        raise AssertionError(f"expected {entries} warmup requests, got {requests}")
    rows = result.stdout.splitlines()
    if rows[:-1] != ["42"] * (entries + hits):
        raise AssertionError("unexpected cached SQL results")
    return {"cached_bind_ms": float(rows[-1]), "provider_requests": requests}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=5)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    report = {
        "method": "1024 warm entries; 2000 repeated binds of the newest entry; timer excludes warmup and process startup",
        "workloads": [],
        "platform": platform.platform(),
        "repetitions": args.repetitions,
        "binary_sha256": {
            label: hashlib.sha256(getattr(args, label).read_bytes()).hexdigest() for label in ("before", "after")
        },
    }
    try:
        for key_chars in (0, 4096):
            rounds = {"before": [], "after": []}
            for repeat in range(args.repetitions):
                order = ("before", "after") if repeat % 2 == 0 else ("after", "before")
                for label in order:
                    rounds[label].append(
                        measure(
                            getattr(args, label).resolve(),
                            f"http://127.0.0.1:{server.server_port}",
                            key_chars,
                            1024,
                            2000,
                        )
                    )
            medians = {
                label: statistics.median(row["cached_bind_ms"] for row in values) for label, values in rounds.items()
            }
            report["workloads"].append(
                {
                    "shared_question_prefix_chars": key_chars,
                    "rounds": rounds,
                    "median_ms": medians,
                    "speedup": medians["before"] / medians["after"],
                }
            )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for workload in report["workloads"]:
        print(f"prefix {workload['shared_question_prefix_chars']}: {workload['median_ms']}, {workload['speedup']:.2f}x")


if __name__ == "__main__":
    main()
