#!/usr/bin/env python3
"""Credential-free native API round trips, including tool state and SSE."""
import json
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


def run(duckdb_path):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append((self.path, dict(self.headers), body))
            if body.get("model") == "http-error":
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b'{"error":{"message":"bad request"}}')
                return
            if body.get("stream"):
                event = {"choices": [{"delta": {"reasoning_content": "reason", "tool_calls": []}}]}
                response = "data: " + json.dumps(event) + "\r\n\r\n"
                if body.get("model") == "stream-error":
                    response += 'data: {"type":"error"}\n\n'
                elif self.path.endswith("messages"):
                    response += 'data: {"type":"message_stop"}\n\n'
                elif self.path.endswith("responses"):
                    response += 'data: {"type":"response.completed"}\n\n'
                elif body.get("model") != "truncated":
                    response += "data: [DONE]\r\n\r\n"
            else:
                response = json.dumps({"echo": body, "choices": [{"message": {"content": "answer"}}]})
                if self.path.endswith("embeddings"):
                    response = json.dumps(
                        {
                            "echo": body,
                            "data": [{"index": 1, "embedding": [0.0, 1.0]}, {"index": 0, "embedding": [1.0, 0.0]}],
                            "usage": {"prompt_tokens": 2},
                        }
                    )
                elif self.path.endswith("rerank"):
                    response = json.dumps(
                        {
                            "echo": body,
                            "results": [{"index": 1, "relevance_score": 0.9}, {"index": 0, "relevance_score": 0.1}],
                        }
                    )
                if body.get("model") == "tool-only":
                    response = json.dumps(
                        {"choices": [{"message": body["messages"][0], "finish_reason": "tool_calls"}]}
                    )
                elif body.get("model") == "json-error":
                    response = '{"error":{"message":"application error"}}'
                elif body.get("model") == "invalid-json":
                    response = '<html>upstream unavailable</html>'
            encoded = response.encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

    server = HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"

    def query(provider, body, api="chat", fail=False, extra=""):
        # All credentials here are fixed mock values, never real provider secrets.
        endpoint = base if api == "chat" else base + "/native/" + api
        payload = json.dumps(body).replace("'", "''")
        sql = f"""
            CREATE SECRET test_ai (TYPE duckdb_ai, AI_PROVIDER '{provider}', API_KEY 'mock-key');
            SELECT ai_provider_call('{payload}', provider := '{provider}', secret := 'test_ai',
                api := '{api}', base_url := '{endpoint}' {extra}) AS response;
        """
        result = subprocess.run(
            [str(duckdb_path), "-json", "-bail", "-c", sql], text=True, capture_output=True, timeout=20
        )
        if fail:
            assert result.returncode != 0, result.stdout
            return
        assert result.returncode == 0, result.stderr
        return json.loads(json.loads(result.stdout.strip().splitlines()[-1])[0]["response"])

    try:
        # Caller-supplied state survives unchanged, including large integers and tool IDs.
        body = {
            "model": "explicit-model",
            "messages": [
                {
                    "role": "assistant",
                    "reasoning_content": "reason",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": "call1",
                            "type": "function",
                            "function": {"name": "lookup", "arguments": '{"id":9007199254740993}'},
                        }
                    ],
                },
                {"role": "tool", "tool_call_id": "call1", "content": "result"},
            ],
            "thinking": {"type": "enabled"},
            "tools": [],
            "seed": 9007199254740993,
        }
        for provider in ("deepseek", "qwen", "zai", "kimi", "minimax", "hunyuan", "xiaomi"):
            assert query(provider, body)["echo"] == body
            path, headers, _ = requests[-1]
            assert path.endswith("/chat/completions")
            if provider == "xiaomi":
                assert headers.get("api-key") == "mock-key"
                assert "Authorization" not in headers
            else:
                assert headers.get("Authorization") == "Bearer mock-key"
        for model in ("hy3", "hy4-preview"):
            assert query("hunyuan", {**body, "model": model})["echo"]["model"] == model
        api_bodies = {
            "messages": {"model": "qwen3.8-max", "max_tokens": 32, "messages": [{"role": "user", "content": "hello"}]},
            "responses": {"model": "qwen3.8-max", "input": "hello", "max_output_tokens": 32},
            "embeddings": {"model": "text-embedding-v4", "input": ["alpha", "beta"], "dimensions": 256},
            "rerank": {"model": "qwen3-rerank", "query": "alpha", "documents": ["alpha", "beta"], "top_n": 1},
            "fim": {"model": "deepseek-chat", "prompt": "def f():", "suffix": "return 1"},
        }
        for api, native_body in api_bodies.items():
            result = query("deepseek" if api == "fim" else "qwen", native_body, api)
            assert result["echo"] == native_body
            if api == "embeddings":
                assert [entry["index"] for entry in result["data"]] == [1, 0]
                assert result["data"][1]["embedding"] == [1.0, 0.0]
            elif api == "rerank":
                assert [entry["index"] for entry in result["results"]] == [1, 0]
                assert result["results"][0]["relevance_score"] == 0.9
            assert requests[-1][0] == "/native/" + api
        for provider in ("qwen", "zai", "kimi", "minimax", "hunyuan"):
            assert query(provider, api_bodies["messages"], "messages")["echo"] == api_bodies["messages"]
            headers = requests[-1][1]
            if provider in ("zai", "kimi"):
                assert headers.get("Authorization") == "Bearer mock-key"
            else:
                assert headers.get("x-api-key") == "mock-key"
        assert query("mimo", body, "messages")["echo"] == body
        assert requests[-1][1].get("api-key") == "mock-key"
        assert requests[-1][1].get("anthropic-version") == "2023-06-01"
        events = query("deepseek", {**body, "stream": True})["events"]
        assert events[0]["choices"][0]["delta"]["reasoning_content"] == "reason"
        query("deepseek", {**body, "stream": True, "model": "truncated"}, fail=True)
        for api in ("messages", "responses"):
            assert query("hunyuan", {**api_bodies[api], "stream": True}, api)["events"]
        for model in ("http-error", "json-error", "invalid-json"):
            query("deepseek", {**body, "model": model}, fail=True)
        query("deepseek", {**body, "model": "stream-error", "stream": True}, fail=True)
        assert query("deepseek", {**body, "model": "tool-only"})["choices"][0]["message"] == body["messages"][0]
        before = len(requests)
        query("deepseek", {"messages": []}, fail=True)
        query("deepseek", {**body, "api_key": "forbidden"}, fail=True)
        query("deepseek", body, fail=True, extra=", temperature := 0.2")
        assert len(requests) == before
    finally:
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()


if __name__ == "__main__":
    import sys

    run(Path(sys.argv[1] if len(sys.argv) > 1 else "build/release/duckdb").resolve())
    print("provider API smoke passed")
