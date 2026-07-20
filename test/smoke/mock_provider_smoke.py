#!/usr/bin/env python3
import argparse
import json
import os
import ssl
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer, ThreadingHTTPServer
from pathlib import Path


class MockProviderHandler(BaseHTTPRequestHandler):
    request_lock = threading.Lock()
    completion_requests = []
    embedding_requests = []
    privacy_filter_requests = []
    ollama_requests = []
    claude_requests = []
    log_requests = []
    control_plane_requests = []
    control_plane_authorization_headers = []
    authorization_headers = []
    openrouter_referer_headers = []
    openrouter_title_headers = []
    xai_conversation_headers = []
    claude_api_keys = []
    claude_versions = []
    retry_completion_attempts = 0
    active_cap_one_requests = 0
    max_cap_one_requests = 0
    active_grow_pool_requests = 0
    max_grow_pool_requests = 0

    @classmethod
    def reset(cls):
        cls.completion_requests.clear()
        cls.embedding_requests.clear()
        cls.privacy_filter_requests.clear()
        cls.ollama_requests.clear()
        cls.claude_requests.clear()
        cls.log_requests.clear()
        cls.control_plane_requests.clear()
        cls.control_plane_authorization_headers.clear()
        cls.authorization_headers.clear()
        cls.openrouter_referer_headers.clear()
        cls.openrouter_title_headers.clear()
        cls.xai_conversation_headers.clear()
        cls.claude_api_keys.clear()
        cls.claude_versions.clear()
        cls.retry_completion_attempts = 0
        cls.active_cap_one_requests = 0
        cls.max_cap_one_requests = 0
        cls.active_grow_pool_requests = 0
        cls.max_grow_pool_requests = 0

    def do_POST(self):
        length = int(self.headers.get("content-length", "0"))
        body = self.rfile.read(length).decode("utf-8")

        if self.path.endswith("/chat/completions"):
            self.authorization_headers.append(self.headers.get("authorization"))
            if self.headers.get("x-openrouter-title") or self.headers.get("http-referer"):
                self.openrouter_referer_headers.append(self.headers.get("http-referer"))
                self.openrouter_title_headers.append(self.headers.get("x-openrouter-title"))
            if self.headers.get("x-grok-conv-id"):
                self.xai_conversation_headers.append(self.headers.get("x-grok-conv-id"))
            request = json.loads(body)
            self.completion_requests.append(request)
            prompt = request["messages"][-1]["content"]
            full_prompt = "\n".join(message["content"] for message in request["messages"])
            concurrency_group = None
            if prompt.startswith("cap one "):
                concurrency_group = "cap_one"
            elif prompt.startswith("grow pool "):
                concurrency_group = "grow_pool"
            if concurrency_group:
                with self.request_lock:
                    active_name = f"active_{concurrency_group}_requests"
                    max_name = f"max_{concurrency_group}_requests"
                    active = getattr(MockProviderHandler, active_name) + 1
                    setattr(MockProviderHandler, active_name, active)
                    setattr(MockProviderHandler, max_name, max(getattr(MockProviderHandler, max_name), active))
                time.sleep(0.05)
            if "force provider error" in prompt:
                self._send_json(
                    {
                        "error": {
                            "message": "credential test-key is invalid",
                            "type": "authentication_error",
                            "code": "invalid_api_key",
                        }
                    },
                    status=401,
                )
                return
            if prompt == "retry once":
                MockProviderHandler.retry_completion_attempts += 1
                if MockProviderHandler.retry_completion_attempts == 1:
                    self._send_json(
                        {
                            "error": {
                                "message": "temporary provider failure",
                                "type": "server_error",
                                "code": "temporary_unavailable",
                            }
                        },
                        status=500,
                    )
                    return
            if prompt == "cache dogpile":
                time.sleep(0.2)
            content = "mock completion"
            if prompt == "retry once":
                content = "mock retry completion"
            elif prompt == "cache dogpile":
                content = "mock dogpile completion"
            elif prompt == "oversized response":
                content = "x" * 2048
            elif "EMPTY_INTERMEDIATE" in prompt and "larger SQL aggregate" in prompt:
                content = ""
            elif "NON_CONVERGING" in prompt and "larger SQL aggregate" in prompt:
                content = ("NON_CONVERGING" + "X" * 40)[:40]
            elif "return invalid schema JSON" in prompt:
                content = '{"summary":123}'
            elif "return constrained schema JSON" in prompt:
                content = '{"summary":"mock structured","score":0.5,"tags":["duck","ai"]}'
            elif "return scalar record JSON object" in prompt:
                content = '{"summary":"mock scalar record","profile":{"company":"DuckDB Labs"}}'
            elif "return record JSON object" in prompt:
                content = (
                    '{"summary":"mock record","score":0.75,"tags":["duck","sql"],'
                    '"profile":{"company":"DuckDB Labs","metrics":{"score":42,"active":true}},'
                    '"refs":[{"label":"docs","weight":1},{"label":"repo","weight":2}]}'
                )
            elif "return length violation JSON" in prompt:
                content = '{"summary":"bad"}'
            elif "return numeric violation JSON" in prompt:
                content = '{"score":2}'
            elif "return array violation JSON" in prompt:
                content = '{"tags":["duck","duck"]}'
            elif "return oneOf violation JSON" in prompt:
                content = '{"a":1,"b":2}'
            elif "Score how well the input satisfies the supplied criteria" in full_prompt:
                content = '{"score":0.91}'
            elif request.get("response_format") or "Return only valid JSON" in full_prompt:
                content = '{"summary":"mock structured"}'
            elif "Explain the DuckDB SQL query" in full_prompt:
                content = "This query returns a single row with 42."
            elif "Correct the DuckDB SQL query" in full_prompt:
                content = "```sql\nSELECT 42\n```"
            elif "Correct one line in the DuckDB SQL query" in full_prompt:
                content = "SELECT 42"
            elif "Generate one DuckDB SQL SELECT statement" in full_prompt and "self correct count" in full_prompt:
                content = "SELECT broken_column FROM smoke_fix_missing"
            elif "Generate one DuckDB SQL SELECT statement" in full_prompt:
                content = "```sql\nSELECT 42\n```"
            elif "Return only a JSON array of chosen labels" in full_prompt:
                content = '["outside"]' if "return unknown labels" in prompt else '["billing, overdue","performance"]'
            elif "class_a" in full_prompt and "class_b" in full_prompt:
                content = "class_a" if "alpha" in prompt else "class_b"
            elif "Classify the following text into exactly one" in full_prompt:
                content = "outside" if "return unknown label" in prompt else "billing, overdue"
            elif "Score candidate relevance to the search query" in full_prompt:
                content = "0.82"
            payload = {
                "choices": [{"message": {"content": content}}],
                "usage": {
                    "prompt_tokens": 7,
                    "completion_tokens": 3,
                    "total_tokens": 10,
                },
            }
            if concurrency_group:
                with self.request_lock:
                    active_name = f"active_{concurrency_group}_requests"
                    setattr(
                        MockProviderHandler,
                        active_name,
                        getattr(MockProviderHandler, active_name) - 1,
                    )
            self._send_json(payload)
            return

        if self.path == "/embeddings":
            self.authorization_headers.append(self.headers.get("authorization"))
            request = json.loads(body)
            self.embedding_requests.append(request)
            inputs = request.get("input")
            input_count = len(inputs) if isinstance(inputs, list) else 1
            if isinstance(inputs, list) and len(inputs) > 2 and any("split batch" in value for value in inputs):
                self._send_json({"error": {"message": "payload too large"}}, status=413)
                return

            def embedding(value):
                if "alpha" in value:
                    return [1.0, 0.0, 0.0]
                if "beta" in value:
                    return [0.0, 1.0, 0.0]
                if "ambiguous" in value:
                    return [1.0, 1.0, 0.0]
                return [0.25, -0.5, 1.25]

            input_values = inputs if isinstance(inputs, list) else [inputs]
            usage_tokens = 5 if any("uneven usage" in value for value in input_values) else 2 * input_count
            payload = {
                "data": [{"embedding": embedding(value)} for value in input_values],
                "usage": {
                    "prompt_tokens": usage_tokens,
                    "total_tokens": usage_tokens,
                },
            }
            self._send_json(payload)
            return

        if self.path == "/redact":
            self.authorization_headers.append(self.headers.get("authorization"))
            request = json.loads(body)
            self.privacy_filter_requests.append(request)
            self._send_json({"redacted_text": "email [PRIVATE_EMAIL] token [SECRET]"})
            return

        if self.path == "/api/chat":
            self.ollama_requests.append(json.loads(body))
            payload = {
                "message": {"role": "assistant", "content": "mock ollama completion"},
                "prompt_eval_count": 5,
                "eval_count": 4,
            }
            self._send_json(payload)
            return

        if self.path == "/messages":
            self.claude_api_keys.append(self.headers.get("x-api-key"))
            self.claude_versions.append(self.headers.get("anthropic-version"))
            self.claude_requests.append(json.loads(body))
            payload = {
                "content": [{"type": "text", "text": "mock claude completion"}],
                "usage": {
                    "input_tokens": 9,
                    "output_tokens": 4,
                },
            }
            self._send_json(payload)
            return

        if self.path == "/log":
            self.log_requests.append(json.loads(body))
            self._send_json({"ok": True})
            return

        if self.path.startswith("/v1/endpoints/"):
            request = json.loads(body)
            self.control_plane_requests.append((self.path, request))
            self.control_plane_authorization_headers.append(self.headers.get("authorization"))
            if self.path == "/v1/endpoints/provision":
                self._send_json(
                    {
                        "operation_id": "provision-op",
                        "status": "submitted",
                        "estimated_hourly_cost_usd": 1.25,
                    }
                )
            elif self.path == "/v1/endpoints/status":
                self._send_json(
                    {
                        "operation_id": request["operation_id"],
                        "status": "ready",
                        "endpoint_url": "https://endpoint.example/v1",
                    }
                )
            else:
                self._send_json({"operation_id": "deprovision-op", "status": "submitted"})
            return

        if self.path == "/v1/documents/parse":
            request = json.loads(body)
            self.control_plane_requests.append((self.path, request))
            self.control_plane_authorization_headers.append(self.headers.get("authorization"))
            if request.get("parser_profile") == "malformed":
                self._send_json({"document_id": "document-malformed", "unexpected": []})
                return
            if request.get("parser_profile") == "error-only":
                self._send_json({"document_id": "document-error", "error": "remote parser failed"})
                return
            self._send_json(
                {
                    "document_id": "document-1",
                    "elements": [
                        {
                            "page": 1,
                            "element_index": 0,
                            "element_type": "paragraph",
                            "text": "DuckDB document",
                            "markdown": "DuckDB document",
                            "bbox": [0, 0, 10, 10],
                            "confidence": 0.99,
                            "metadata": {"language": "en"},
                        }
                    ],
                }
            )
            return

        self.send_response(404)
        self.end_headers()

    def log_message(self, fmt, *args):
        return

    def _send_json(self, payload, status=200):
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run_duckdb_tls_request(duckdb_path: Path, base_url: str, ca_environment: dict[str, str], expect_success: bool):
    sql = f"""
        SELECT ai_embed(
            'tls trust smoke',
            provider := 'openai',
            model := 'mock-embedding',
            base_url := '{base_url}'
        )[1] AS first_embedding_value;
    """
    env = os.environ.copy()
    for name in ("CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR"):
        env.pop(name, None)
    env["OPENAI_API_KEY"] = "test-key"
    env.update(ca_environment)
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if expect_success and result.returncode != 0:
        raise AssertionError(f"duckdb TLS trust smoke exited with {result.returncode}\n{result.stdout}")
    if not expect_success and result.returncode == 0:
        raise AssertionError(f"duckdb TLS trust smoke unexpectedly succeeded\n{result.stdout}")
    return result


