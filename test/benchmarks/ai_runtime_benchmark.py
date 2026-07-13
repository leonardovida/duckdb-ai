#!/usr/bin/env python3
"""Deterministic 1k/10k benchmarks for packed embeddings and similarity deduplication."""

import argparse
import csv
import io
import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class BenchmarkProviderHandler(BaseHTTPRequestHandler):
    lock = threading.Lock()
    request_input_counts = []

    @classmethod
    def reset(cls):
        with cls.lock:
            cls.request_input_counts.clear()

    @classmethod
    def snapshot(cls):
        with cls.lock:
            return list(cls.request_input_counts)

    def do_POST(self):
        if self.path != "/embeddings":
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("content-length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        inputs = request.get("input")
        values = inputs if isinstance(inputs, list) else [inputs]
        with self.lock:
            self.request_input_counts.append(len(values))

        # Keep requests observable long enough for thread/RSS sampling while remaining fast.
        time.sleep(0.002)
        payload = {
            "data": [
                {"embedding": [1.0, 0.0, 0.0] if sum(value.encode("utf-8")) % 2 else [0.0, 1.0, 0.0]}
                for value in values
            ],
            "usage": {"prompt_tokens": 2 * len(values), "total_tokens": 2 * len(values)},
        }
        encoded = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, fmt, *args):
        return


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def process_metrics(pid: int):
    status_path = Path(f"/proc/{pid}/status")
    if status_path.exists():
        rss_kb = 0
        thread_count = 0
        for line in status_path.read_text().splitlines():
            if line.startswith("VmRSS:"):
                rss_kb = int(line.split()[1])
            elif line.startswith("Threads:"):
                thread_count = int(line.split()[1])
        return rss_kb, thread_count

    rss = subprocess.run(
        ["ps", "-o", "rss=", "-p", str(pid)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout.strip()
    threads = subprocess.run(
        ["ps", "-M", "-p", str(pid)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout.splitlines()
    return int(rss or 0), max(0, len(threads) - 1)


def benchmark_sql(rows: int, workload: str, base_url: str) -> str:
    if workload == "unique_embed":
        query = f"""
            SELECT sum(ai_embed(
                'benchmark-value-' || i,
                input_token_price_per_million := 1.0
            )[1])
            FROM range({rows}) AS input(i);
        """
    else:
        query = f"""
            SELECT sum(ai_similarity(
                'query-' || (i % 10),
                'candidate-' || (i % 10),
                input_token_price_per_million := 1.0
            ))
            FROM range({rows}) AS input(i);
        """
    return f"""
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'benchmark-model';
        SET duckdb_ai_embedding_model = 'benchmark-embedding-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 30;
        {query}
        SELECT 'BENCH', coalesce(sum(calls), 0), coalesce(sum(batch_count), 0),
               coalesce(sum(total_tokens), 0), coalesce(sum(estimated_cost_usd), 0),
               coalesce(max(retained_events), 0), coalesce(max(dropped_events), 0)
        FROM ai_usage_summary();
    """


def run_case(duckdb_path: Path, base_url: str, workload: str, rows: int):
    BenchmarkProviderHandler.reset()
    process = subprocess.Popen(
        [str(duckdb_path), "-csv", "-noheader", "-c", benchmark_sql(rows, workload, base_url)],
        cwd=repo_root(),
        env={**os.environ, "OPENAI_API_KEY": "benchmark-key"},
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    started = time.perf_counter()
    peak_rss_kb = 0
    peak_threads = 0
    while process.poll() is None:
        rss_kb, thread_count = process_metrics(process.pid)
        peak_rss_kb = max(peak_rss_kb, rss_kb)
        peak_threads = max(peak_threads, thread_count)
        time.sleep(0.005)
    output = process.stdout.read() if process.stdout else ""
    elapsed_ms = (time.perf_counter() - started) * 1000
    if process.returncode != 0:
        raise RuntimeError(f"benchmark failed for {workload}/{rows}:\n{output}")

    summary = None
    for row in csv.reader(io.StringIO(output)):
        if row and row[0] == "BENCH":
            summary = row
    if summary is None:
        raise RuntimeError(f"benchmark summary missing for {workload}/{rows}:\n{output}")

    request_sizes = BenchmarkProviderHandler.snapshot()
    return {
        "workload": workload,
        "rows": rows,
        "distinct_inputs": rows if workload == "unique_embed" else 20,
        "http_requests": len(request_sizes),
        "largest_batch": max(request_sizes, default=0),
        "wall_ms": round(elapsed_ms, 1),
        "peak_threads": peak_threads,
        "peak_rss_mb": round(peak_rss_kb / 1024, 1),
        "usage_calls": int(summary[1]),
        "usage_batches": int(summary[2]),
        "total_tokens": int(summary[3]),
        "usage_estimated_cost_usd": float(summary[4]),
        "provider_cost_estimate_usd": round(sum(request_sizes) * 2 / 1_000_000, 8),
        "retained_events": int(summary[5]),
        "dropped_events": int(summary[6]),
    }


def print_markdown(results):
    columns = [
        "workload",
        "rows",
        "distinct_inputs",
        "http_requests",
        "largest_batch",
        "wall_ms",
        "peak_threads",
        "peak_rss_mb",
        "provider_cost_estimate_usd",
        "dropped_events",
    ]
    print("| " + " | ".join(columns) + " |")
    print("| " + " | ".join("---" for _ in columns) + " |")
    for result in results:
        print("| " + " | ".join(str(result[column]) for column in columns) + " |")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--duckdb",
        type=Path,
        default=repo_root() / "build" / "release" / "duckdb",
        help="Path to the built DuckDB shell with duckdb_ai linked",
    )
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON")
    args = parser.parse_args()
    if not args.duckdb.exists():
        raise SystemExit(f"{args.duckdb} does not exist; run `GEN=ninja make release` first")

    server = ThreadingHTTPServer(("127.0.0.1", 0), BenchmarkProviderHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        results = [
            run_case(args.duckdb, base_url, workload, rows)
            for workload in ("unique_embed", "similarity_dedup")
            for rows in (1000, 10000)
        ]
    finally:
        server.shutdown()
        thread.join(timeout=5)

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        print_markdown(results)


if __name__ == "__main__":
    main()
