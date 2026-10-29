#!/usr/bin/env python3
"""Exercise provider transport/status retries through every public request path."""

import argparse
import json
import os
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


class RetryServer:
    def __init__(self):
        self.actions = []
        self.requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                raw_body = self.rfile.read(length)
                body = json.loads(raw_body)
                owner.requests.append((self.path, body))
                action = owner.actions.pop(0) if owner.actions else ("success",)
                if action[0].startswith("sleep_"):
                    time.sleep(action[1])
                if action[0] in ("disconnect", "sleep_disconnect"):
                    try:
                        self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    self.connection.close()
                    return

                status = action[1] if action[0] == "status" else 200
                payload = owner.payload(self.path, body, action)
                encoded = payload.encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        self.httpd = HTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    @property
    def base_url(self):
        return f"http://127.0.0.1:{self.httpd.server_port}"

    @staticmethod
    def payload(path, body, action):
        if action[0] == "status":
            return '{"error":{"message":"retry smoke status sentinel"}}'
        if path.endswith("/embeddings"):
            values = body.get("input", [])
            if not isinstance(values, list):
                values = [values]
            return json.dumps(
                {
                    "data": [
                        {"index": index, "embedding": [float(index + 1), 2.0]} for index, _value in enumerate(values)
                    ],
                    "usage": {"prompt_tokens": 3, "total_tokens": 3},
                }
            )
        if path.endswith("/redact"):
            return '{"redacted_text":"masked retry input"}'
        return '{"choices":[{"message":{"content":"recovered retry response"}}]}'

    def reset(self, *actions):
        self.actions = list(actions)
        self.requests.clear()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join()


def _sql_options(base_url, provider, model, cache, retry_count):
    return (
        f"provider := '{provider}', model := '{model}', base_url := '{base_url}', "
        f"cache := {'true' if cache else 'false'}, retry_count := {retry_count}, "
        "retry_backoff_ms := 0, fail_on_error := false"
    )


def _call_sql(kind, base_url, cache, retry_count):
    if kind == "complete":
        options = _sql_options(base_url, "openai", "retry-model", cache, retry_count)
        return "SELECT ai_try_complete('retry input', " + options + ") AS result"
    if kind == "provider_call":
        options = _sql_options(base_url, "openai", "retry-model", cache, retry_count)
        body = '{"model":"retry-model","messages":[{"role":"user","content":"retry input"}]}'
        return (
            "SELECT ai_provider_call('"
            + body
            + "', api := 'chat', "
            + options.replace("model := 'retry-model', ", "")
            + ") AS result"
        )
    if kind == "redact":
        options = _sql_options(base_url, "openai_privacy_filter", "openai/privacy-filter", cache, retry_count)
        return "SELECT ai_redact('retry input', " + options + ") AS result"
    if kind == "embed":
        options = _sql_options(base_url, "openai", "retry-embedding-model", cache, retry_count)
        return "SELECT ai_embed('retry input', " + options + ") AS result"
    if kind == "embed_batch":
        options = _sql_options(base_url, "openai", "retry-embedding-model", cache, retry_count)
        return (
            "SELECT list(result ORDER BY i) AS result FROM ("
            "SELECT i, ai_embed('batch-' || i::VARCHAR, " + options + ") AS result FROM range(3) t(i))"
        )
    raise AssertionError(kind)


def _result_projection(kind):
    if kind == "complete":
        return "result.response AS response, result.error AS error"
    if kind == "embed":
        return "result AS response, NULL::VARCHAR AS error"
    if kind == "embed_batch":
        return "list_count(result) AS response, NULL::VARCHAR AS error"
    return "result AS response, NULL::VARCHAR AS error"


