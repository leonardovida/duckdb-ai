#!/usr/bin/env python3
"""Pin model-call counts for chained steps and run the chaining cookbook against a mock provider."""
import json
import os
import re
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TRANSLATIONS = {
    "Le paiement a été débité deux fois.": "The payment was charged twice.",
}


def run(duckdb_path):
    calls = []
    failed_once = set()
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, status, payload):
            data = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path.endswith("/systemone"):
                state = body["state"]
                with lock:
                    calls.append(("decide", state))
                technical = "down" in state
                answers = {}
                for key, question in body["questions"].items():
                    if question["type"] == "noul":
                        answers[key] = {"type": "noul", "noul": 0.9 if technical else 0.1}
                    else:
                        labels = list(question["criteria"])
                        choice = "technical" if technical else "billing"
                        answers[key] = {
                            "type": "choice",
                            "choice": choice,
                            "confidence": 0.8,
                            "probabilities": {label: float(label == choice) for label in labels},
                        }
                self.reply(200, {"model": body["model"], "answers": answers, "usage": {"input_tokens": 10}})
                return
            prompt = body["messages"][-1]["content"]
            if prompt.startswith("Translate this support ticket"):
                text = prompt.split(": ", 1)[1]
                with lock:
                    calls.append(("translate", text))
                    fail = "billing email" in text and text not in failed_once
                    if fail:
                        failed_once.add(text)
                if fail:
                    self.reply(400, {"error": {"message": "mock rejected the request"}})
                    return
                content = TRANSLATIONS.get(text, text)
            elif "fail-me" in prompt:
                with lock:
                    calls.append(("chat", prompt))
                self.reply(400, {"error": {"message": "mock rejected the request"}})
                return
            else:
                kind = "summarize" if body["messages"][0]["role"] == "system" else "chat"
                with lock:
                    calls.append((kind, prompt))
                content = "Escalate: dashboard outage." if kind == "summarize" else "ok:" + prompt
            self.reply(
                200,
                {
                    "choices": [{"message": {"role": "assistant", "content": content}}],
                    "usage": {"prompt_tokens": 5, "completion_tokens": 3, "total_tokens": 8},
                },
            )

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DUCKDB_AI_", "OPENAI_", "OLLAMA_"))}
    env.update(
        DUCKDB_AI_PROVIDER="openai",
        OPENAI_API_KEY="chain-mock-key",
        OPENAI_BASE_URL=base + "/v1",
        OLLAMA_BASE_URL=base,
    )

    def query(sql, database=":memory:"):
        process = subprocess.run(
            [str(duckdb_path), "-unsigned", "-batch", "-bail", "-json", database],
            input="LOAD ai; " + sql,
            text=True,
            capture_output=True,
            env=env,
            timeout=60,
        )
        assert process.returncode == 0, process.stderr
        output = process.stdout.strip()
        rows = []
        while output:
            rows, end = json.JSONDecoder().raw_decode(output)
            output = output[end:].lstrip()
        return rows

    def count(sql, kind="chat"):
        calls.clear()
        query(sql)
        return sum(1 for call_kind, _ in calls if call_kind == kind)

    try:
        # NULL inputs never call the model.
        assert count("SELECT ai_complete(x) FROM (VALUES (NULL::VARCHAR), (NULL)) t(x);") == 0
        # Rows that failed upstream are not sent to the next step.
        calls.clear()
        query(
            """
            CREATE TABLE step1 AS SELECT i, ai_try_complete(p) AS s
            FROM (VALUES (1, 'a'), (2, 'fail-me'), (3, 'b')) t(i, p);
            SELECT i, ai_complete('next ' || s.response) AS r FROM step1;
        """
        )
        assert sum(not prompt.startswith("next ") for _, prompt in calls) == 3, calls
        assert sum(prompt.startswith("next ") for _, prompt in calls) == 2, calls
        # CASE calls only the rows that take the branch.
        assert count("SELECT CASE WHEN i % 2 = 0 THEN ai_complete('x' || i) END FROM range(4) t(i);") == 2
        # A WHERE filter runs before the model call.
        assert count("SELECT ai_complete('x' || i) FROM range(4) t(i) WHERE i < 1;") == 1
        # Reading several fields from a subquery calls once per row.
        assert count("SELECT r.response, r.error FROM (SELECT ai_try_complete('x' || i) AS r FROM range(3) t(i));") == 3

        # The cookbook runs as published; reruns only retry what is missing.
        cookbook = (ROOT / "docs/cookbooks/chain-ai-steps.md").read_text()
        sql = "\n".join(re.findall(r"```sql\n(.*?)```", cookbook, re.S))
        with tempfile.TemporaryDirectory() as tmp:
            database = str(Path(tmp) / "pipeline.duckdb")
            expected_runs = [
                # Ticket 4 is NULL and skipped; ticket 3 fails once in translation.
                {"translate": 3, "decide": 2, "summarize": 1},
                # Only the failed ticket is retried, then triaged; it is not urgent.
                {"translate": 1, "decide": 1, "summarize": 0},
                # Nothing left to do.
                {"translate": 0, "decide": 0, "summarize": 0},
            ]
            usage_by_run = []
            for expected in expected_runs:
                calls.clear()
                usage_by_run.append(query(sql, database))
                seen = {kind: sum(1 for k, _ in calls if k == kind) for kind in expected}
                assert seen == expected, (seen, expected, calls)
            results = query(
                "SET VARIABLE pipeline_version = 'triage-v1'; "
                "SELECT n.ticket_id, n.english, tr.team, e.note, n.error FROM step_normalize n "
                "LEFT JOIN step_triage tr USING (ticket_id, input_hash, version) "
                "LEFT JOIN step_escalation e USING (ticket_id, input_hash, version) ORDER BY n.ticket_id;",
                database,
            )
            assert results == [
                {
                    "ticket_id": 1,
                    "english": "The payment was charged twice.",
                    "team": "billing",
                    "note": None,
                    "error": None,
                },
                {
                    "ticket_id": 2,
                    "english": "The dashboard is down for every user since 9am.",
                    "team": "technical",
                    "note": "Escalate: dashboard outage.",
                    "error": None,
                },
                {
                    "ticket_id": 3,
                    "english": "Can you update the billing email on our account?",
                    "team": "billing",
                    "note": None,
                    "error": None,
                },
            ], results
            # The cookbook's usage query reports each function of the first run.
            assert [(r["function_name"], r["calls"]) for r in usage_by_run[0]] == [
                ("ai_decide", 2),
                ("ai_summarize", 1),
                ("ai_try_complete", 3),
            ], usage_by_run[0]
            assert usage_by_run[2] == [], usage_by_run[2]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("Chaining smoke passed: NULL skip, upstream failure skip, CASE, WHERE, subquery fields, cookbook reruns")


if __name__ == "__main__":
    run(ROOT / "build/release/duckdb")
