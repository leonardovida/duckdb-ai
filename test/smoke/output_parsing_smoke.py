#!/usr/bin/env python3
"""Pin how model replies become output columns: lenient label, boolean and score parsing, JSON extraction,
schema-to-type mapping, decision answers, SQL extraction and usage rows. No network calls leave localhost."""
import json
import os
import re
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MARKER = re.compile(r"<<<(.*?)>>>", re.S)


def find_marker(value):
    if isinstance(value, str):
        match = MARKER.search(value)
        return match.group(1).replace("~NL~", "\n") if match else None
    items = value.values() if isinstance(value, dict) else value if isinstance(value, list) else []
    for item in items:
        found = find_marker(item)
        if found is not None:
            return found
    return None


def run(duckdb_path):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, payload):
            data = payload.encode() if isinstance(payload, str) else json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            marker = find_marker(body)
            if marker and marker.startswith("RAW:"):
                self.reply(marker[4:])
                return
            if self.path == "/api/chat":
                self.reply(
                    {
                        "message": {"role": "assistant", "content": marker or "ok"},
                        "done": True,
                        "done_reason": "stop",
                        "prompt_eval_count": 53,
                        "prompt_eval_cached_count": 52,
                        "eval_count": 2,
                    }
                )
                return
            content, finish = marker or "ok", "stop"
            if content.startswith("LENGTH:"):
                content, finish = content[7:], "length"
            elif content.startswith("DEEP:"):
                depth = int(content[5:])
                content = '{"a":' + "[" * depth + "]" * depth + "}"
            self.reply(
                {
                    "choices": [{"message": {"role": "assistant", "content": content}, "finish_reason": finish}],
                    "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
                }
            )

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DUCKDB_AI_", "OPENAI_", "OLLAMA_"))}
    env.update(DUCKDB_AI_PROVIDER="openai", OPENAI_API_KEY="output-mock-key", OPENAI_BASE_URL=base + "/v1")
    env.update(OLLAMA_BASE_URL=base)

    def query(sql, error=None):
        process = subprocess.run(
            [str(duckdb_path), "-unsigned", "-batch", "-bail", "-json"],
            input="LOAD ai; SET duckdb_ai_cache = false; " + sql,
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

    def value(expression):
        return query(f"SELECT {expression} AS v;")[0]["v"]

    def record(expression):
        # The CLI prints STRUCT values in DuckDB syntax, so expand their fields instead.
        return query(f"SELECT r.* FROM (SELECT {expression} AS r);")[0]

    def reply(text):
        return "'<<<" + text.replace("'", "''") + ">>>'"

    labels = "['billing', 'technical']"
    try:
        # Single labels: markdown, quotes, a "Label:" prefix, punctuation and a trailing explanation.
        for text, expected in [
            ("**Billing**", "billing"),
            ('Label: "technical"', "technical"),
            ("The category: `billing`.", "billing"),
            ("technical~NL~The user reports a crash.", "technical"),
            ("billing - the invoice is wrong", "billing"),
        ]:
            assert value(f"ai_classify({reply(text)}, {labels})") == expected, text
        assert value(f"ai_classify({reply('bug report: crash')}, ['bug', 'bug report'])") == "bug report"
        assert value(f"ai_classify({reply('P1: urgent')}, ['P1: urgent', 'P2'])") == "P1: urgent"
        query(f"SELECT ai_classify({reply('not sure')}, {labels});", error="outside the allowed label set")
        query(f"SELECT ai_classify({reply(' ')}, {labels});", error="empty reply instead of a label")

        # Sentiment returns one of its three labels.
        assert [value(f"ai_sentiment({reply(t)})") for t in ("NEGATIVE", "**Positive.**", "Sentiment: neutral")] == [
            "negative",
            "positive",
            "neutral",
        ]
        query(f"SELECT ai_sentiment({reply('mixed feelings')});", error="ai_sentiment model returned a label")

        # Multi-label replies: unquoted arrays, bare labels, bullet lists and duplicates.
        for text, expected in [
            ("[billing, technical, billing]", "[billing, technical]"),
            ("technical", "[technical]"),
            ("- billing~NL~- technical", "[billing, technical]"),
            ('["Billing", "billing"]', "[billing]"),
            ("```json~NL~[]~NL~```", "[]"),
        ]:
            assert value(f"ai_classify_labels({reply(text)}, {labels})::VARCHAR") == expected, text

        # Booleans: punctuation, markdown, quotes and a reason after the answer.
        for text, expected in [
            ("True.", True),
            ("No, it is about billing", False),
            ("**Yes**", True),
            ('"false"', False),
            ("Answer: yes", True),
        ]:
            assert value(f"ai_filter({reply(text)}, 'x')") is expected, text
        assert value(f"ai_filter({reply('Not sure')}, 'x', on_error := 'null')") is None

        # Scores: JSON, labels and explanations are accepted; hex, exponents and ratios are not.
        for text, expected in [
            ("0.85", 0.85),
            ("**0.7** - relevant", 0.7),
            ("Relevance score: 0.50~NL~~NL~Because...", 0.5),
            ('{"score": 0.4}', 0.4),
            (".5", 0.5),
        ]:
            assert value(f"ai_rerank({reply(text)}, 'q')") == expected, text
        for text in ("0x1p-1", "1e-1", "8/10", "80%", "1.5"):
            assert value(f"ai_rerank({reply(text)}, 'q', on_error := 'null')") is None, text

        # Text outputs are trimmed and ai_extract drops a JSON fence.
        assert value(f"ai_summarize({reply('  short summary  ~NL~')})") == "short summary"
        assert value(f"ai_extract({reply('```json~NL~{\"a\": 1}~NL~```')}, 'a')") == '{"a": 1}'

        # JSON outputs: a fence or a sentence before the JSON is removed.
        assert value(f"ai_complete_json({reply('Here is the JSON: {\"a\": 1}')})") == '{"a": 1}'
        schema = '{"type":"object","properties":{"a":{"type":"string"}}}'
        assert record(f"ai_extract_record({reply('Sure!~NL~```json~NL~{\"a\": \"x\"}~NL~```')}, '{schema}')") == {
            "a": "x"
        }

        # Schemas keep property order, follow $ref, unwrap nullable anyOf and keep mixed types as text.
        assert (
            value(
                f"""typeof(ai_extract_record({reply('{}')},
            '{{"type":"object","properties":{{"zeta":{{"type":"string"}},"alpha":{{"type":"integer"}}}}}}'))"""
            )
            == "STRUCT(zeta VARCHAR, alpha BIGINT)"
        )
        pydantic = (
            '{"$defs":{"Addr":{"type":"object","properties":{"city":{"type":"string"}}}},"type":"object",'
            '"properties":{"age":{"anyOf":[{"type":"integer"},{"type":"null"}]},"addr":{"$ref":"#/$defs/Addr"},'
            '"v":{"type":["string","integer"]},"n":{"type":["integer","number"]}}}'
        )
        assert value(f"typeof(ai_extract_record({reply('{}')}, '{pydantic}'))") == (
            "STRUCT(age BIGINT, addr STRUCT(city VARCHAR), v VARCHAR, n DOUBLE)"
        )
        answer = '{"age": null, "addr": {"city": "Paris"}, "v": 7, "n": 3}'
        assert query(
            f"SELECT r.age, r.addr.city AS city, r.v, r.n FROM (SELECT ai_extract_record({reply(answer)}, '{pydantic}') r);"
        ) == [{"age": None, "city": "Paris", "v": "7", "n": 3.0}]
        query(
            f"""SELECT ai_extract_record({reply('{"a": 99999999999999999999}')}, '{{"type":"object","properties":{{"a":{{"type":"integer"}}}}}}');""",
            error='ai_extract_record field "a" integer is out of BIGINT range',
        )

        # Deeply nested replies fail cleanly instead of overflowing the stack.
        assert value(f"ai_extract_record({reply('DEEP:30000')}, '{schema}', on_error := 'null') IS NULL") is True

        # Reasoning blocks are removed; a reply cut off by max_tokens is an error, and its tokens are still logged.
        assert value(f"ai_complete({reply('<think>hmm</think>~NL~Answer')})") == "Answer"
        query(f"SELECT ai_complete({reply('LENGTH:partial')});", error="max_tokens was reached")
        rows = query(
            f"SELECT ai_complete_json({reply('not json')}, on_error := 'null') AS r; "
            "SELECT status, prompt_tokens, total_tokens FROM ai_usage();"
        )
        assert rows == [{"status": "error", "prompt_tokens": 10, "total_tokens": 15}], rows

        # Decision answers match choices ignoring case and space, and a null confidence is NULL.
        decision = '{"model":"nimble","answers":{"q0":{"type":"choice","choice":"Billing ","confidence":null}}}'
        assert record(
            f"ai_decide({reply('RAW:' + decision)}, {{team: MAP {{'billing': 'b', 'tech': 't'}}}}, provider := 'ollama')"
        ) == {"team": "billing", "team_confidence": None}

        # Generated SQL is found after prose, inside a one-line fence, after a "SQL:" label, and before an explanation.
        for text, expected in [
            ("Here is your query:~NL~```sql~NL~SELECT 42 AS answer;~NL~```~NL~It returns 42.", "SELECT 42 AS answer"),
            ("```sql SELECT 43```", "SELECT 43"),
            ("SQL: SELECT 44;~NL~~NL~This selects 44.", "SELECT 44"),
            ("SELECT 'a;b' AS s; and then some words", "SELECT 'a;b' AS s"),
        ]:
            assert value(f"ai_sql({reply(text)})") == expected, text
        rows = query(
            f"SELECT count(*) AS n FROM ai_query_data({reply('FRM nowhere')}, on_error := 'null'); "
            "SELECT status, error FROM ai_usage();"
        )
        assert rows[0]["status"] == "error" and "not a single read-only SELECT" in rows[0]["error"], rows
        assert not rows[0]["error"].startswith("{"), rows
        query(
            "SELECT * FROM ai_fix_sql('SELECT 1 <<<SELECT 2>>>', error := 'syntax error LINE 7');",
            error="points at line 7 but the query has 1 lines",
        )

        # Usage rows: unknown counts are NULL, characters are counted, and Ollama reports cached prompt tokens.
        # query() returns the last result set, so each check ends with the statement it asserts on.
        no_usage = reply('RAW:{"choices":[{"message":{"content":"é"}}]}')
        rows = query(
            f"SELECT ai_complete({no_usage}) AS r; SELECT total_tokens, estimated_cost_usd FROM ai_usage_summary();"
        )
        assert rows == [{"total_tokens": None, "estimated_cost_usd": None}], rows
        rows = query(
            f"SELECT ai_complete({no_usage}) AS r; SELECT response_chars, prompt_tokens, dimensions FROM ai_usage();"
        )
        assert rows == [{"response_chars": 1, "prompt_tokens": None, "dimensions": None}], rows
        rows = query(
            f"SELECT ai_complete({reply('hello')}, provider := 'ollama', model := 'llama3.2') AS r; "
            "SELECT prompt_tokens, cached_prompt_tokens FROM ai_usage();"
        )
        assert rows == [{"prompt_tokens": 53, "cached_prompt_tokens": 52}], rows
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("Output parsing smoke passed: labels, booleans, scores, JSON, schemas, decisions, SQL, usage")


if __name__ == "__main__":
    run(ROOT / "build/release/duckdb")
