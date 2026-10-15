#!/usr/bin/env python3
"""Local-only ai_decide contracts for every supported decision-model API."""
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def answer_for(question, marker, include_type):
    kind = question["type"]
    if kind == "noul":
        answer = {"noul": 0.8}
    elif kind == "choice":
        labels = list(question["criteria"])
        choice = "outside labels" if marker == "bad-label" else labels[-1]
        answer = {
            "choice": choice,
            "confidence": 0.9,
            "probabilities": {label: float(label == choice) for label in labels},
        }
    else:
        answer = {
            "score": 1.25,
            "confidence": 0.6,
            "legend": {str(i): level for i, level in enumerate(question["criteria"])},
            "probabilities": {"0": 0.1, "1": 0.55, "2": 0.35},
        }
    if include_type:
        answer["type"] = kind
    return answer


def run(duckdb_path):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append((self.path, self.headers.get("Authorization"), body))
            marker = body.get("state")
            perplexity = self.path.endswith("/decisions")
            cloudflare = "/ai/run/" in self.path
            answers = {
                name: answer_for(question, marker, include_type=not perplexity)
                for name, question in body["questions"].items()
            }
            if marker == "missing-answer":
                answers = {}
            result = {"model": body["model"], "answers": answers, "usage": {"input_tokens": 40, "output_tokens": 0}}
            if cloudflare:
                result = {"result": result, "success": marker != "cf-failure", "errors": [], "messages": []}
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
    prefixes = ("DUCKDB_AI_", "TYPESAFE_", "CLOUDFLARE_", "CF_", "PERPLEXITY_", "OLLAMA_", "SYSTEMONE_")
    env = {k: v for k, v in os.environ.items() if not k.startswith(prefixes)}
    env.update(
        TYPESAFE_API_KEY="jev-mock-key",
        TYPESAFE_BASE_URL=base + "/v1",
        CLOUDFLARE_API_KEY="cf-mock-key",
        CLOUDFLARE_BASE_URL=base + "/client/v4/accounts/acct/ai/v1",
        PERPLEXITY_API_KEY="pplx-mock-key",
        PERPLEXITY_BASE_URL=base,
        OLLAMA_BASE_URL=base,
        OLLAMA_MODEL="qwen3.8:27b",
        SYSTEMONE_API_KEY="s1-mock-key",
        SYSTEMONE_BASE_URL=base + "/v1",
    )

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
        rows = []
        while output:
            rows, end = json.JSONDecoder().raw_decode(output)
            output = output[end:].lstrip()
        return rows

    questions = """{
        team: MAP {'billing': 'Payments and refunds', 'technical': 'Errors and outages'},
        severity: ['Routine', 'Degraded', 'Blocked'],
        urgent: MAP {'true': 'Needs action now', 'false': 'Can wait'}
    }"""
    expected = {
        "team": "technical",
        "team_confidence": 0.9,
        "severity": 1.25,
        "severity_confidence": 0.6,
        "urgent": 0.8,
    }

    def decide(state, options, question_sql=questions):
        sql = f"SELECT d.* FROM (SELECT ai_decide({state}, {question_sql}{options}) AS d);"
        return query(sql)

    try:
        # Single-row providers: the row text is the state and questions keep string instructions.
        cases = [
            ("ollama", "", "/v1/systemone", None, "nimble"),
            ("ollama", ", model := 'tev1'", "/v1/systemone", None, "tev1"),
            ("cloudflare", "", "/client/v4/accounts/acct/ai/run/@cf/cloudflare/clef", "Bearer cf-mock-key", "clef"),
            (
                "cloudflare",
                ", model := '@cf/cloudflare/clef-flash'",
                "/client/v4/accounts/acct/ai/run/@cf/cloudflare/clef-flash",
                "Bearer cf-mock-key",
                "clef-flash",
            ),
            ("perplexity", "", "/v1/decisions", "Bearer pplx-mock-key", "pplx-decider-v1.1-27b"),
            ("systemone", ", model := 'clm-latest'", "/v1/systemone", "Bearer s1-mock-key", "clm-latest"),
        ]
        for provider, extra, path, auth, model in cases:
            requests.clear()
            rows = decide("'Checkout fails for everyone'", f", provider := '{provider}'{extra}")
            assert rows == [expected], (provider, rows)
            assert len(requests) == 1, (provider, requests)
            sent_path, sent_auth, body = requests[0]
            assert sent_path == path, (provider, sent_path)
            assert sent_auth == auth, (provider, sent_auth)
            assert body["model"] == model, (provider, body)
            assert body["state"] == "Checkout fails for everyone", body
            assert list(body["questions"]) == ["q0", "q1", "q2"], body
            assert [q["type"] for q in body["questions"].values()] == ["choice", "score", "noul"], body
            assert all(isinstance(q["instructions"], str) for q in body["questions"].values()), body

        # Single-row providers send one request per non-null row.
        requests.clear()
        rows = query(
            f"""SELECT d.* FROM (SELECT i, ai_decide(t, {questions}, provider := 'perplexity') AS d
            FROM (VALUES (1, 'a'), (2, NULL), (3, 'b')) v(i, t)) ORDER BY i;"""
        )
        assert rows == [expected, dict.fromkeys(expected), expected], rows
        assert sorted(r[2]["state"] for r in requests) == ["a", "b"], requests

        # TypeSafe keeps row batching: three rows share one request.
        requests.clear()
        rows = query(
            f"""SELECT d.* FROM (SELECT ai_decide(t, {questions}, provider := 'typesafe') AS d
            FROM (VALUES ('a'), ('b'), ('c')) v(t));"""
        )
        assert rows == [expected] * 3, rows
        assert len(requests) == 1 and requests[0][0] == "/v1/systemone", requests
        assert requests[0][2]["state"] == "" and len(requests[0][2]["questions"]) == 9, requests
        assert requests[0][2]["model"] == "jev-latest", requests

        # Session provider setting applies; chat model settings do not leak into decision requests.
        requests.clear()
        query(
            "SET duckdb_ai_provider = 'ollama'; SET duckdb_ai_model = 'qwen3.8:27b'; "
            f"SELECT ai_decide('x', {questions}).team AS team;"
        )
        assert requests[-1][0] == "/v1/systemone" and requests[-1][2]["model"] == "nimble", requests

        # A chat model stored in the provider's secret does not leak either; a TypeSafe secret model applies.
        requests.clear()
        query(
            "CREATE SECRET ollama_ai (TYPE duckdb_ai, AI_PROVIDER 'ollama', MODEL 'qwen3.8:27b'); "
            "CREATE SECRET jev_ai (TYPE duckdb_ai, AI_PROVIDER 'typesafe', MODEL 'jev-1.13.0'); "
            f"SELECT ai_decide('x', {questions}, provider := 'ollama').team AS a, "
            f"ai_decide('x', {questions}, secret := 'jev_ai').team AS b;"
        )
        assert sorted(r[2]["model"] for r in requests) == ["jev-1.13.0", "nimble"], requests

        # DUCKDB_AI_MODEL names a chat model, so TypeSafe keeps its own default.
        requests.clear()
        env["DUCKDB_AI_MODEL"] = "gpt-5.6-luna"
        try:
            query(f"SELECT ai_decide('x', {questions}, provider := 'typesafe').team AS team;")
        finally:
            del env["DUCKDB_AI_MODEL"]
        assert requests[-1][2]["model"] == "jev-latest", requests

        # Per-question instructions.
        requests.clear()
        rows = decide(
            "'Refund please'",
            ", provider := 'cloudflare'",
            "{refund: {instructions: 'Is the customer asking for money back?', "
            "criteria: MAP {'true': 'Asks for a refund', 'false': 'Does not'}}}",
        )
        assert rows == [{"refund": 0.8}], rows
        assert requests[0][2]["questions"]["q0"]["instructions"] == "Is the customer asking for money back?"

        # Response validation and error policy.
        rows = decide("'bad-label'", ", provider := 'perplexity', on_error := 'null'")
        assert rows == [dict.fromkeys(expected)], rows
        decide_error = "SELECT ai_decide('cf-failure', {u: MAP {'true': 'y', 'false': 'n'}}, provider := 'cloudflare');"
        query(decide_error, error="success=false")
        query(
            "SELECT ai_decide('missing-answer', {u: MAP {'true': 'y', 'false': 'n'}}, provider := 'ollama');",
            error="missing or mismatched answer",
        )

        # Usage events carry the function, provider, model and tokens.
        rows = query(
            "SELECT ai_decide('x', {u: MAP {'true': 'y', 'false': 'n'}}, provider := 'cloudflare').u AS u; "
            "SELECT function_name, provider, model, prompt_tokens FROM ai_usage();"
        )
        assert rows == [
            {"function_name": "ai_decide", "provider": "cloudflare", "model": "clef", "prompt_tokens": 40}
        ], rows

        # Bind and configuration errors happen before any request.
        requests.clear()
        query("SELECT ai_decide('x', {u: ['a', 'b']}, provider := 'openai');", error="OpenAI has not published")
        query("SELECT ai_decide('x', {u: ['a', 'b']}, provider := 'anthropic');", error="no supported decision")
        query("SELECT ai_decide('x', {u: ['a', 'b']}, provider := 'systemone');", error="requires a model")
        query("SELECT ai_decide('x', {u: {criteria: ['a', 'b'], hint: 'x'}});", error="accepts only instructions")
        query("SELECT ai_decide('x', {u: {instructions: 'Rate it'}});", error="requires criteria")
        query("SELECT ai_jev('x', {u: ['a', 'b']}, provider := 'ollama');", error="Unsupported ai_jev option")
        query("SELECT ai_complete('x', provider := 'systemone', model := 'm');", error="only supports ai_decide")
        assert requests == [], requests

        # Chat functions keep their existing endpoints for providers that also host decision models.
        assert query("SELECT ai_provider_protocol('cloudflare') AS p;") == [{"p": "openai_chat"}]
        assert query("SELECT ai_provider_protocol('systemone') AS p;") == [{"p": "systemone"}]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("Decision provider smoke passed")


if __name__ == "__main__":
    run(Path(__file__).resolve().parents[2] / "build/release/duckdb")