def run_duckdb_tls_log(
    duckdb_path: Path, provider_base_url: str, log_endpoint: str, ca_environment: dict[str, str]
) -> str:
    sql = f"""
        SET duckdb_ai_log_endpoint = '{log_endpoint}';
        SET duckdb_ai_log_strict = true;
        SELECT ai_complete(
            'tls log smoke',
            provider := 'openai',
            model := 'mock-completion',
            base_url := '{provider_base_url}'
        ) AS completion;
    """
    env = os.environ.copy()
    for name in ("CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR"):
        env.pop(name, None)
    env["OPENAI_API_KEY"] = "test-key"
    env.update(ca_environment)
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb TLS log smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def run_tls_smoke_suite(duckdb_path: Path, provider_base_url: str):
    with tempfile.TemporaryDirectory() as tls_directory:
        tls_directory = Path(tls_directory)
        tls_certificate = tls_directory / "server.pem"
        tls_key = tls_directory / "server-key.pem"
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-sha256",
                "-nodes",
                "-days",
                "1",
                "-subj",
                "/CN=127.0.0.1",
                "-addext",
                "subjectAltName=IP:127.0.0.1",
                "-keyout",
                str(tls_key),
                "-out",
                str(tls_certificate),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        tls_server = HTTPServer(("127.0.0.1", 0), MockProviderHandler)
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.minimum_version = ssl.TLSVersion.TLSv1_2
        tls_context.load_cert_chain(tls_certificate, tls_key)
        tls_server.socket = tls_context.wrap_socket(tls_server.socket, server_side=True)
        tls_thread = threading.Thread(target=tls_server.serve_forever, daemon=True)
        tls_thread.start()
        try:
            tls_base_url = f"https://127.0.0.1:{tls_server.server_address[1]}"
            certificate_path = str(tls_certificate)

            ssl_cert_result = run_duckdb_tls_request(
                duckdb_path,
                tls_base_url,
                {"SSL_CERT_FILE": certificate_path},
                expect_success=True,
            )
            if "first_embedding_value" not in ssl_cert_result.stdout or "0.25" not in ssl_cert_result.stdout:
                raise AssertionError(f"SSL_CERT_FILE output missing embedding\n{ssl_cert_result.stdout}")

            curl_bundle_result = run_duckdb_tls_request(
                duckdb_path,
                tls_base_url,
                {
                    "CURL_CA_BUNDLE": certificate_path,
                    "SSL_CERT_FILE": str(tls_directory / "missing-ca.pem"),
                },
                expect_success=True,
            )
            if "0.25" not in curl_bundle_result.stdout:
                raise AssertionError(f"CURL_CA_BUNDLE output missing embedding\n{curl_bundle_result.stdout}")

            run_duckdb_tls_request(duckdb_path, tls_base_url, {}, expect_success=False)
            run_duckdb_tls_request(
                duckdb_path,
                f"https://localhost:{tls_server.server_address[1]}",
                {"SSL_CERT_FILE": certificate_path},
                expect_success=False,
            )

            MockProviderHandler.reset()
            tls_log_output = run_duckdb_tls_log(
                duckdb_path,
                provider_base_url,
                f"{tls_base_url}/log",
                {"SSL_CERT_FILE": certificate_path},
            )
            if "mock completion" not in tls_log_output or len(MockProviderHandler.log_requests) != 1:
                raise AssertionError(f"HTTPS usage log smoke failed\n{tls_log_output}")
        finally:
            tls_server.shutdown()
            tls_thread.join(timeout=5)


def run_duckdb(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT * FROM ai_clear_usage();
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'mock-model';
        SET duckdb_ai_completion_model = 'mock-completion-model';
        SET duckdb_ai_task_model = 'mock-task-model';
        SET duckdb_ai_aggregate_model = 'mock-aggregate-model';
        SET duckdb_ai_embedding_model = 'mock-embedding-default';
        SET duckdb_ai_sql_assistant_model = 'mock-sql-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_log_endpoint = '{base_url}/log';
        SET duckdb_ai_log_strict = true;
        SET duckdb_ai_timeout_seconds = 5;
        CREATE OR REPLACE SECRET smoke_duckdb_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'openai'
        );
        CREATE OR REPLACE SECRET smoke_privacy_filter_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'openai_privacy_filter',
            BASE_URL '{base_url}',
            MODEL 'openai/privacy-filter'
        );
        CREATE OR REPLACE SECRET smoke_databricks_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'databricks',
            BASE_URL '{base_url}',
            MODEL 'databricks-gpt-oss-120b'
        );
        CREATE OR REPLACE SECRET smoke_snowflake_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'snowflake',
            BASE_URL '{base_url}',
            MODEL 'claude-sonnet-4-5'
        );
        SELECT ai_complete(
            'hello from smoke',
            system_prompt := 'answer briefly',
            temperature := 0.2,
            max_tokens := 17,
            log_tags := 'smoke-run',
            max_concurrent_requests := 1,
            min_request_interval_ms := 0,
            input_token_price_per_million := 1.0,
            output_token_price_per_million := 2.0
        ) AS completion;
        SELECT ai_complete_json(
            'return a smoke JSON object',
            response_schema := '{{"type":"object","properties":{{"summary":{{"type":"string"}}}},"required":["summary"],"additionalProperties":false}}'
        ) AS structured_completion;
        SELECT ai_complete_json(
            'return invalid schema JSON',
            response_schema := '{{"type":"object","properties":{{"summary":{{"type":"string"}}}},"required":["summary"],"additionalProperties":false}}',
            fail_on_error := false
        ) IS NULL AS invalid_schema_completion;
        SELECT ai_complete_json(
            'return constrained schema JSON',
            response_schema := '{{"type":"object","properties":{{"summary":{{"type":"string","minLength":4,"maxLength":20,"pattern":"^mock"}},"score":{{"type":"number","minimum":0,"exclusiveMaximum":1,"multipleOf":0.25}},"tags":{{"type":"array","minItems":2,"maxItems":3,"uniqueItems":true,"items":{{"type":"string"}}}}}},"required":["summary","score","tags"],"minProperties":3,"maxProperties":3,"additionalProperties":false}}'
        ) AS constrained_completion;
        SELECT summary, score, tags, profile.company, profile.metrics.score, profile.metrics.active,
               refs[1].label, refs[2].weight
        FROM ai_complete_record(
            'return record JSON object',
            '{{"type":"object","properties":{{"summary":{{"type":"string","minLength":4,"pattern":"^mock"}},"score":{{"type":"number","minimum":0,"maximum":1}},"tags":{{"type":"array","items":{{"type":"string"}},"minItems":2}},"profile":{{"type":"object","properties":{{"company":{{"type":"string"}},"metrics":{{"type":"object","properties":{{"score":{{"type":"integer"}},"active":{{"type":"boolean"}}}},"required":["score","active"],"additionalProperties":false}}}},"required":["company","metrics"],"additionalProperties":false}},"refs":{{"type":"array","items":{{"type":"object","properties":{{"label":{{"type":"string"}},"weight":{{"type":"integer"}}}},"required":["label","weight"],"additionalProperties":false}}}}}},"required":["summary","score","tags","profile","refs"],"additionalProperties":false}}'
        );
        SELECT ai_complete_json(
            'return length violation JSON',
            response_schema := '{{"type":"object","properties":{{"summary":{{"type":"string","minLength":4,"pattern":"^mock"}}}},"required":["summary"],"additionalProperties":false}}',
            fail_on_error := false
        ) IS NULL AS length_violation_completion;
        SELECT ai_complete_json(
            'return numeric violation JSON',
            response_schema := '{{"type":"object","properties":{{"score":{{"type":"number","maximum":1}}}},"required":["score"],"additionalProperties":false}}',
            fail_on_error := false
        ) IS NULL AS numeric_violation_completion;
        SELECT ai_complete_json(
            'return array violation JSON',
            response_schema := '{{"type":"object","properties":{{"tags":{{"type":"array","uniqueItems":true,"contains":{{"const":"duck"}},"minContains":1,"maxContains":1}}}},"required":["tags"],"additionalProperties":false}}',
            fail_on_error := false
        ) IS NULL AS array_violation_completion;
        SELECT ai_complete_json(
            'return oneOf violation JSON',
            response_schema := '{{"type":"object","oneOf":[{{"required":["a"]}},{{"required":["b"]}}]}}',
            fail_on_error := false
        ) IS NULL AS oneof_violation_completion;
        SELECT ai_summarize('DuckDB executes analytical SQL quickly') AS summary;
        SELECT ai_sentiment('I love DuckDB') AS sentiment;
        SELECT ai_fix_grammar('duckdb are fast') AS fixed_grammar;
        SELECT ai_redact('email alice@example.com token fake-token') AS masked_text;
        SELECT ai_translate('hello', 'Dutch') AS translation;
        SELECT ai_classify('invoice overdue', ['billing, overdue', 'support']) AS classification;
        SELECT ai_classify_labels('invoice overdue and slow app', ['billing, overdue', 'performance', 'support']) AS labels;
        SELECT ai_extract('name: DuckDB', 'name') AS extracted;
        SELECT ai_sql(
            'count rows',
            schema_context := 'CREATE TABLE smoke_table(id INTEGER)'
        ) AS generated_sql;
        SELECT * FROM ai_query_data(
            'count rows',
            schema_context := 'CREATE TABLE smoke_table(id INTEGER)'
        );
        SELECT * FROM ai_query_data(
            'count rows',
            schema_context := 'CREATE TABLE smoke_table(id INTEGER)'
        );
        SELECT explanation
        FROM ai_explain_sql(
            'SELECT 42',
            schema_context := 'CREATE TABLE smoke_table(id INTEGER)'
        );
        SELECT sql
        FROM ai_fix_sql(
            'SEELECT 42',
            schema_context := 'CREATE TABLE smoke_table(id INTEGER)'
        );
        SELECT line_number, replacement_line
        FROM ai_fix_sql(
            'SEELECT 42',
            mode := 'line',
            error := 'Parser Error: syntax error at or near "SEELECT" LINE 1: SEELECT 42',
            schema_context := 'CREATE TABLE smoke_table(id INTEGER)'
        );
        SELECT ai_summarize_agg(value)
        FROM (VALUES ('first row'), ('second row')) AS input(value);
        SELECT ai_agg(value, 'Return the most important word')
        FROM (VALUES ('duck'), ('database')) AS input(value);
        SELECT ai_complete('sample suppressed', log_sample_rate := 0.0) AS suppressed_completion;
        SELECT ai_complete('retry once', retry_count := 1, retry_backoff_ms := 0) AS retry_completion;
        SELECT ai_complete('otlp log smoke', log_format := 'otlp_json', log_tags := 'otlp-smoke') AS otlp_completion;
        SELECT ai_complete(
            'hello ollama',
            provider := 'ollama',
            model := 'mock-ollama-model',
            base_url := '{base_url}',
            temperature := 0.3,
            max_tokens := 11
        ) AS ollama_completion;
        SELECT ai_complete(
            'hello claude',
            provider := 'claude',
            model := 'mock-claude-model',
            base_url := '{base_url}',
            system_prompt := 'be exact',
            max_tokens := 13
        ) AS claude_completion;
        SELECT ai_complete(
            'builtin pricing smoke',
            model := 'gpt-5.4-mini',
            use_builtin_model_prices := true
        ) AS builtin_priced_completion;
        SELECT ai_complete(
            'hello databricks',
            secret := 'smoke_databricks_ai',
            provider := 'databricks',
            model := 'databricks-gpt-oss-120b'
        ) AS databricks_completion;
        SELECT ai_complete(
            'hello snowflake',
            secret := 'smoke_snowflake_ai',
            provider := 'snowflake',
            model := 'claude-sonnet-4-5',
            max_tokens := 19
        ) AS snowflake_completion;
        SELECT result.response, result.error IS NULL
        FROM (SELECT ai_try_complete('try complete smoke', max_tokens := 5) AS result);
        SELECT extracted.summary, extracted.profile.company
        FROM (
            SELECT ai_extract_record(
                'return scalar record JSON object',
                '{{"type":"object","properties":{{"summary":{{"type":"string"}},"profile":{{"type":"object","properties":{{"company":{{"type":"string"}}}},"required":["company"],"additionalProperties":false}}}},"required":["summary","profile"],"additionalProperties":false}}'
            ) AS extracted
        );
        SELECT ai_redact(
            'email alice@example.com token fake-token',
            secret := 'smoke_privacy_filter_ai',
            provider := 'openai_privacy_filter',
            model := 'openai/privacy-filter'
        ) AS privacy_filter_redaction;
        SELECT ai_embed('embed smoke')[1] AS first_embedding_value;
        SELECT ai_embed(
            'embed cache controls',
            cache := true,
            cache_ttl_seconds := 60,
            cache_max_entries := 2,
            connect_timeout_seconds := 1
        )[1] AS controlled_embedding_value;
        SELECT ai_embed(
            'embed cache controls',
            cache := true,
            cache_ttl_seconds := 60,
            cache_max_entries := 2,
            connect_timeout_seconds := 1
        )[1] AS cached_controlled_embedding_value;
        SELECT sum(embedding[1]) AS batched_embedding_sum
        FROM (
            SELECT ai_embed(value) AS embedding
            FROM (VALUES ('embed batch one'), ('embed batch two'), ('embed batch three')) AS input(value)
        );
        SELECT round(ai_similarity('same left', 'same right'), 6) AS similarity;
        SELECT round(sum(ai_similarity('constant query', candidate)), 6) AS constant_similarity_sum
        FROM (VALUES ('candidate one'), ('candidate two'), ('candidate three')) AS input(candidate);
        SELECT ai_rerank('analytics database', 'DuckDB runs analytical SQL') AS rerank_score;
        SELECT event, provider, protocol, model, prompt_chars, response_chars, input_chars,
               dimensions, prompt_tokens, completion_tokens, total_tokens, http_status,
               estimated_cost_usd
        FROM ai_usage();
    """
    env = os.environ.copy()
    env.update(
        {
            "OPENAI_API_KEY": "test-key",
            "ANTHROPIC_API_KEY": "anthropic-test-key",
        }
    )
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def run_duckdb_self_correction(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT * FROM ai_clear_usage();
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'mock-model';
        SET duckdb_ai_sql_assistant_model = 'mock-sql-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
        CREATE OR REPLACE SECRET smoke_fix_duckdb_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'openai'
        );
        SELECT * FROM ai_query_data(
            'self correct count',
            schema_context := 'CREATE TABLE smoke_fix(id INTEGER)',
            fix_attempts := 2
        );
        SELECT ai_sql(
            'self correct count',
            schema_context := 'CREATE TABLE smoke_fix(id INTEGER)',
            fix_attempts := 2
        ) AS repaired_sql;
        SELECT sql AS fixed_with_error
        FROM ai_fix_sql(
            'SELECT broken_column FROM smoke_fix_missing',
            mode := 'query',
            error := 'Catalog Error: Table with name smoke_fix_missing does not exist',
            schema_context := 'CREATE TABLE smoke_fix(id INTEGER)'
        );
        SELECT event, count(*) AS events
        FROM ai_usage()
        WHERE event IN ('ai_query_data_fix_attempt', 'ai_sql_fix_attempt')
        GROUP BY event
        ORDER BY event;
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb self correction smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_self_correction_result(output: str):
    required = [
        "repaired_sql",
        "SELECT 42",
        "fixed_with_error",
        "ai_query_data_fix_attempt",
        "ai_sql_fix_attempt",
    ]
    missing = [value for value in required if value not in output]
    if missing:
        raise AssertionError(f"self correction output missing {missing}\n{output}")

    if len(MockProviderHandler.completion_requests) != 5:
        raise AssertionError(
            f"expected 5 self correction completion requests, got {len(MockProviderHandler.completion_requests)}"
        )
    query_data_repair = "\n".join(
        message["content"] for message in MockProviderHandler.completion_requests[1]["messages"]
    )
    for expected in (
        "Correct the DuckDB SQL query",
        "Original question:",
        "self correct count",
        "Error message:",
        "smoke_fix_missing",
        "Broken SQL query:",
        "SELECT broken_column FROM smoke_fix_missing",
    ):
        if expected not in query_data_repair:
            raise AssertionError(f"ai_query_data repair prompt missing {expected!r}\n{query_data_repair}")
    fixup_prompt = "\n".join(message["content"] for message in MockProviderHandler.completion_requests[4]["messages"])
    for expected in (
        "Correct the DuckDB SQL query",
        "Error message:",
        "Catalog Error: Table with name smoke_fix_missing does not exist",
        "Broken SQL query:",
    ):
        if expected not in fixup_prompt:
            raise AssertionError(f"ai_fix_sql prompt missing {expected!r}\n{fixup_prompt}")


def run_duckdb_provider_error(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'mock-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
        CREATE OR REPLACE SECRET smoke_error_duckdb_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'openai'
        );
        SELECT ai_complete('force provider error');
    """
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(f"duckdb provider error smoke unexpectedly succeeded\n{result.stdout}")
    return result.stdout