def _run_sql(duckdb, env, sql):
    result = subprocess.run(
        [str(duckdb), "-batch", "-bail", "-json", "-init", os.devnull],
        input="SET threads=1;" + sql,
        text=True,
        capture_output=True,
        env=env,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr
    try:
        # DuckDB emits one JSON array per statement. The setup clear and the optional
        # first cache call are intentionally ignored; the final SELECT is last.
        rows = json.loads([line for line in result.stdout.splitlines() if line][-1])
    except (IndexError, json.JSONDecodeError) as ex:
        raise AssertionError(f"invalid DuckDB JSON output: {result.stdout!r}") from ex
    assert len(rows) == 1, rows
    return rows[0]


def _query(server, duckdb, kind, actions, cache=False, retry_count=2, second_call=False):
    server.reset(*actions)
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "transport-retry-dummy"
    env["OPENAI_PRIVACY_FILTER_API_KEY"] = "transport-retry-privacy-dummy"
    call = _call_sql(kind, server.base_url, cache, retry_count)
    projection = _result_projection(kind)
    sql = "SELECT * FROM ai_clear_usage();"
    if second_call:
        sql += call + ";"
    sql += "CREATE TEMP TABLE call_result AS " + call + ";"
    sql += (
        "SELECT " + projection + ", (SELECT count(*) FROM ai_usage()) AS usage_count, "
        "(SELECT min(u.retries) FROM ai_usage() u) AS usage_min_retries, "
        "(SELECT max(u.retries) FROM ai_usage() u) AS usage_max_retries, "
        "(SELECT min(u.http_status) FROM ai_usage() u) AS usage_min_http_status, "
        "(SELECT max(u.http_status) FROM ai_usage() u) AS usage_max_http_status, "
        "(SELECT min(u.status) FROM ai_usage() u) AS usage_status, "
        "(SELECT min(u.error) FROM ai_usage() u) AS usage_error, "
        "(SELECT sum(u.cache_hit::INTEGER) FROM ai_usage() u) AS usage_cache_hits, "
        "(SELECT arg_max(u.status, u.event_id) FROM ai_usage() u) AS usage_last_status, "
        "(SELECT arg_max(u.cache_hit, u.event_id) FROM ai_usage() u) AS usage_last_cache_hit, "
        "(SELECT request_attempts FROM ai_usage_totals()) AS request_attempts FROM call_result;"
    )
    row = _run_sql(duckdb, env, sql)
    row["usage"] = {
        "count": row.pop("usage_count"),
        "min_retries": row.pop("usage_min_retries"),
        "max_retries": row.pop("usage_max_retries"),
        "min_http_status": row.pop("usage_min_http_status"),
        "max_http_status": row.pop("usage_max_http_status"),
        "status": row.pop("usage_status"),
        "error": row.pop("usage_error"),
        "cache_hits": row.pop("usage_cache_hits"),
        "last_status": row.pop("usage_last_status"),
        "last_cache_hit": row.pop("usage_last_cache_hit"),
    }
    row["request_attempts"] = int(row["request_attempts"])
    return row


def _coalesced_query(server, duckdb):
    server.reset(
        ("sleep_disconnect", 0.15),
        ("sleep_disconnect", 0.15),
        ("sleep_success", 0.15),
    )
    env = {key: value for key, value in os.environ.items() if not key.startswith("DUCKDB_AI_")}
    env["OPENAI_API_KEY"] = "transport-retry-dummy"
    options = _sql_options(server.base_url, "openai", "retry-model", True, 2)
    sql = (
        "SET threads=4; SELECT * FROM ai_clear_usage(); "
        "CREATE TEMP TABLE coalesced AS SELECT ai_try_complete('coalesced retry input', "
        + options
        + ") AS result FROM range(8); "
        "SELECT sum((result.response IS NOT NULL)::INTEGER) AS successes, "
        "(SELECT count(*) FROM ai_usage()) AS usage_count, "
        "(SELECT min(retries) FROM ai_usage()) AS min_retries, "
        "(SELECT max(retries) FROM ai_usage()) AS max_retries, "
        "(SELECT sum(retries) FROM ai_usage()) AS retry_sum, "
        "(SELECT sum(cache_hit::INTEGER) FROM ai_usage()) AS cache_hits, "
        "(SELECT request_attempts FROM ai_usage_totals()) AS request_attempts "
        "FROM coalesced;"
    )
    row = _run_sql(duckdb, env, sql)
    for key in (
        "successes",
        "usage_count",
        "min_retries",
        "max_retries",
        "retry_sum",
        "cache_hits",
        "request_attempts",
    ):
        row[key] = int(row[key])
    return row


def _assert_usage(row, expected_requests, expected_retries, expected_status, expected_http_status=None):
    assert row["request_attempts"] == expected_requests, row
    usage = row["usage"]
    assert usage["count"] > 0, row
    assert usage["min_retries"] == expected_retries and usage["max_retries"] == expected_retries, row
    assert usage["status"] == expected_status and usage["last_status"] == expected_status, row
    if expected_http_status is not None:
        assert (
            usage["min_http_status"] == expected_http_status and usage["max_http_status"] == expected_http_status
        ), row
    return usage


def _assert_complete_error(row, usage):
    error = json.loads(row["error"])
    assert error["exception_type"] == "IO", row
    assert error["exception_message"] == usage["error"], row
    return error["exception_message"]


def _assert_no_response(row, kind):
    response = row["response"]
    assert response == 0 if kind == "embed_batch" else response is None, row


def _assert_response(row, kind):
    response = row["response"]
    assert response > 0 if kind == "embed_batch" else response is not None, row


def run(duckdb):
    server = RetryServer()
    checks = 0
    try:
        # Every public request path must report transport retries, including a zero-retry disconnect.
        for kind in ("complete", "provider_call", "redact", "embed", "embed_batch"):
            row = _query(server, duckdb, kind, [("disconnect",)], retry_count=0)
            assert len(server.requests) == 1, (kind, server.requests)
            usage = _assert_usage(row, 1, 0, "error")
            _assert_no_response(row, kind)
            if kind == "complete":
                error = _assert_complete_error(row, usage)
                assert "AI provider request failed:" in error, row
            checks += 1

            row = _query(server, duckdb, kind, [("disconnect",), ("disconnect",), ("success",)], retry_count=2)
            assert len(server.requests) == 3, (kind, server.requests)
            _assert_usage(row, 3, 2, "ok")
            _assert_response(row, kind)
            checks += 1

            row = _query(
                server,
                duckdb,
                kind,
                [("disconnect",), ("disconnect",), ("disconnect",)],
                retry_count=2,
            )
            assert len(server.requests) == 3, (kind, server.requests)
            usage = _assert_usage(row, 3, 2, "error")
            _assert_no_response(row, kind)
            if kind == "complete":
                error = _assert_complete_error(row, usage)
                assert "AI provider request failed:" in error, row
            checks += 1

            row = _query(
                server,
                duckdb,
                kind,
                [("disconnect",), ("disconnect",), ("disconnect",)],
                cache=True,
                retry_count=2,
            )
            assert len(server.requests) == 3, (kind, server.requests)
            usage = _assert_usage(row, 3, 2, "error")
            _assert_no_response(row, kind)
            if kind == "complete":
                _assert_complete_error(row, usage)
            checks += 1

        # Retryable HTTP status responses are retried, while a client error is returned immediately.
        for kind in ("complete", "provider_call", "redact", "embed", "embed_batch"):
            row = _query(
                server,
                duckdb,
                kind,
                [("status", 400), ("success",)],
                retry_count=2,
            )
            assert len(server.requests) == 1, (kind, server.requests)
            usage = _assert_usage(row, 1, 0, "error", 400)
            _assert_no_response(row, kind)
            if kind == "complete":
                error = _assert_complete_error(row, usage)
                assert "retry smoke status sentinel" in error, row
            checks += 1

            row = _query(
                server,
                duckdb,
                kind,
                [("status", 503), ("status", 503), ("status", 503)],
                retry_count=2,
            )
            assert len(server.requests) == 3, (kind, server.requests)
            usage = _assert_usage(row, 3, 2, "error", 503)
            _assert_no_response(row, kind)
            if kind == "complete":
                error = _assert_complete_error(row, usage)
                assert "HTTP 503" in error, row
            checks += 1

        # A failed response must not poison the response cache. The next identical call reaches the server.
        for kind in ("complete", "redact", "embed"):
            row = _query(
                server,
                duckdb,
                kind,
                [("disconnect",), ("success",)],
                cache=True,
                retry_count=0,
                second_call=True,
            )
            assert len(server.requests) == 2, (kind, server.requests)
            usage = row["usage"]
            assert usage["count"] == 2 and usage["min_retries"] == usage["max_retries"] == 0, row
            _assert_response(row, kind)
            assert usage["last_status"] == "ok" and usage["last_cache_hit"] is False, row
            checks += 1

            row = _query(
                server,
                duckdb,
                kind,
                [("success",)],
                cache=True,
                retry_count=0,
                second_call=True,
            )
            assert len(server.requests) == 1, (kind, server.requests)
            usage = _assert_usage(row, 1, 0, "ok")
            assert usage["last_cache_hit"] is True, row
            checks += 1

        # Concurrent identical cache misses share one owner request. Its two transport
        # retries are counted once; seven cache waiters keep retries at zero.
        row = _coalesced_query(server, duckdb)
        assert len(server.requests) == 3, server.requests
        assert row == {
            "successes": 8,
            "usage_count": 8,
            "min_retries": 0,
            "max_retries": 2,
            "retry_sum": 2,
            "cache_hits": 7,
            "request_attempts": 3,
        }, row
        checks += 1
    finally:
        server.close()
    print(f"transport retry smoke passed ({checks} cases, {len(server.requests)} final requests)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path(__file__).resolve().parents[2] / "build/release/duckdb")
    run(parser.parse_args().duckdb)
