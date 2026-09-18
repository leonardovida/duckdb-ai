#!/usr/bin/env python3
"""Local-only Jev contracts and multi-question request-count regression."""
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def run(duckdb_path):
    requests = []
    attempts = {}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append((self.path, self.headers.get("Authorization"), body))
            marker = body.get("state")
            marker = marker if isinstance(marker, str) else "structured"
            attempts[marker] = attempts.get(marker, 0) + 1
            if marker in ("retry-429", "retry-529") and attempts[marker] == 1:
                self.send_response(int(marker[-3:]))
                self.send_header("Retry-After", "0")
                self.end_headers()
                self.wfile.write(b'{"error":"retry"}')
                return
            answers = {}
            for name, question in body["questions"].items():
                kind = question["type"]
                if kind == "noul":
                    probability = {"false": 0.49, "tie": 0.5, "bad-probability": 1.2}.get(marker, 0.95)
                    answers[name] = {"type": kind, "noul": probability}
                elif kind == "choice":
                    labels = list(question["criteria"])
                    choice = "outside labels" if marker == "bad-label" else labels[0]
                    answers[name] = {
                        "type": kind,
                        "choice": choice,
                        "confidence": 1.0,
                        "probabilities": {label: float(i == 0) for i, label in enumerate(labels)},
                    }
                elif kind == "score":
                    answers[name] = {
                        "type": kind,
                        "score": 1.5,
                        "confidence": 0.7,
                        "legend": dict(enumerate(question["criteria"])),
                        "probabilities": {"0": 0, "1": 0.5, "2": 0.5},
                    }
            if marker == "missing-answer":
                answers = {}
            result = {"model": "jev-1.13.0", "answers": answers, "usage": {"input_tokens": 100, "output_tokens": 20}}
            data = json.dumps(result).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DUCKDB_AI_", "TYPESAFE_"))}
    env.update(TYPESAFE_API_KEY="jev-mock-key", TYPESAFE_BASE_URL=base + "/v1")

    def query(sql, error=None):
        process = subprocess.run(
            [str(duckdb_path), "-unsigned", "-batch", "-bail", "-json"],
            input="LOAD ai; " + sql,
            text=True,
            capture_output=True,
            env=env,
            timeout=30,
        )
        if error:
            assert process.returncode != 0 and error in process.stderr, process.stderr
            return None
        assert process.returncode == 0, process.stderr
        output = process.stdout.strip()
        while output:
            rows, end = json.JSONDecoder().raw_decode(output)
            output = output[end:].lstrip()
        return rows

    def native(state, questions, options=""):
        body = json.dumps({"model": "jev-preview", "state": state, "questions": questions}).replace("'", "''")
        return f"ai_provider_call('{body}', provider := 'typesafe'{options})"

    try:
        rows = query(
            "SELECT ai_classify('charged twice', ['billing, refunds', 'technical'], provider := 'jev') AS label;"
        )
        assert rows == [{"label": "billing, refunds"}], rows
        assert requests[-1][2]["model"] == "jev-latest"
        question = requests[-1][2]["questions"]["classification"]
        assert question["type"] == "choice" and list(question["criteria"]) == ["billing, refunds", "technical"]
        query(
            "SELECT ai_classify('line 1' || chr(10) || 'line 2' || chr(1), ['billing', 'other'], "
            "provider := 'jev', instructions := 'Prefer billing', "
            "label_descriptions := '{\"billing\":\"charges\"}', "
            "examples := '[{\"input\":\"duplicate\",\"label\":\"billing\"}]');"
        )
        sent = requests[-1][2]
        assert sent["state"] == "line 1\nline 2\x01", sent
        instruction = sent["questions"]["classification"]["instructions"]
        assert all(value in instruction for value in ("Prefer billing", "charges", "duplicate")), instruction
        rows = query(
            """
            SELECT ai_filter(input, 'Is urgent?', provider := 'typesafe') AS accepted
            FROM (VALUES ('false'), ('tie'), ('true'), (NULL)) t(input);
        """
        )
        assert rows == [{"accepted": False}, {"accepted": True}, {"accepted": True}, {"accepted": None}], rows
        for marker in ("bad-probability", "missing-answer"):
            rows = query(
                f"SELECT ai_filter('{marker}', 'Is urgent?', provider := 'jev', fail_on_error := false) AS result;"
            )
            assert rows == [{"result": None}], rows
        rows = query(
            "SELECT ai_classify('bad-label', ['billing', 'other'], provider := 'jev', fail_on_error := false) AS result;"
        )
        assert rows == [{"result": None}], rows
        for status in (429, 529):
            rows = query(
                f"SELECT ai_filter('retry-{status}', 'Is urgent?', provider := 'jev', retry_count := 1, retry_backoff_ms := 1) AS result;"
            )
            assert rows == [{"result": True}] and attempts[f"retry-{status}"] == 2, rows

        questions = {
            "urgent": {"type": "noul", "instructions": "Is urgent?"},
            "team": {"type": "choice", "instructions": "Which team?", "criteria": {"billing": None, "other": None}},
            "priority": {
                "type": "score",
                "instructions": "How urgent?",
                "criteria": ["Routine", "Degraded", "Blocked"],
            },
        }
        start = len(requests)
        rows = query("SELECT " + native({"ticket": "charged twice"}, questions) + " AS response;")
        response = json.loads(rows[0]["response"])
        assert set(response["answers"]) == set(questions)
        assert response["model"] == "jev-1.13.0"
        assert len(requests) == start + 1
        assert requests[-1][2]["model"] == "jev-preview", "moving model aliases must pass through"
        for name, question in questions.items():
            query("SELECT " + native({"ticket": "charged twice"}, {name: question}) + " AS response;")
        assert len(requests) == start + 4, "bundled request must replace three single-question requests"
        for endpoint in (base, base + "/v1", base + "/v1/systemone"):
            query("SELECT " + native("url", {"urgent": questions["urgent"]}, f", base_url := '{endpoint}'") + ";")
        assert all(path == "/v1/systemone" and auth == "Bearer jev-mock-key" for path, auth, _ in requests)

        rows = query(
            """
            CREATE SECRET jev_test (TYPE duckdb_ai, AI_PROVIDER 'typesafe', MODEL 'jev-1.13.0');
            SELECT ai_classify('usage', ['billing', 'other'], secret := 'jev_test') AS result;
        """
        )
        assert requests[-1][2]["model"] == "jev-1.13.0"
        # The shell emits one JSON document per result, so project usage in a later statement without printing inference.
        rows = query(
            "CREATE TEMP TABLE done AS SELECT ai_filter('usage-log', 'Is urgent?', provider := 'typesafe'); "
            "SELECT function_name, prompt_tokens, completion_tokens FROM ai_usage();"
        )
        assert rows == [{"function_name": "ai_filter", "prompt_tokens": 100, "completion_tokens": 20}], rows
        start = len(requests)
        query("SELECT ai_complete('write text', provider := 'typesafe');", error="only supports")
        query("SELECT ai_embed('text', provider := 'typesafe');", error="embedding")
        query("SELECT " + native("options", questions, ", temperature := 0.5") + ";", error="generation options")
        assert len(requests) == start, "unsupported calls must fail before HTTP"
        print("jev provider smoke passed (three questions: one request instead of three)")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    run(Path(__file__).resolve().parents[2] / "build" / "release" / "duckdb")