def run_duckdb_cache_and_allowlist(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT * FROM ai_clear_usage();
        SELECT * FROM ai_clear_cache();
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'mock-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_allowed_hosts = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
        SET duckdb_ai_connect_timeout_seconds = 1;
        SET duckdb_ai_cache_max_entries = 1;
        CREATE OR REPLACE SECRET smoke_cache_duckdb_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'openai'
        );
        SELECT ai_complete('cache smoke', cache := true) AS first_completion;
        SELECT ai_complete('cache smoke', cache := true) AS second_completion;
        SELECT count(*) AS dogpile_cached_rows
        FROM (
            SELECT ai_complete('cache dogpile', cache := true, max_concurrent_requests := 8) AS completion
            FROM range(16)
        )
        WHERE completion = 'mock dogpile completion';
        SELECT ai_complete('cache smoke', cache := true) AS evicted_completion;
        SELECT count(*) AS usage_events FROM ai_usage();
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb cache smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def run_duckdb_allowlist_error(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'mock-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_allowed_hosts = 'example.com';
        SET duckdb_ai_timeout_seconds = 5;
        CREATE OR REPLACE SECRET smoke_allowlist_duckdb_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'openai'
        );
        SELECT ai_complete('blocked by allowlist');
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(f"duckdb allowlist smoke unexpectedly succeeded\n{result.stdout}")
    return result.stdout


def run_duckdb_strict_log_error(duckdb_path: Path, base_url: str, port: int) -> str:
    sql = f"""
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'mock-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_allowed_hosts = '{base_url}';
        SET duckdb_ai_log_endpoint = 'http://localhost:{port}/log';
        SET duckdb_ai_log_strict = true;
        SET duckdb_ai_timeout_seconds = 5;
        CREATE OR REPLACE SECRET smoke_strict_log_duckdb_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'openai'
        );
        SELECT ai_complete('strict log blocked');
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(f"duckdb strict log smoke unexpectedly succeeded\n{result.stdout}")
    return result.stdout


def run_duckdb_invalid_env_log_sample_rate(duckdb_path: Path) -> str:
    sql = "SELECT ai_completion_request_json('hello', provider := 'openai');"
    env = os.environ.copy()
    env["DUCKDB_AI_LOG_SAMPLE_RATE"] = "nan"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(f"duckdb invalid env log sample rate unexpectedly succeeded\n{result.stdout}")
    return result.stdout


def run_duckdb_invalid_url_scheme(duckdb_path: Path) -> str:
    sql = """
        SELECT ai_complete(
            'invalid scheme',
            provider := 'openai',
            model := 'mock-model',
            base_url := 'ftp://127.0.0.1'
        );
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(f"duckdb invalid URL scheme unexpectedly succeeded\n{result.stdout}")
    return result.stdout


