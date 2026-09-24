#!/usr/bin/env python3
"""Exercise the bounded Jev batch evaluation example without live provider calls."""

import csv
import sys
import json
import os
import subprocess
import argparse
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def run(duckdb_override=None):
    root = Path(__file__).resolve().parents[2]
    duckdb = Path(duckdb_override) if duckdb_override else root / "build/release/duckdb"
    script = root / "examples/jev_batch_evaluation.py"
    requests = []
    logs = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/log":
                logs.append(body)
                self.send_response(200)
                self.end_headers()
                return
            requests.append(body)
            questions = body["questions"]
            records = [question["instructions"]["record"] for question in questions.values()]
            if any(record.startswith("row-10 ") for record in records):
                self.send_response(503)
                self.end_headers()
                self.wfile.write(b'{"error":"deliberate smoke failure"}')
                return
            answers = {}
            # Reverse answer insertion order to prove generated IDs, not row order, are used.
            for key, question in reversed(list(questions.items())):
                record = question["instructions"]["record"]
                labels = list(question["criteria"])
                number = int(record.split()[0].split("-")[1])
                choice = labels[number % 2]
                # Deliberately disagree with the baseline for one non-null candidate row.
                if number == 0 and len(questions) > 1:
                    choice = labels[1]
                answers[key] = {
                    "type": "choice",
                    "choice": choice,
                    "confidence": 0.9,
                    "probabilities": {label: float(label == choice) for label in labels},
                }
            response = {"model": "jev-1.13.0", "answers": answers}
            # Keep one successful event's usage absent to prove partial totals are reported as unknown.
            if records != ["row-0 café"]:
                response["usage"] = {"input_tokens": 10, "output_tokens": 2}
            payload = json.dumps(response).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"
    env = os.environ.copy()
    env.update(
        TYPESAFE_API_KEY="smoke-secret-must-not-be-printed",
        TYPESAFE_BASE_URL=endpoint + "/v1",
        DUCKDB_AI_LOG_ENDPOINT=endpoint + "/log",
        DUCKDB_AI_LOG_INCLUDE_TEXT="1",
    )

    def invoke(input_path, criteria_path, allow_live=True, model="jev-1.13.0"):
        command = [
            sys.executable,
            str(script),
            "--duckdb",
            str(duckdb),
            "--input",
            str(input_path),
            "--criteria",
            str(criteria_path),
            "--model",
            model,
        ]
        if allow_live:
            command.append("--allow-live")
        return subprocess.run(command, text=True, capture_output=True, env=env, timeout=120)

    try:
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            input_path = directory / "tickets.csv"
            with input_path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=["id", "text", "label"])
                writer.writeheader()
                for number in range(33):
                    label = "a'b" if number % 2 == 0 else "b"
                    text = f"row-{number}"
                    if number == 0:
                        text += " café"
                    if number == 10:
                        text += " fail"
                    writer.writerow({"id": str(number), "text": text, "label": label})
            criteria_path = directory / "criteria.json"
            criteria_path.write_text(json.dumps({"a'b": "Première équipe", "b": "Second team"}), encoding="utf-8")

            refused = invoke(input_path, criteria_path, allow_live=False)
            assert refused.returncode != 0 and not requests, refused.stderr

            completed = invoke(input_path, criteria_path)
            assert completed.returncode == 0, completed.stdout + completed.stderr
            assert "smoke-secret" not in completed.stdout
            assert not logs, "inherited usage log endpoint must be disabled"
            report = json.loads(completed.stdout)
            assert report["row_cap"] == 1000 and report["rows"] == 33
            assert report["model"] == "jev-1.13.0"
            assert len(report["input_sha256"]) == 64 and len(report["criteria_sha256"]) == 64
            runs = {run["batch_size"]: run for run in report["runs"]}
            assert sorted(len(body["questions"]) for body in requests) == sorted(
                [1] * 33 + [8, 8, 8, 8, 1] + [16, 16, 1] + [32, 1]
            )
            assert [run["usage"]["request_operations"] for run in report["runs"]] == [33, 5, 3, 2]
            assert runs[1]["metrics"] == {
                "rows": 33,
                "correct": 32,
                "failed": 1,
                "accuracy": 32 / 33,
                "accuracy_denominator": 33,
                "failures_in_denominator": 1,
            }
            assert runs[8]["metrics"]["failed"] == 8
            assert runs[8]["metrics"]["accuracy"] == 24 / 33
            assert runs[8]["agreement_to_batch_1"] == {
                "compared_non_null_predictions": 25,
                "agreed": 24,
                "agreement": 24 / 25,
                "baseline_coverage": 32,
                "candidate_coverage": 25,
            }
            assert runs[1]["usage"]["tokens_complete"] is False, runs[1]["usage"]
            assert runs[1]["usage"]["prompt_tokens"] is None
            assert runs[1]["usage"]["usage_events"] == 33
            assert runs[1]["usage"]["prompt_token_events"] == 31
            assert all(run["elapsed_query_wall_ms"] >= 0 for run in report["runs"])
            assert runs[1]["rows"][10]["prediction"] is None

            # Missing usage is encoded as -1 by ai_usage(), even on successful calls.
            for text, complete in (("row-0 café", False), ("row-2", True)):
                single = directory / "single.csv"
                single.write_text(f"id,text,label\n0,{text},a'b\n", encoding="utf-8")
                checked = invoke(single, criteria_path)
                assert checked.returncode == 0, checked.stderr
                for result in json.loads(checked.stdout)["runs"]:
                    assert result["usage"]["failures"] == 0
                    assert result["usage"]["tokens_complete"] is complete
                    assert result["usage"]["prompt_tokens"] == (10 if complete else None)
                    assert result["usage"]["total_tokens"] == (12 if complete else None)

            duplicate = directory / "duplicate.csv"
            duplicate.write_text("id,text,label\n0,ok,a'b\n0,again,a'b\n", encoding="utf-8")
            requests.clear()
            invalid = invoke(duplicate, criteria_path)
            assert invalid.returncode != 0 and not requests and "duplicate id" in invalid.stderr
            capped = directory / "capped.csv"
            capped.write_text("id,text,label\n" + "".join(f"{n},ok,a'b\n" for n in range(1001)), encoding="utf-8")
            invalid = invoke(capped, criteria_path)
            assert invalid.returncode != 0 and not requests and "1000-row cap" in invalid.stderr
            empty = directory / "empty.csv"
            empty.write_text("id,text,label\n0,,a\n", encoding="utf-8")
            invalid = invoke(empty, criteria_path)
            assert invalid.returncode != 0 and not requests
            extra = directory / "extra.csv"
            extra.write_text("id,text,label,extra\n0,ok,a'b,nope\n", encoding="utf-8")
            invalid = invoke(extra, criteria_path)
            assert invalid.returncode != 0 and not requests
            whitespace = directory / "whitespace.csv"
            whitespace.write_text("id,text,label\n 0,ok,a'b\n", encoding="utf-8")
            invalid = invoke(whitespace, criteria_path)
            assert invalid.returncode != 0 and not requests
            noul = directory / "noul.json"
            noul.write_text('{"true":"yes","false":"no"}', encoding="utf-8")
            invalid = invoke(input_path, noul)
            assert invalid.returncode != 0 and not requests
            too_many = directory / "too-many.json"
            too_many.write_text(json.dumps({str(number): "label" for number in range(256)}), encoding="utf-8")
            invalid = invoke(input_path, too_many)
            assert invalid.returncode != 0 and not requests
            duplicate_criteria = directory / "duplicate.json"
            duplicate_criteria.write_text('{"a":"one","a":"two"}', encoding="utf-8")
            invalid = invoke(input_path, duplicate_criteria)
            assert invalid.returncode != 0 and not requests
            invalid = invoke(input_path, criteria_path, model="   ")
            assert invalid.returncode != 0 and not requests
        print("jev batch evaluation smoke passed: validation, opt-in, request counts, failures, agreement")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path)
    run(parser.parse_args().duckdb)
