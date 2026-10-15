#!/usr/bin/env python3
"""Pin that ai_* functions accept DuckDB native types, columns, macros and prepared parameters."""
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = '{"type":"object","properties":{"a":{"type":"string"}}}'


def run(duckdb_path):
    requests = []
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, payload):
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            with lock:
                requests.append(body)
            if self.path.endswith("/systemone"):
                answers = {}
                for key, question in body["questions"].items():
                    if question["type"] == "noul":
                        answers[key] = {"type": "noul", "noul": 0.9}
                    elif question["type"] == "score":
                        answers[key] = {"type": "score", "score": 1, "confidence": 0.7}
                    else:
                        label = next(iter(question["criteria"]))
                        answers[key] = {"type": "choice", "choice": label, "confidence": 0.8}
                self.reply({"model": body["model"], "answers": answers})
                return
            if self.path.endswith("/embeddings"):
                self.reply({"data": [{"index": 0, "embedding": [1.0, 0.0]}]})
                return
            system = body["messages"][0]["content"] if body["messages"][0]["role"] == "system" else ""
            if "labels:" in system:
                content = system.split("labels: ", 1)[1].split(", ")[0].strip('"').rstrip(".")
                if "JSON array" in system:
                    content = json.dumps([content])
            elif "response_format" in body:
                content = '{"a": "mock"}'
            else:
                content = "ok:" + body["messages"][-1]["content"]
            self.reply({"choices": [{"message": {"role": "assistant", "content": content}}]})

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DUCKDB_AI_", "OPENAI_", "OLLAMA_"))}
    env.update(
        DUCKDB_AI_PROVIDER="openai", OPENAI_API_KEY="types-mock-key", OPENAI_BASE_URL=base + "/v1", OLLAMA_BASE_URL=base
    )

    def query(sql, error=None):
        process = subprocess.run(
            [str(duckdb_path), "-unsigned", "-batch", "-bail", "-json"],
            input="LOAD ai; " + sql,
            text=True,
            capture_output=True,
            env=env,
            timeout=60,
        )
        if error:
            assert process.returncode != 0 and error in process.stderr, (sql, process.stderr)
            return None
        assert process.returncode == 0, (sql, process.stderr)
        output = process.stdout.strip()
        rows = []
        while output:
            rows, end = json.JSONDecoder().raw_decode(output)
            output = output[end:].lstrip()
        return rows

    def value(sql):
        return list(query(sql)[0].values())[0]

    def record(expression, setup=""):
        # The CLI prints STRUCT values in DuckDB syntax, so expand their fields instead.
        return query(f"{setup} SELECT r.* FROM (SELECT {expression} AS r);")[0]

    def last_request(sql):
        requests.clear()
        query(sql)
        return requests[-1]

    try:
        # Columns and macro parameters in positional slots are values, not option names.
        assert (
            last_request("SELECT ai_complete(p, m) FROM (VALUES ('hi', 'col-model')) v(p, m);")["model"] == "col-model"
        )
        assert last_request("SELECT ai_complete(p, model) FROM (VALUES ('hi', 'm2')) v(p, model);")["model"] == "m2"
        assert last_request("CREATE MACRO c(x, mdl) AS ai_complete(x, mdl); SELECT c('hi', 'm3');")["model"] == "m3"
        query("SELECT ai_complete('hi', max_token := 5);", error='Unsupported AI option "max_token"')
        query("SELECT ai_complete('hi', bogus := NULL);", error='Unsupported AI option "bogus"')

        # Prepared parameters and constant macro arguments work wherever a constant is required.
        assert last_request("PREPARE p AS SELECT ai_complete($1, model := $2); EXECUTE p('hi', 'm4');")["model"] == "m4"
        assert query(
            f"PREPARE p AS SELECT r.* FROM (SELECT ai_extract_record($1, $2) AS r); EXECUTE p('hi', '{SCHEMA}');"
        ) == [{"a": "mock"}]
        assert query(
            "PREPARE p AS SELECT r.* FROM (SELECT ai_decide('x', $1, provider := 'ollama') AS r); "
            "EXECUTE p({u: MAP {'true': 'y', 'false': 'n'}});"
        ) == [{"u": 0.9}]
        assert (
            last_request("CREATE MACRO t(x, mt) AS ai_complete(x, max_tokens := mt); SELECT t('a', 10);")[
                "max_completion_tokens"
            ]
            == 10
        )
        assert record("e('a', '" + SCHEMA + "')", "CREATE MACRO e(x, s) AS ai_extract_record(x, s);") == {"a": "mock"}

        # Label lists can be arrays, JSON arrays or maps of label to description.
        assert value("SELECT ai_classify('x', ['billing', 'tech']::VARCHAR[2]) AS r;") == "billing"
        assert value("""SELECT ai_classify('x', '["billing", "tech"]') AS r;""") == "billing"
        request = last_request("SELECT ai_classify('x', MAP {'billing': 'Payments', 'tech': 'Bugs'});")
        assert "- billing: Payments" in request["messages"][0]["content"], request
        assert value("SELECT ai_classify_labels('x', ['billing', 'tech']::VARCHAR[2])[1] AS r;") == "billing"
        query("SELECT ai_classify('x', {a: 1});", error="labels must be a list of labels")

        # Bad label sets fail before any request is sent.
        requests.clear()
        query("SELECT ai_classify('x', ['billing', NULL]);", error="must not contain empty or NULL values")
        query("SELECT ai_classify('x', []::VARCHAR[]);", error="must not be empty")
        assert value("SELECT ai_classify('x', ['billing', NULL], on_error := 'null') AS r;") is None
        assert requests == [], requests

        # Decision criteria accept any type that casts to text.
        assert record("ai_decide('x', {u: MAP {true: 'now', false: 'later'}}, provider := 'ollama')") == {"u": 0.9}
        assert record("ai_decide('x', {l: ['lo', 'hi']::VARCHAR[2]}, provider := 'ollama')") == {
            "l": 1.0,
            "l_confidence": 0.7,
        }

        # Named aggregate instructions.
        request = last_request("SELECT ai_agg(v, instruction := 'list them') FROM (VALUES ('a')) t(v);")
        assert "list them" in json.dumps(request), request

        # A column of documents can be chunked with a lateral join; options still validate at bind time.
        rows = query(
            "CREATE TABLE docs AS SELECT * FROM (VALUES (1, 'alpha beta gamma'), (2, NULL), (3, 'one two')) t(id, body); "
            "SELECT d.id, count(*) AS chunks FROM docs d, ai_generate_chunks(d.body, chunk_size := 6) c "
            "GROUP BY d.id ORDER BY d.id;"
        )
        assert [r["id"] for r in rows] == [1, 3] and all(r["chunks"] > 1 for r in rows), rows
        assert value("SELECT count(*) AS n FROM ai_generate_chunks('First paragraph. Second.', chunk_size := 20);") == 2
        query(
            "CREATE TABLE d2 AS SELECT 'abc' AS body; SELECT * FROM d2, ai_generate_chunks(d2.body, chunk_sise := 3);",
            error="must use named arguments",
        )

        # Non-text inputs are sent as text.
        assert last_request("SELECT ai_complete(42);")["messages"][-1]["content"] == "42"
        row_prompt = last_request("SELECT ai_complete(t) FROM (SELECT 1 AS id, 'charged twice' AS body) t;")
        assert row_prompt["messages"][-1]["content"] == "{'id': 1, 'body': charged twice}", row_prompt
        assert value("SELECT ai_embed(DATE '2026-01-02')[1] AS r;") == 1.0

        # Nested values for JSON options become JSON.
        request = last_request("SELECT ai_complete('x', request_options := {top_p: 0.5, stop: ['END']});")
        assert request["top_p"] == 0.5 and request["stop"] == ["END"], request
        assert record("ai_extract_record('x', {type: 'object', properties: {a: {type: 'string'}}})") == {"a": "mock"}
        assert value("SELECT metadata FROM ai_prep_search('abc', metadata := {uri: '/x'});") == {"uri": "/x"}

        # Record schemas whose property names collide fail at bind time.
        query(
            """SELECT ai_extract_record('x', '{"type":"object","properties":{"Name":{"type":"string"},"name":{"type":"string"}}}');""",
            error="must be unique ignoring case",
        )

        # Table lists accept a single name; NULL options keep their defaults.
        assert value("CREATE TABLE t1(a INT); SELECT count(*) AS n FROM ai_schema_prompt(include_tables := 't1');") == 1
        assert last_request("SELECT ai_complete('x', model := getvariable('unset_model'));")["model"] == "gpt-5.6-luna"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("Native types smoke passed: columns, macros, prepared parameters, labels, criteria, chunks, JSON options")


if __name__ == "__main__":
    run(ROOT / "build/release/duckdb")