def run_duckdb_invalid_header(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT ai_complete(
            'invalid header',
            provider := 'openrouter',
            model := 'openai/gpt-4o-mini',
            base_url := '{base_url}'
        );
    """
    env = os.environ.copy()
    env.update(
        {
            "OPENROUTER_API_KEY": "openrouter-test-key",
            "OPENROUTER_X_TITLE": "safe title\r\nX-Injected: true",
        }
    )
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(f"duckdb invalid HTTP header unexpectedly succeeded\n{result.stdout}")
    return result.stdout


def run_duckdb_oversized_response(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT ai_complete(
            'oversized response',
            provider := 'openai',
            model := 'mock-model',
            base_url := '{base_url}'
        );
    """
    env = os.environ.copy()
    env.update(
        {
            "OPENAI_API_KEY": "test-key",
            "DUCKDB_AI_MAX_RESPONSE_BYTES": "1024",
        }
    )
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(f"duckdb oversized HTTP response unexpectedly succeeded\n{result.stdout}")
    return result.stdout


def run_duckdb_openrouter_headers(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT ai_complete(
            'hello openrouter',
            provider := 'openrouter',
            model := 'openai/gpt-4o-mini',
            base_url := '{base_url}'
        );
    """
    env = os.environ.copy()
    env.update(
        {
            "OPENROUTER_API_KEY": "openrouter-test-key",
            "OPENROUTER_HTTP_REFERER": "https://duckdb-ai.example",
            "OPENROUTER_X_TITLE": "DuckDB AI smoke",
        }
    )
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb openrouter header smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_openrouter_headers(output: str):
    if "mock completion" not in output:
        raise AssertionError(f"duckdb openrouter output missing completion\n{output}")
    if MockProviderHandler.authorization_headers != ["Bearer openrouter-test-key"]:
        raise AssertionError(
            f"unexpected OpenRouter authorization headers: {MockProviderHandler.authorization_headers}"
        )
    if MockProviderHandler.openrouter_referer_headers != ["https://duckdb-ai.example"]:
        raise AssertionError(f"unexpected OpenRouter referer headers: {MockProviderHandler.openrouter_referer_headers}")
    if MockProviderHandler.openrouter_title_headers != ["DuckDB AI smoke"]:
        raise AssertionError(f"unexpected OpenRouter title headers: {MockProviderHandler.openrouter_title_headers}")


def run_duckdb_xai_prompt_cache_header(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT ai_complete(
            'hello xai',
            provider := 'xai',
            base_url := '{base_url}',
            system_prompt := 'answer briefly',
            prompt_cache := true
        );
    """
    env = os.environ.copy()
    env["XAI_API_KEY"] = "xai-test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb xAI prompt-cache header smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_xai_prompt_cache_header(output: str):
    if "mock completion" not in output:
        raise AssertionError(f"duckdb xAI output missing completion\n{output}")
    if MockProviderHandler.authorization_headers != ["Bearer xai-test-key"]:
        raise AssertionError(f"unexpected xAI authorization headers: {MockProviderHandler.authorization_headers}")
    if len(MockProviderHandler.completion_requests) != 1:
        raise AssertionError(f"expected one xAI completion request, got {MockProviderHandler.completion_requests}")
    request = MockProviderHandler.completion_requests[0]
    if request["model"] != "grok-4.5":
        raise AssertionError(f"unexpected xAI default model: {request}")
    if "prompt_cache_key" in request:
        raise AssertionError(f"xAI prompt cache key should be sent as a header, not request JSON: {request}")
    if len(MockProviderHandler.xai_conversation_headers) != 1:
        raise AssertionError(f"missing xAI conversation header: {MockProviderHandler.xai_conversation_headers}")
    if not MockProviderHandler.xai_conversation_headers[0].startswith("duckdb-ai-"):
        raise AssertionError(f"unexpected xAI conversation header: {MockProviderHandler.xai_conversation_headers}")


def assert_provider_error(output: str):
    required = [
        'AI provider "openai" (openai_chat, model "mock-model") returned HTTP 401',
        "type=authentication_error",
        "code=invalid_api_key",
        "credential [redacted] is invalid",
    ]
    missing = [value for value in required if value not in output]
    if missing:
        raise AssertionError(f"provider error output missing {missing}\n{output}")
    if "test-key" in output:
        raise AssertionError(f"provider error output leaked API key\n{output}")


def assert_cache_and_allowlist_result(output: str):
    required = [
        "first_completion",
        "second_completion",
        "evicted_completion",
        "mock completion",
        "dogpile_cached_rows",
        "16",
        "usage_events",
        "19",
    ]
    missing = [value for value in required if value not in output]
    if missing:
        raise AssertionError(f"cache output missing {missing}\n{output}")
    if len(MockProviderHandler.completion_requests) != 3:
        raise AssertionError(
            f"expected 3 cached completion requests, got {len(MockProviderHandler.completion_requests)}"
        )
    if len(MockProviderHandler.authorization_headers) != 3:
        raise AssertionError(f"expected 3 cached auth headers, got {len(MockProviderHandler.authorization_headers)}")
    prompts = [request["messages"][-1]["content"] for request in MockProviderHandler.completion_requests]
    if prompts != ["cache smoke", "cache dogpile", "cache smoke"]:
        raise AssertionError(f"unexpected cache prompts: {prompts}")


def assert_allowlist_error(output: str):
    if "not allowed by duckdb_ai_allowed_hosts" not in output:
        raise AssertionError(f"allowlist error missing expected message\n{output}")
    if MockProviderHandler.completion_requests:
        raise AssertionError(
            f"allowlist should block before provider request: {MockProviderHandler.completion_requests}"
        )


def assert_strict_log_error(output: str):
    if "AI usage log request failed" not in output or "not allowed by duckdb_ai_allowed_hosts" not in output:
        raise AssertionError(f"strict log error missing expected message\n{output}")
    if len(MockProviderHandler.completion_requests) != 1:
        raise AssertionError(f"strict log should call provider once: {MockProviderHandler.completion_requests}")
    if MockProviderHandler.log_requests:
        raise AssertionError(
            f"strict log allowlist should block before log request: {MockProviderHandler.log_requests}"
        )


def assert_invalid_env_log_sample_rate(output: str):
    if "DUCKDB_AI_LOG_SAMPLE_RATE must be between 0 and 1" not in output:
        raise AssertionError(f"invalid env log sample rate error missing expected message\n{output}")
    if MockProviderHandler.completion_requests:
        raise AssertionError(
            f"invalid env log sample rate should fail before provider request: {MockProviderHandler.completion_requests}"
        )


def assert_invalid_url_scheme(output: str):
    if "AI provider URL scheme must be http or https" not in output:
        raise AssertionError(f"invalid URL scheme error missing expected message\n{output}")
    if MockProviderHandler.completion_requests:
        raise AssertionError(
            f"invalid URL scheme should fail before provider request: {MockProviderHandler.completion_requests}"
        )


def assert_invalid_header(output: str):
    if "AI HTTP header value contains prohibited control characters" not in output:
        raise AssertionError(f"invalid HTTP header error missing expected message\n{output}")
    if MockProviderHandler.completion_requests:
        raise AssertionError(
            f"invalid HTTP header should fail before provider request: {MockProviderHandler.completion_requests}"
        )


def assert_oversized_response(output: str):
    if "AI HTTP response exceeded DUCKDB_AI_MAX_RESPONSE_BYTES (1024 bytes)" not in output:
        raise AssertionError(f"oversized HTTP response error missing expected message\n{output}")
    if len(MockProviderHandler.completion_requests) != 1:
        raise AssertionError(
            f"oversized response should make one provider request: {MockProviderHandler.completion_requests}"
        )


def run_duckdb_provider_metadata(duckdb_path: Path) -> str:
    sql = """
        SELECT ai_provider_base_url('aws_bedrock') AS bedrock_url;
        SELECT ai_provider_base_url('workers_ai') AS cloudflare_url;
        SELECT ai_provider_base_url('qwen') AS dashscope_url;
        SELECT ai_provider_base_url('google_vertex') AS vertex_url;
        SELECT ai_provider_base_url('kimi') AS moonshot_url;
        SELECT ai_provider_base_url('ernie') AS qianfan_url;
        SELECT ai_provider_base_url('tencent_hunyuan') AS hunyuan_url;
        SELECT ai_provider_base_url('step') AS stepfun_url;
        SELECT ai_provider_base_url('vercel_ai_gateway') AS vercel_url;
        SELECT ai_provider_base_url('doubao') AS volcengine_url;
        SELECT ai_provider_protocol('github_models') AS github_protocol;
        SELECT ai_provider_protocol('hf') AS huggingface_protocol;
        SELECT ai_provider_protocol('mini_max') AS minimax_protocol;
        SELECT ai_provider_protocol('poe_api') AS poe_protocol;
        SELECT ai_provider_protocol('sambanova_ai') AS sambanova_protocol;
        SELECT ai_provider_protocol('silicon_flow') AS siliconflow_protocol;
        SELECT ai_provider_protocol('x.ai') AS xai_protocol;
        SELECT ai_completion_request_json('hello qwen', provider := 'qwen') AS dashscope_request;
        SELECT ai_completion_request_json('hello kimi', provider := 'kimi') AS moonshot_request;
        SELECT ai_completion_request_json('hello ark', provider := 'ark') AS volcengine_request;
        SELECT ai_embedding_request_json('hello cloudflare', provider := 'workers_ai') AS cloudflare_embedding_request;
        SELECT ai_completion_request_json('hello nvidia', provider := 'nvidia_nim') AS nvidia_request;
    """
    env = os.environ.copy()
    env.update(
        {
            "AWS_REGION": "us-west-2",
            "CLOUDFLARE_ACCOUNT_ID": "duckdb-ai-account",
            "GOOGLE_CLOUD_PROJECT": "duckdb-ai-test",
            "GOOGLE_CLOUD_LOCATION": "us-central1",
        }
    )
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb provider metadata smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_provider_metadata(output: str):
    required = [
        "https://bedrock-mantle.us-west-2.api.aws/v1",
        "https://api.cloudflare.com/client/v4/accounts/duckdb-ai-account/ai/v1",
        "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
        "https://aiplatform.googleapis.com/v1/projects/duckdb-ai-test/locations/us-central1/endpoints/openapi",
        "https://api.moonshot.ai/v1",
        "https://qianfan.baidubce.com/v2",
        "https://tokenhub.tencentmaas.com/v1",
        "https://api.stepfun.ai/v1",
        "https://ai-gateway.vercel.sh/v1",
        "https://ark.cn-beijing.volces.com/api/v3",
        "github_protocol",
        "huggingface_protocol",
        "minimax_protocol",
        "poe_protocol",
        "sambanova_protocol",
        "siliconflow_protocol",
        "xai_protocol",
        "openai_chat",
        '"model":"qwen-plus"',
        '"model":"kimi-k2.7-code"',
        '"model":"doubao-seed-2-1-pro-260628"',
        '"model":"@cf/baai/bge-base-en-v1.5"',
        '"model":"meta/llama-3.3-70b-instruct"',
    ]
    missing = [value for value in required if value not in output]
    if missing:
        raise AssertionError(f"provider metadata output missing {missing}\n{output}")


def assert_smoke_result(output: str):
    required = [
        "mock completion",
        "mock structured",
        "invalid_schema_completion",
        "constrained_completion",
        "mock record",
        "DuckDB Labs",
        "42",
        "true",
        "docs",
        "2",
        "0.75",
        "length_violation_completion",
        "numeric_violation_completion",
        "array_violation_completion",
        "oneof_violation_completion",
        "mock retry completion",
        "mock ollama completion",
        "mock claude completion",
        "gpt-5.4-mini",
        "This query returns a single row with 42.",
        "SELECT 42",
        "ai_embedding",
        "openai",
        "openai_chat",
        "openai_embeddings",
        "mock-completion-model",
        "mock-task-model",
        "mock-sql-model",
        "mock-embedding-default",
        "ai_query_data_cache_hit",
        "openai_privacy_filter",
        "privacy_filter",
        "databricks",
        "snowflake",
        "databricks-gpt-oss-120b",
        "claude-sonnet-4-5",
        "email [PRIVATE_EMAIL] token [SECRET]",
        "billing, overdue",
        "performance",
        "0.82",
        "0.25",
        "1.0",
        "3.0",
        "16",
        "15",
        "7",
        "3",
        "10",
        "200",
    ]
    missing = [value for value in required if value not in output]
    if missing:
        raise AssertionError(f"duckdb output missing {missing}\n{output}")

    if len(MockProviderHandler.completion_requests) != 34:
        raise AssertionError(f"expected 34 completion requests, got {len(MockProviderHandler.completion_requests)}")
    if len(MockProviderHandler.authorization_headers) != 40:
        raise AssertionError(f"expected 40 auth headers, got {len(MockProviderHandler.authorization_headers)}")
    for header in MockProviderHandler.authorization_headers:
        if header != "Bearer test-key":
            raise AssertionError(f"unexpected authorization header: {header}")

    completion_models = [request["model"] for request in MockProviderHandler.completion_requests]
    if completion_models[0:9] != ["mock-completion-model"] * 9:
        raise AssertionError(f"unexpected completion default models: {completion_models[0:9]}")
    if completion_models[9:17] != ["mock-task-model"] * 8:
        raise AssertionError(f"unexpected task default models: {completion_models[9:17]}")
    if completion_models[17:22] != ["mock-sql-model"] * 5:
        raise AssertionError(f"unexpected SQL assistant default models: {completion_models[17:22]}")
    if completion_models[22:24] != ["mock-aggregate-model"] * 2:
        raise AssertionError(f"unexpected aggregate default models: {completion_models[22:24]}")
    if completion_models[24:28] != ["mock-completion-model"] * 4:
        raise AssertionError(f"unexpected later completion default models: {completion_models[24:28]}")

    completion_request = MockProviderHandler.completion_requests[0]
    if completion_request["model"] != "mock-completion-model":
        raise AssertionError(f"unexpected completion model: {completion_request}")
    if completion_request["messages"][0] != {"role": "system", "content": "answer briefly"}:
        raise AssertionError(f"unexpected completion system prompt: {completion_request}")
    if completion_request["messages"][1]["content"] != "hello from smoke":
        raise AssertionError(f"unexpected completion prompt: {completion_request}")
    if completion_request.get("temperature") != 0.2:
        raise AssertionError(f"unexpected completion temperature: {completion_request}")
    if completion_request.get("max_completion_tokens") != 17 or "max_tokens" in completion_request:
        raise AssertionError(f"unexpected completion token limit: {completion_request}")

    structured_request = MockProviderHandler.completion_requests[1]
    response_format = structured_request.get("response_format")
    if response_format is None or response_format.get("type") != "json_schema":
        raise AssertionError(f"unexpected structured response format: {structured_request}")
    if response_format.get("json_schema", {}).get("schema", {}).get("type") != "object":
        raise AssertionError(f"unexpected structured response schema: {structured_request}")
    structured_system = structured_request["messages"][0]["content"]
    if "Return only valid JSON" not in structured_system:
        raise AssertionError(f"unexpected structured system prompt: {structured_request}")
    structured_prompt = structured_request["messages"][-1]["content"]
    if structured_prompt != "return a smoke JSON object":
        raise AssertionError(f"unexpected structured prompt: {structured_prompt}")

    invalid_schema_prompt = MockProviderHandler.completion_requests[2]["messages"][-1]["content"]
    if "return invalid schema JSON" not in invalid_schema_prompt:
        raise AssertionError(f"unexpected invalid schema prompt: {invalid_schema_prompt}")

    constrained_prompt = MockProviderHandler.completion_requests[3]["messages"][-1]["content"]
    if "return constrained schema JSON" not in constrained_prompt:
        raise AssertionError(f"unexpected constrained schema prompt: {constrained_prompt}")
    record_prompt = MockProviderHandler.completion_requests[4]["messages"][-1]["content"]
    if "return record JSON object" not in record_prompt:
        raise AssertionError(f"unexpected record prompt: {record_prompt}")
    schema_failure_prompts = [
        request["messages"][-1]["content"] for request in MockProviderHandler.completion_requests[5:9]
    ]
    expected_schema_failure_fragments = [
        "return length violation JSON",
        "return numeric violation JSON",
        "return array violation JSON",
        "return oneOf violation JSON",
    ]
    for fragment, prompt in zip(expected_schema_failure_fragments, schema_failure_prompts):
        if fragment not in prompt:
            raise AssertionError(f"schema failure prompt missing {fragment!r}: {prompt}")

    task_messages = [request["messages"] for request in MockProviderHandler.completion_requests[9:17]]
    expected_system_fragments = [
        "Summarize the following text concisely",
        "positive, neutral, negative",
        "Fix grammar, spelling, and punctuation",
        "Mask direct personal data",
        "Translate the following text to Dutch",
        'exactly one of these labels: "billing, overdue", "support"',
        'zero or more of these labels: "billing, overdue", "performance", "support"',
        "Extract the requested information",
    ]
    expected_user_fragments = [
        "Text:\nDuckDB executes analytical SQL quickly",
        "Text:\nI love DuckDB",
        "Text:\nduckdb are fast",
        "Text:\nemail alice@example.com token fake-token",
        "Text:\nhello",
        "Text:\ninvoice overdue",
        "Text:\ninvoice overdue and slow app",
        "Extraction request:\nname",
    ]
    for system_fragment, user_fragment, messages in zip(
        expected_system_fragments, expected_user_fragments, task_messages
    ):
        system_prompt = messages[0]["content"]
        user_prompt = messages[-1]["content"]
        if system_fragment not in system_prompt:
            raise AssertionError(f"task system prompt missing {system_fragment!r}: {system_prompt}")
        if user_fragment not in user_prompt:
            raise AssertionError(f"task user prompt missing {user_fragment!r}: {user_prompt}")

    ai_sql_messages = MockProviderHandler.completion_requests[17]["messages"]
    ai_sql_system = ai_sql_messages[0]["content"]
    ai_sql_prompt = ai_sql_messages[-1]["content"]
    if "Generate one DuckDB SQL SELECT statement" not in ai_sql_system:
        raise AssertionError(f"unexpected ai_sql system prompt: {ai_sql_system}")
    if "CREATE TABLE smoke_table(id INTEGER)" not in ai_sql_system:
        raise AssertionError(f"ai_sql system prompt missing schema context: {ai_sql_system}")
    if "Request:\ncount rows" not in ai_sql_prompt:
        raise AssertionError(f"ai_sql user prompt missing request: {ai_sql_prompt}")
    ai_query_data_messages = MockProviderHandler.completion_requests[18]["messages"]
    ai_query_data_system = ai_query_data_messages[0]["content"]
    ai_query_data_prompt = ai_query_data_messages[-1]["content"]
    if "Generate one DuckDB SQL SELECT statement" not in ai_query_data_system:
        raise AssertionError(f"unexpected ai_query_data system prompt: {ai_query_data_system}")
    if "CREATE TABLE smoke_table(id INTEGER)" not in ai_query_data_system:
        raise AssertionError(f"ai_query_data system prompt missing schema context: {ai_query_data_system}")
    if "Request:\ncount rows" not in ai_query_data_prompt:
        raise AssertionError(f"ai_query_data user prompt missing request: {ai_query_data_prompt}")
    ai_explain_sql_messages = MockProviderHandler.completion_requests[19]["messages"]
    ai_explain_sql_system = ai_explain_sql_messages[0]["content"]
    ai_explain_sql_prompt = ai_explain_sql_messages[-1]["content"]
    if "Explain the DuckDB SQL query" not in ai_explain_sql_system:
        raise AssertionError(f"unexpected ai_explain_sql system prompt: {ai_explain_sql_system}")
    if "SELECT 42" not in ai_explain_sql_prompt:
        raise AssertionError(f"ai_explain_sql prompt missing SQL: {ai_explain_sql_prompt}")
    ai_fix_sql_messages = MockProviderHandler.completion_requests[20]["messages"]
    ai_fix_sql_system = ai_fix_sql_messages[0]["content"]
    ai_fix_sql_prompt = ai_fix_sql_messages[-1]["content"]
    if "Correct the DuckDB SQL query" not in ai_fix_sql_system:
        raise AssertionError(f"unexpected ai_fix_sql system prompt: {ai_fix_sql_system}")
    if "SEELECT 42" not in ai_fix_sql_prompt:
        raise AssertionError(f"ai_fix_sql prompt missing broken SQL: {ai_fix_sql_prompt}")
    ai_fix_sql_line_mode_messages = MockProviderHandler.completion_requests[21]["messages"]
    ai_fix_sql_line_mode_system = ai_fix_sql_line_mode_messages[0]["content"]
    ai_fix_sql_line_mode_prompt = ai_fix_sql_line_mode_messages[-1]["content"]
    if "Correct one line in the DuckDB SQL query" not in ai_fix_sql_line_mode_system:
        raise AssertionError(f"unexpected ai_fix_sql line-mode system prompt: {ai_fix_sql_line_mode_system}")
    if "Line to correct: 1" not in ai_fix_sql_line_mode_prompt:
        raise AssertionError(f"ai_fix_sql line-mode prompt missing line number: {ai_fix_sql_line_mode_prompt}")
    summarize_agg_prompt = MockProviderHandler.completion_requests[22]["messages"][-1]["content"]
    if "Summarize the following SQL aggregate input values" not in summarize_agg_prompt:
        raise AssertionError(f"unexpected ai_summarize_agg prompt: {summarize_agg_prompt}")
    if "first row" not in summarize_agg_prompt or "second row" not in summarize_agg_prompt:
        raise AssertionError(f"ai_summarize_agg prompt missing aggregate values: {summarize_agg_prompt}")
    agg_prompt = MockProviderHandler.completion_requests[23]["messages"][-1]["content"]
    if "Return the most important word" not in agg_prompt:
        raise AssertionError(f"unexpected ai_agg prompt: {agg_prompt}")
    if "duck" not in agg_prompt or "database" not in agg_prompt:
        raise AssertionError(f"ai_agg prompt missing aggregate values: {agg_prompt}")
    suppressed_prompt = MockProviderHandler.completion_requests[24]["messages"][-1]["content"]
    if suppressed_prompt != "sample suppressed":
        raise AssertionError(f"unexpected suppressed completion prompt: {suppressed_prompt}")
    retry_prompts = [request["messages"][-1]["content"] for request in MockProviderHandler.completion_requests[25:27]]
    if retry_prompts != ["retry once", "retry once"]:
        raise AssertionError(f"unexpected retry prompts: {retry_prompts}")
    if MockProviderHandler.retry_completion_attempts != 2:
        raise AssertionError(f"expected 2 retry attempts, got {MockProviderHandler.retry_completion_attempts}")
    otlp_prompt = MockProviderHandler.completion_requests[27]["messages"][-1]["content"]
    if otlp_prompt != "otlp log smoke":
        raise AssertionError(f"unexpected OTLP completion prompt: {otlp_prompt}")
    builtin_price_request = MockProviderHandler.completion_requests[28]
    if builtin_price_request.get("model") != "gpt-5.4-mini":
        raise AssertionError(f"unexpected builtin pricing model: {builtin_price_request}")
    if builtin_price_request["messages"][-1]["content"] != "builtin pricing smoke":
        raise AssertionError(f"unexpected builtin pricing prompt: {builtin_price_request}")
    databricks_request = MockProviderHandler.completion_requests[29]
    if databricks_request.get("model") != "databricks-gpt-oss-120b":
        raise AssertionError(f"unexpected Databricks model: {databricks_request}")
    if databricks_request["messages"][-1]["content"] != "hello databricks":
        raise AssertionError(f"unexpected Databricks prompt: {databricks_request}")
    snowflake_request = MockProviderHandler.completion_requests[30]
    if snowflake_request.get("model") != "claude-sonnet-4-5":
        raise AssertionError(f"unexpected Snowflake model: {snowflake_request}")
    if snowflake_request.get("max_completion_tokens") != 19 or "max_tokens" in snowflake_request:
        raise AssertionError(f"unexpected Snowflake token limit: {snowflake_request}")
    if snowflake_request["messages"][-1]["content"] != "hello snowflake":
        raise AssertionError(f"unexpected Snowflake prompt: {snowflake_request}")
    try_complete_request = MockProviderHandler.completion_requests[31]
    if try_complete_request["messages"][-1]["content"] != "try complete smoke":
        raise AssertionError(f"unexpected try completion prompt: {try_complete_request}")
    if try_complete_request.get("max_completion_tokens") != 5 or "max_tokens" in try_complete_request:
        raise AssertionError(f"unexpected try completion token limit: {try_complete_request}")
    extract_record_request = MockProviderHandler.completion_requests[32]
    if extract_record_request["messages"][-1]["content"] != "return scalar record JSON object":
        raise AssertionError(f"unexpected extract record prompt: {extract_record_request}")
    if "Return only valid JSON" not in extract_record_request["messages"][0]["content"]:
        raise AssertionError(f"unexpected extract record system prompt: {extract_record_request}")
    rerank_request = MockProviderHandler.completion_requests[33]
    if "Score candidate relevance" not in rerank_request["messages"][0]["content"]:
        raise AssertionError(f"unexpected rerank system prompt: {rerank_request}")
    if (
        rerank_request["messages"][-1]["content"]
        != "Query:\nanalytics database\n\nCandidate:\nDuckDB runs analytical SQL"
    ):
        raise AssertionError(f"unexpected rerank prompt: {rerank_request}")
    if len(MockProviderHandler.privacy_filter_requests) != 1:
        raise AssertionError(
            f"expected 1 Privacy Filter request, got {len(MockProviderHandler.privacy_filter_requests)}"
        )
    privacy_filter_request = MockProviderHandler.privacy_filter_requests[0]
    if privacy_filter_request != {
        "text": "email alice@example.com token fake-token",
        "model": "openai/privacy-filter",
    }:
        raise AssertionError(f"unexpected Privacy Filter request: {privacy_filter_request}")

    if len(MockProviderHandler.ollama_requests) != 1:
        raise AssertionError(f"expected 1 Ollama request, got {len(MockProviderHandler.ollama_requests)}")
    ollama_request = MockProviderHandler.ollama_requests[0]
    if ollama_request.get("model") != "mock-ollama-model":
        raise AssertionError(f"unexpected Ollama model: {ollama_request}")
    if ollama_request.get("stream") is not False:
        raise AssertionError(f"unexpected Ollama stream setting: {ollama_request}")
    if ollama_request["messages"][-1]["content"] != "hello ollama":
        raise AssertionError(f"unexpected Ollama prompt: {ollama_request}")
    if ollama_request.get("options", {}).get("temperature") != 0.3:
        raise AssertionError(f"unexpected Ollama temperature: {ollama_request}")
    if ollama_request.get("options", {}).get("num_predict") != 11:
        raise AssertionError(f"unexpected Ollama max tokens: {ollama_request}")

    if len(MockProviderHandler.claude_requests) != 1:
        raise AssertionError(f"expected 1 Claude request, got {len(MockProviderHandler.claude_requests)}")
    claude_request = MockProviderHandler.claude_requests[0]
    if claude_request.get("model") != "mock-claude-model":
        raise AssertionError(f"unexpected Claude model: {claude_request}")
    if claude_request.get("system") != "be exact":
        raise AssertionError(f"unexpected Claude system prompt: {claude_request}")
    if claude_request.get("max_tokens") != 13:
        raise AssertionError(f"unexpected Claude max tokens: {claude_request}")
    if claude_request["messages"][-1]["content"] != "hello claude":
        raise AssertionError(f"unexpected Claude prompt: {claude_request}")
    if MockProviderHandler.claude_api_keys != ["anthropic-test-key"]:
        raise AssertionError(f"unexpected Claude API key headers: {MockProviderHandler.claude_api_keys}")
    if MockProviderHandler.claude_versions != ["2023-06-01"]:
        raise AssertionError(f"unexpected Claude version headers: {MockProviderHandler.claude_versions}")

    if len(MockProviderHandler.embedding_requests) != 5:
        raise AssertionError(f"expected 5 embedding requests, got {len(MockProviderHandler.embedding_requests)}")
    embedding_request = MockProviderHandler.embedding_requests[0]
    if embedding_request["model"] != "mock-embedding-default":
        raise AssertionError(f"unexpected embedding model: {embedding_request}")
    if embedding_request["input"] != "embed smoke":
        raise AssertionError(f"unexpected embedding input: {embedding_request}")
    controlled_embedding_request = MockProviderHandler.embedding_requests[1]
    if controlled_embedding_request["input"] != "embed cache controls":
        raise AssertionError(f"unexpected controlled embedding input: {controlled_embedding_request}")
    batched_embedding_request = MockProviderHandler.embedding_requests[2]
    if batched_embedding_request["input"] != ["embed batch one", "embed batch two", "embed batch three"]:
        raise AssertionError(f"unexpected batched embedding input: {batched_embedding_request}")
    similarity_request = MockProviderHandler.embedding_requests[3]
    if similarity_request["input"] != ["same left", "same right"]:
        raise AssertionError(f"unexpected packed similarity inputs: {similarity_request}")
    if similarity_request["model"] != "mock-embedding-default":
        raise AssertionError(f"unexpected similarity embedding model: {similarity_request}")
    constant_similarity_inputs = MockProviderHandler.embedding_requests[4]["input"]
    if constant_similarity_inputs.count("constant query") != 1:
        raise AssertionError(f"expected one constant-side embedding, got {constant_similarity_inputs}")
    if set(constant_similarity_inputs) != {"constant query", "candidate one", "candidate two", "candidate three"}:
        raise AssertionError(f"unexpected packed constant similarity inputs: {constant_similarity_inputs}")

    log_deadline = time.time() + 5
    while len(MockProviderHandler.log_requests) < 47 and time.time() < log_deadline:
        time.sleep(0.05)
    if len(MockProviderHandler.log_requests) != 47:
        event_counts = {}
        for request in MockProviderHandler.log_requests:
            event = "otlp" if "resourceLogs" in request else request.get("event", "unknown")
            event_counts[event] = event_counts.get(event, 0) + 1
        raise AssertionError(f"expected 47 log requests, got {len(MockProviderHandler.log_requests)}: {event_counts}")
    completion_logs = [
        request for request in MockProviderHandler.log_requests if request.get("event") == "ai_completion"
    ]
    embedding_logs = [request for request in MockProviderHandler.log_requests if request.get("event") == "ai_embedding"]
    otlp_logs = [request for request in MockProviderHandler.log_requests if "resourceLogs" in request]
    if len(completion_logs) != 34 or len(embedding_logs) != 12:
        raise AssertionError(f"unexpected log events: {MockProviderHandler.log_requests}")
    if len(otlp_logs) != 1:
        raise AssertionError(f"expected 1 OTLP log request, got {otlp_logs}")
    logged_providers = {request.get("provider") for request in completion_logs}
    if not {"openai", "ollama", "anthropic", "databricks", "snowflake", "openai_privacy_filter"}.issubset(
        logged_providers
    ):
        raise AssertionError(f"missing provider logs: {completion_logs}")

    otlp_log = otlp_logs[0]
    resource_log = otlp_log["resourceLogs"][0]
    resource_attrs = {
        attribute["key"]: attribute["value"].get("stringValue")
        for attribute in resource_log.get("resource", {}).get("attributes", [])
    }
    if resource_attrs.get("service.name") != "duckdb_ai":
        raise AssertionError(f"unexpected OTLP resource attributes: {otlp_log}")
    log_record = resource_log["scopeLogs"][0]["logRecords"][0]
    if log_record.get("body", {}).get("stringValue") != "ai_completion":
        raise AssertionError(f"unexpected OTLP body: {otlp_log}")
    otlp_attrs = {}
    for attribute in log_record.get("attributes", []):
        value = attribute["value"]
        otlp_attrs[attribute["key"]] = value.get("stringValue", value.get("intValue"))
    expected_otlp_attrs = {
        "ai.event": "ai_completion",
        "ai.function_name": "ai_complete",
        "ai.provider": "openai",
        "ai.protocol": "openai_chat",
        "ai.model": "mock-completion-model",
        "ai.tags": "otlp-smoke",
        "ai.prompt_chars": "14",
        "ai.response_chars": "15",
        "http.status_code": "200",
    }
    for key, expected in expected_otlp_attrs.items():
        actual = otlp_attrs.get(key)
        if actual != expected:
            raise AssertionError(f"unexpected OTLP attribute {key}: expected {expected!r}, got {actual!r}")
    if "ai.prompt" in otlp_attrs or "ai.response" in otlp_attrs:
        raise AssertionError(f"OTLP log unexpectedly included prompt/response text: {otlp_attrs}")
    if not otlp_attrs.get("ai.query_id", "").startswith("duckdb_ai_query_"):
        raise AssertionError(f"OTLP log missing query id: {otlp_attrs}")

    log_request = completion_logs[0]
    expected_log_fields = {
        "extension": "duckdb_ai",
        "event": "ai_completion",
        "function_name": "ai_complete",
        "provider": "openai",
        "protocol": "openai_chat",
        "model": "mock-completion-model",
        "tags": "smoke-run",
        "prompt_chars": 16,
        "response_chars": 15,
        "prompt_tokens": 7,
        "completion_tokens": 3,
        "total_tokens": 10,
        "http_status": 200,
    }
    for key, expected in expected_log_fields.items():
        actual = log_request.get(key)
        if actual != expected:
            raise AssertionError(f"unexpected log field {key}: expected {expected!r}, got {actual!r}")
    if not log_request.get("query_id", "").startswith("duckdb_ai_query_"):
        raise AssertionError(f"log request missing query id: {log_request}")
    estimated_cost = log_request.get("estimated_cost_usd")
    if estimated_cost is None or abs(estimated_cost - 0.000013) > 0.000000001:
        raise AssertionError(f"unexpected estimated cost: {log_request}")
    builtin_price_log = next((request for request in completion_logs if request.get("model") == "gpt-5.4-mini"), None)
    if builtin_price_log is None:
        raise AssertionError(f"missing builtin pricing log: {completion_logs}")
    builtin_estimated_cost = builtin_price_log.get("estimated_cost_usd")
    if builtin_estimated_cost is None or abs(builtin_estimated_cost - 0.00001875) > 0.000000001:
        raise AssertionError(f"unexpected builtin pricing cost: {builtin_price_log}")
    databricks_log = next(
        (request for request in completion_logs if request.get("model") == "databricks-gpt-oss-120b"), None
    )
    if databricks_log is None or databricks_log.get("provider") != "databricks":
        raise AssertionError(f"missing Databricks completion log: {completion_logs}")
    snowflake_log = next((request for request in completion_logs if request.get("model") == "claude-sonnet-4-5"), None)
    if snowflake_log is None or snowflake_log.get("provider") != "snowflake":
        raise AssertionError(f"missing Snowflake completion log: {completion_logs}")
    privacy_filter_log = next(
        (request for request in completion_logs if request.get("model") == "openai/privacy-filter"), None
    )
    if privacy_filter_log is None or privacy_filter_log.get("provider") != "openai_privacy_filter":
        raise AssertionError(f"missing Privacy Filter redaction log: {completion_logs}")
    for completion_log in completion_logs:
        if "prompt" in completion_log or "response" in completion_log:
            raise AssertionError(f"log request unexpectedly included prompt/response text: {completion_log}")

    embedding_log_request = embedding_logs[0]
    expected_embedding_log_fields = {
        "extension": "duckdb_ai",
        "event": "ai_embedding",
        "function_name": "ai_embed",
        "provider": "openai",
        "protocol": "openai_embeddings",
        "model": "mock-embedding-default",
        "input_chars": 11,
        "dimensions": 3,
        "prompt_tokens": 2,
        "total_tokens": 2,
        "http_status": 200,
    }
    for key, expected in expected_embedding_log_fields.items():
        actual = embedding_log_request.get(key)
        if actual != expected:
            raise AssertionError(f"unexpected embedding log field {key}: expected {expected!r}, got {actual!r}")
    if not embedding_log_request.get("query_id", "").startswith("duckdb_ai_query_"):
        raise AssertionError(f"embedding log missing query id: {embedding_log_request}")
    if "input" in embedding_log_request:
        raise AssertionError(f"embedding log request unexpectedly included input text: {embedding_log_request}")
    controlled_embedding_logs = [request for request in embedding_logs if request.get("input_chars") == 20]
    if len(controlled_embedding_logs) != 2:
        raise AssertionError(f"expected two controlled embedding logs, got {controlled_embedding_logs}")
    if sorted(request.get("cache_hit") for request in controlled_embedding_logs) != [False, True]:
        raise AssertionError(f"expected controlled embedding cache miss and hit, got {controlled_embedding_logs}")


def run_duckdb_adaptive_batches(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT * FROM ai_clear_usage();
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_embedding_model = 'mock-embedding';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
        SELECT sum(ai_embed('split batch ' || i::VARCHAR)[1]) AS split_embedding_sum
        FROM range(8) t(i);
        CREATE EXTERNAL MODEL capped_embedding WITH (
            provider = 'openai',
            model = 'mock-embedding',
            location = '{base_url}',
            model_type = 'embedding',
            options = '{{"max_batch_inputs":3,"max_batch_tokens":100,"max_request_bytes":4096,"embedding_dimensions":3}}'
        );
        SELECT sum(ai_embed('capped input ' || i::VARCHAR, profile := 'capped_embedding')[1]) AS capped_embedding_sum
        FROM range(8) t(i);
        CREATE EXTERNAL MODEL tiny_embedding_request WITH (
            provider = 'openai',
            model = 'mock-embedding',
            location = '{base_url}',
            model_type = 'embedding',
            options = '{{"max_batch_tokens":1}}'
        );
        SELECT ai_embed(
            'too many tokens for one request',
            profile := 'tiny_embedding_request',
            fail_on_error := false
        ) IS NULL AS token_limit_rejected;
        CREATE EXTERNAL MODEL escaped_embedding_request WITH (
            provider = 'openai',
            model = 'mock-embedding',
            location = '{base_url}',
            model_type = 'embedding',
            options = '{{"max_request_bytes":200}}'
        );
        SELECT ai_embed(repeat('"', 100), profile := 'escaped_embedding_request',
                        fail_on_error := false) IS NULL AS escaped_byte_limit_rejected;
        CREATE EXTERNAL MODEL tiny_completion_context WITH (
            provider = 'openai',
            model = 'mock-completion',
            location = '{base_url}',
            model_type = 'completion',
            options = '{{"max_input_tokens":1}}'
        );
        SELECT ai_complete('too many completion tokens', profile := 'tiny_completion_context',
                           fail_on_error := false) IS NULL AS completion_context_rejected;
        CREATE EXTERNAL MODEL cached_embedding_dimensions WITH (
            provider = 'openai',
            model = 'mock-embedding',
            location = '{base_url}',
            model_type = 'embedding',
            options = '{{"embedding_dimensions":3}}'
        );
        SELECT count(ai_embed('cached dimension ' || i::VARCHAR,
                              profile := 'cached_embedding_dimensions', cache := true)) AS cached_dimensions_seeded
        FROM range(2) t(i);
        CREATE OR REPLACE EXTERNAL MODEL cached_embedding_dimensions WITH (
            provider = 'openai',
            model = 'mock-embedding',
            location = '{base_url}',
            model_type = 'embedding',
            options = '{{"embedding_dimensions":4}}'
        );
        SELECT count(ai_embed('cached dimension ' || i::VARCHAR,
                              profile := 'cached_embedding_dimensions', cache := true,
                              fail_on_error := false)) = 0 AS stale_dimensions_rejected
        FROM range(2) t(i);
        SELECT round(sum(ai_similarity(left_text, right_text)), 6) AS deduplicated_similarity_sum
        FROM (VALUES
            ('same left', 'same right'),
            ('same left', 'same right'),
            ('same left', 'other right'),
            ('same left', 'other right')
        ) input(left_text, right_text);
        SELECT sum(batch_count) AS request_batches, sum(failures) = 4 AS only_terminal_failures
        FROM ai_usage_summary();
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb adaptive batch smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_adaptive_batches(output: str):
    for expected in (
        "split_embedding_sum",
        "2.0",
        "token_limit_rejected",
        "escaped_byte_limit_rejected",
        "completion_context_rejected",
        "true",
        "cached_dimensions_seeded",
        "stale_dimensions_rejected",
        "deduplicated_similarity_sum",
        "4.0",
        "request_batches",
        "only_terminal_failures",
    ):
        if expected not in output:
            raise AssertionError(f"adaptive batch output missing {expected!r}\n{output}")
    if len(MockProviderHandler.embedding_requests) != 12:
        raise AssertionError(f"expected 12 adaptive embedding requests, got {MockProviderHandler.embedding_requests}")
    split_sizes = [len(request["input"]) for request in MockProviderHandler.embedding_requests[:7]]
    if split_sizes != [8, 4, 2, 2, 4, 2, 2]:
        raise AssertionError(f"unexpected recursive split request sizes: {split_sizes}")
    capped_sizes = [len(request["input"]) for request in MockProviderHandler.embedding_requests[7:10]]
    if capped_sizes != [3, 3, 2]:
        raise AssertionError(f"external model batch capabilities were not applied: {capped_sizes}")
    cached_dimension_inputs = MockProviderHandler.embedding_requests[10]["input"]
    if cached_dimension_inputs != ["cached dimension 0", "cached dimension 1"]:
        raise AssertionError(f"unexpected cache dimension seed inputs: {cached_dimension_inputs}")
    deduplicated_inputs = MockProviderHandler.embedding_requests[11]["input"]
    if deduplicated_inputs != ["same left", "same right", "other right"]:
        raise AssertionError(f"similarity inputs were not packed and deduplicated: {deduplicated_inputs}")
    if MockProviderHandler.completion_requests:
        raise AssertionError(
            f"completion context preflight unexpectedly called the provider: {MockProviderHandler.completion_requests}"
        )


def run_duckdb_embedding_usage_distribution(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT * FROM ai_clear_usage();
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_embedding_model = 'mock-embedding';
        SET duckdb_ai_base_url = '{base_url}';
        SELECT sum(ai_embed('uneven usage ' || i::VARCHAR)[1]) FROM range(3) t(i);
        SELECT sum(prompt_tokens) = 5 AS exact_prompt_tokens,
               sum(total_tokens) = 5 AS exact_total_tokens
        FROM ai_usage() WHERE event = 'ai_embedding';
    """
    env = {**os.environ, "OPENAI_API_KEY": "test-key"}
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb embedding usage distribution exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_embedding_usage_distribution(output: str):
    for expected in ("exact_prompt_tokens", "exact_total_tokens", "true"):
        if expected not in output:
            raise AssertionError(f"embedding usage distribution output missing {expected!r}\n{output}")
    if len(MockProviderHandler.embedding_requests) != 1:
        raise AssertionError(f"expected one packed uneven-usage request: {MockProviderHandler.embedding_requests}")


def run_duckdb_executor_concurrency(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_completion_model = 'mock-completion';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
        SELECT count(ai_complete('grow pool ' || i::VARCHAR, max_concurrent_requests := 8)) AS grown
        FROM range(8) t(i);
        SELECT count(ai_complete('cap one ' || i::VARCHAR, max_concurrent_requests := 1)) AS capped
        FROM range(8) t(i);
    """
    env = {**os.environ, "OPENAI_API_KEY": "test-key"}
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb executor concurrency exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_executor_concurrency(output: str):
    for expected in ("grown", "capped", "8"):
        if expected not in output:
            raise AssertionError(f"executor concurrency output missing {expected!r}\n{output}")
    if MockProviderHandler.max_grow_pool_requests < 2:
        raise AssertionError("executor did not exercise concurrent provider requests")
    if MockProviderHandler.max_cap_one_requests != 1:
        raise AssertionError(
            f"max_concurrent_requests=1 allowed {MockProviderHandler.max_cap_one_requests} concurrent requests"
        )


def run_duckdb_hierarchical_aggregate(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT * FROM ai_clear_usage();
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_aggregate_model = 'mock-aggregate';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
        SELECT ai_agg(value, 'Summarize all values', max_context_chars := 40) AS hierarchical_result
        FROM (
            SELECT repeat(chr(65 + (i % 26)::INTEGER), 18) || i::VARCHAR AS value
            FROM range(20) t(i)
        );
        SELECT count(DISTINCT operation_id) AS operations,
               count(DISTINCT parent_operation_id) AS parents,
               bool_and(parent_operation_id IS NOT NULL) AS all_children_linked
        FROM ai_usage()
        WHERE function_name LIKE 'ai_agg%';
        SELECT ai_agg(value, 'Summarize all values', max_context_chars := 40,
                      fail_on_error := false) IS NULL AS empty_intermediate_rejected
        FROM (
            SELECT repeat('EMPTY_INTERMEDIATE', 4) || i::VARCHAR AS value
            FROM range(4) t(i)
        );
        SELECT ai_agg(value, 'Summarize all values', max_context_chars := 40,
                      fail_on_error := false) IS NULL AS non_converging_rejected
        FROM (
            SELECT repeat('NON_CONVERGING', 4) || i::VARCHAR AS value
            FROM range(4) t(i)
        );
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb hierarchical aggregate smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_hierarchical_aggregate(output: str):
    for expected in (
        "hierarchical_result",
        "mock completion",
        "operations",
        "parents",
        "empty_intermediate_rejected",
        "non_converging_rejected",
        "true",
    ):
        if expected not in output:
            raise AssertionError(f"hierarchical aggregate output missing {expected!r}\n{output}")
    if len(MockProviderHandler.completion_requests) < 5:
        raise AssertionError(
            f"expected a multi-level aggregate request tree, got {len(MockProviderHandler.completion_requests)} requests"
        )
    prompts = [request["messages"][-1]["content"] for request in MockProviderHandler.completion_requests]
    if sum("mock completion" in prompt for prompt in prompts) < 2:
        raise AssertionError(f"aggregate did not perform recursive map/reduce: {prompts}")
    non_converging_requests = sum("NON_CONVERGING" in prompt for prompt in prompts)
    if non_converging_requests == 0 or non_converging_requests > 10:
        raise AssertionError(f"non-converging aggregate was not stopped promptly: {non_converging_requests} requests")


def run_duckdb_optimized_classifier(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_task_model = 'mock-task';
        SET duckdb_ai_embedding_model = 'mock-embedding';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
        CREATE TEMP TABLE training AS
        SELECT CASE WHEN i % 2 = 0 THEN 'alpha example ' ELSE 'beta example ' END || i::VARCHAR AS text
        FROM range(20) t(i);
        CREATE TEMP TABLE classifier AS
        SELECT ai_build_classifier(
            text,
            ['class_a', 'class_b'],
            optimization := 'minimize_cost',
            sample_size := 20,
            quality_threshold := 0.75,
            confidence_margin := 0.05
        ) AS artifact
        FROM training;
        SELECT contains(artifact, '"optimization":"minimize_cost"') AS optimization,
               contains(artifact, '"usable":true') AS usable,
               contains(artifact, '"validation_count":') AS validation_count
        FROM classifier;
        SELECT result.value AS local_value, result.used_fallback AS local_fallback,
               contains(result.metadata, '"path":"local"') AS local_metadata
        FROM (SELECT ai_classify_optimized('alpha new', artifact) AS result FROM classifier);
        SELECT result.value AS fallback_value, result.used_fallback AS used_fallback,
               contains(result.metadata, '"path":"fallback"') AS fallback_metadata,
               result.error IS NULL AS fallback_ok
        FROM (SELECT ai_classify_optimized('ambiguous example', artifact) AS result FROM classifier);
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb optimized classifier smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_optimized_classifier(output: str):
    for expected in (
        "optimization",
        "usable",
        "validation_count",
        "local_value",
        "class_a",
        "local_fallback",
        "fallback_value",
        "class_b",
        "used_fallback",
        "fallback_ok",
    ):
        if expected not in output:
            raise AssertionError(f"optimized classifier output missing {expected!r}\n{output}")
    if len(MockProviderHandler.completion_requests) != 21:
        raise AssertionError(
            f"expected 20 sampled labels and one fallback, got {len(MockProviderHandler.completion_requests)}"
        )
    if len(MockProviderHandler.embedding_requests) != 3:
        raise AssertionError(
            f"expected three packed/local embedding requests, got {MockProviderHandler.embedding_requests}"
        )
    training_inputs = MockProviderHandler.embedding_requests[0]["input"]
    if not isinstance(training_inputs, list) or len(training_inputs) != 20:
        raise AssertionError(f"classifier training embeddings were not batched: {training_inputs}")


def run_duckdb_new_task_functions(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_task_model = 'mock-task';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_timeout_seconds = 5;
        SELECT ai_score('DuckDB runs in process', 'describes an embedded database') AS score;
        SELECT ai_classify(
            'alpha example', ['class_a', 'class_b'],
            label_descriptions := '{{"class_a":"alpha text","class_b":"beta text"}}',
            instructions := 'Prefer explicit terms.',
            examples := '[{{"input":"alpha","label":"class_a"}}]'
        ) AS rich_classification;
        SELECT result.value, result.error IS NULL, contains(result.metadata, '"multi_label":true')
        FROM (
            SELECT ai_classify_result(
                'invoice overdue and slow app', ['billing, overdue', 'performance', 'support']
            ) AS result
        );
        SELECT ai_classify('return unknown label', ['red', 'blue'],
                           fail_on_error := false) IS NULL AS unknown_single_rejected;
        SELECT ai_classify_labels('return unknown labels', ['red', 'blue'],
                                  fail_on_error := false) IS NULL AS unknown_multi_rejected;
        SELECT ai_classify('alpha example', ['class_a', 'CLASS_A'],
                           fail_on_error := false) IS NULL AS duplicate_labels_rejected;
        SELECT contains(result.error, 'outside the allowed label set') AS diagnostic_error,
               contains(result.metadata, '"status":"error"') AS diagnostic_metadata
        FROM (
            SELECT ai_classify_result('return unknown labels', ['red', 'blue']) AS result
        );
        SELECT contains(result.error, 'labels must be unique') AS duplicate_diagnostic_error,
               contains(result.metadata, '"status":"error"') AS duplicate_diagnostic_metadata
        FROM (
            SELECT ai_classify_result('invoice overdue', ['billing', 'BILLING']) AS result
        );
    """
    env = os.environ.copy()
    env["OPENAI_API_KEY"] = "test-key"
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb new task function smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_new_task_functions(output: str):
    for expected in (
        "score",
        "0.91",
        "rich_classification",
        "class_a",
        "billing, overdue",
        "performance",
        "unknown_single_rejected",
        "unknown_multi_rejected",
        "duplicate_labels_rejected",
        "diagnostic_error",
        "diagnostic_metadata",
        "duplicate_diagnostic_error",
        "duplicate_diagnostic_metadata",
    ):
        if expected not in output:
            raise AssertionError(f"new task function output missing {expected!r}\n{output}")
    score_format = MockProviderHandler.completion_requests[0].get("response_format", {})
    score_schema = score_format.get("json_schema", {}).get("schema", {})
    if score_format.get("type") != "json_schema" or "score" not in score_schema.get("properties", {}):
        raise AssertionError(f"ai_score did not send its constrained score schema: {score_format}")
    rich_request = MockProviderHandler.completion_requests[1]
    system_prompt = rich_request["messages"][0]["content"]
    for expected in ("Label descriptions", "Prefer explicit terms.", "Examples"):
        if expected not in system_prompt:
            raise AssertionError(f"rich classification prompt missing {expected!r}: {system_prompt}")


def run_duckdb_control_plane(duckdb_path: Path, base_url: str) -> str:
    sql = """
        CREATE EXTERNAL MODEL provisioned_model WITH (
            provider = 'azure',
            model = 'gpt-test',
            location = 'https://safe-metadata.example/v1',
            capabilities = 'completion,json_schema'
        );
        SELECT operation_id, status, estimated_hourly_cost_usd
        FROM ai_provision_endpoint(
            'provisioned_model', dry_run := false, max_hourly_cost_usd := 2.0
        );
        SELECT operation_id, status, endpoint_url FROM ai_endpoint_status('provision-op');
        SELECT operation_id, status FROM ai_deprovision_endpoint('provisioned_model');
        SELECT document_id, page, element_index, element_type, text, markdown,
               bbox, confidence, metadata, error IS NULL
        FROM ai_parse_document('abc'::BLOB, 'text/plain', 'documents');
        SELECT error
        FROM ai_parse_document('abc'::BLOB, 'text/plain', 'malformed', fail_on_error := false);
        SELECT document_id, error
        FROM ai_parse_document('abc'::BLOB, 'text/plain', 'error-only', fail_on_error := false);
    """
    env = os.environ.copy()
    env.update(
        {
            "DUCKDB_AI_CONTROL_PLANE_URL": base_url,
            "DUCKDB_AI_CONTROL_PLANE_TOKEN": "control-token",
        }
    )
    result = subprocess.run(
        [str(duckdb_path), "-c", sql],
        cwd=repo_root(),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"duckdb control-plane smoke exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def assert_control_plane(output: str):
    for expected in (
        "provision-op",
        "submitted",
        "1.25",
        "ready",
        "https://endpoint.example/v1",
        "deprovision-op",
        "document-1",
        "paragraph",
        "DuckDB document",
        "0.99",
        "AI document parser response must contain an elements array",
        "document-error",
        "remote parser failed",
    ):
        if expected not in output:
            raise AssertionError(f"control-plane output missing {expected!r}\n{output}")
    paths = [path for path, _ in MockProviderHandler.control_plane_requests]
    if paths != [
        "/v1/endpoints/provision",
        "/v1/endpoints/status",
        "/v1/endpoints/deprovision",
        "/v1/documents/parse",
        "/v1/documents/parse",
        "/v1/documents/parse",
    ]:
        raise AssertionError(f"unexpected control-plane requests: {MockProviderHandler.control_plane_requests}")
    if MockProviderHandler.control_plane_authorization_headers != ["Bearer control-token"] * 6:
        raise AssertionError(
            f"unexpected control-plane authorization: {MockProviderHandler.control_plane_authorization_headers}"
        )
    document_request = MockProviderHandler.control_plane_requests[3][1]
    if document_request.get("content_base64") != "YWJj" or document_request.get("mime_type") != "text/plain":
        raise AssertionError(f"unexpected normalized document request: {document_request}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--duckdb",
        type=Path,
        default=repo_root() / "build" / "release" / "duckdb",
        help="Path to the built DuckDB shell with duckdb_ai linked",
    )
    parser.add_argument("--tls-only", action="store_true", help="Run only the HTTPS trust-store regression suite")
    args = parser.parse_args()
    if not args.duckdb.exists():
        raise SystemExit(f"{args.duckdb} does not exist; run `GEN=ninja make release` first")

    server = HTTPServer(("127.0.0.1", 0), MockProviderHandler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        provider_metadata_output = run_duckdb_provider_metadata(args.duckdb)
        assert_provider_metadata(provider_metadata_output)
        run_tls_smoke_suite(args.duckdb, f"http://127.0.0.1:{port}")
        if args.tls_only:
            print("tls trust smoke passed")
            return
        MockProviderHandler.reset()
        output = run_duckdb(args.duckdb, f"http://127.0.0.1:{port}")
        assert_smoke_result(output)
        MockProviderHandler.reset()
        adaptive_batch_output = run_duckdb_adaptive_batches(args.duckdb, f"http://127.0.0.1:{port}")
        assert_adaptive_batches(adaptive_batch_output)
        MockProviderHandler.reset()
        usage_distribution_output = run_duckdb_embedding_usage_distribution(args.duckdb, f"http://127.0.0.1:{port}")
        assert_embedding_usage_distribution(usage_distribution_output)
        MockProviderHandler.reset()
        threaded_server = ThreadingHTTPServer(("127.0.0.1", 0), MockProviderHandler)
        threaded_thread = threading.Thread(target=threaded_server.serve_forever, daemon=True)
        threaded_thread.start()
        try:
            concurrency_output = run_duckdb_executor_concurrency(
                args.duckdb, f"http://127.0.0.1:{threaded_server.server_address[1]}"
            )
            assert_executor_concurrency(concurrency_output)
        finally:
            threaded_server.shutdown()
            threaded_thread.join(timeout=5)
        MockProviderHandler.reset()
        hierarchical_output = run_duckdb_hierarchical_aggregate(args.duckdb, f"http://127.0.0.1:{port}")
        assert_hierarchical_aggregate(hierarchical_output)
        MockProviderHandler.reset()
        optimized_classifier_output = run_duckdb_optimized_classifier(args.duckdb, f"http://127.0.0.1:{port}")
        assert_optimized_classifier(optimized_classifier_output)
        MockProviderHandler.reset()
        new_task_output = run_duckdb_new_task_functions(args.duckdb, f"http://127.0.0.1:{port}")
        assert_new_task_functions(new_task_output)
        MockProviderHandler.reset()
        control_plane_output = run_duckdb_control_plane(args.duckdb, f"http://127.0.0.1:{port}")
        assert_control_plane(control_plane_output)
        MockProviderHandler.reset()
        self_correction_output = run_duckdb_self_correction(args.duckdb, f"http://127.0.0.1:{port}")
        assert_self_correction_result(self_correction_output)
        MockProviderHandler.reset()
        provider_error_output = run_duckdb_provider_error(args.duckdb, f"http://127.0.0.1:{port}")
        assert_provider_error(provider_error_output)
        MockProviderHandler.reset()
        openrouter_headers_output = run_duckdb_openrouter_headers(args.duckdb, f"http://127.0.0.1:{port}")
        assert_openrouter_headers(openrouter_headers_output)
        MockProviderHandler.reset()
        xai_header_output = run_duckdb_xai_prompt_cache_header(args.duckdb, f"http://127.0.0.1:{port}")
        assert_xai_prompt_cache_header(xai_header_output)
        MockProviderHandler.reset()
        cache_output = run_duckdb_cache_and_allowlist(args.duckdb, f"http://127.0.0.1:{port}")
        assert_cache_and_allowlist_result(cache_output)
        MockProviderHandler.reset()
        allowlist_error_output = run_duckdb_allowlist_error(args.duckdb, f"http://127.0.0.1:{port}")
        assert_allowlist_error(allowlist_error_output)
        MockProviderHandler.reset()
        strict_log_error_output = run_duckdb_strict_log_error(args.duckdb, f"http://127.0.0.1:{port}", port)
        assert_strict_log_error(strict_log_error_output)
        MockProviderHandler.reset()
        invalid_env_log_sample_rate_output = run_duckdb_invalid_env_log_sample_rate(args.duckdb)
        assert_invalid_env_log_sample_rate(invalid_env_log_sample_rate_output)
        MockProviderHandler.reset()
        invalid_url_scheme_output = run_duckdb_invalid_url_scheme(args.duckdb)
        assert_invalid_url_scheme(invalid_url_scheme_output)
        MockProviderHandler.reset()
        invalid_header_output = run_duckdb_invalid_header(args.duckdb, f"http://127.0.0.1:{port}")
        assert_invalid_header(invalid_header_output)
        MockProviderHandler.reset()
        oversized_response_output = run_duckdb_oversized_response(args.duckdb, f"http://127.0.0.1:{port}")
        assert_oversized_response(oversized_response_output)
    finally:
        server.shutdown()
        thread.join(timeout=5)

    print("mock provider smoke passed")


if __name__ == "__main__":
    main()
