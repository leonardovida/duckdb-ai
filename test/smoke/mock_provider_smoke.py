#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


class MockProviderHandler(BaseHTTPRequestHandler):
    completion_requests = []
    embedding_requests = []
    ollama_requests = []
    claude_requests = []
    log_requests = []
    authorization_headers = []
    claude_api_keys = []
    claude_versions = []
    retry_completion_attempts = 0

    def do_POST(self):
        length = int(self.headers.get("content-length", "0"))
        body = self.rfile.read(length).decode("utf-8")

        if self.path == "/chat/completions":
            self.authorization_headers.append(self.headers.get("authorization"))
            request = json.loads(body)
            self.completion_requests.append(request)
            prompt = request["messages"][-1]["content"]
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
            content = "mock completion"
            if prompt == "retry once":
                content = "mock retry completion"
            elif "return invalid schema JSON" in prompt:
                content = '{"summary":123}'
            elif "return constrained schema JSON" in prompt:
                content = '{"summary":"mock structured","score":0.5,"tags":["duck","ai"]}'
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
            elif request.get("response_format") or "Return only valid JSON" in prompt:
                content = '{"summary":"mock structured"}'
            elif "Explain the DuckDB SQL query" in prompt:
                content = "This query returns a single row with 42."
            elif "Correct the DuckDB SQL query" in prompt:
                content = "```sql\nSELECT 42\n```"
            elif "Correct one line in the DuckDB SQL query" in prompt:
                content = "SELECT 42"
            elif "Generate one DuckDB SQL SELECT statement" in prompt:
                content = "```sql\nSELECT 42\n```"
            payload = {
                "choices": [{"message": {"content": content}}],
                "usage": {
                    "prompt_tokens": 7,
                    "completion_tokens": 3,
                    "total_tokens": 10,
                },
            }
            self._send_json(payload)
            return

        if self.path == "/embeddings":
            self.authorization_headers.append(self.headers.get("authorization"))
            self.embedding_requests.append(json.loads(body))
            payload = {
                "data": [{"embedding": [0.25, -0.5, 1.25]}],
                "usage": {
                    "prompt_tokens": 2,
                    "total_tokens": 2,
                },
            }
            self._send_json(payload)
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


def run_duckdb(duckdb_path: Path, base_url: str) -> str:
    sql = f"""
        SELECT * FROM ai_clear_usage();
        SET duckdb_ai_provider = 'openai';
        SET duckdb_ai_model = 'mock-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_log_endpoint = '{base_url}/log';
        SET duckdb_ai_timeout_seconds = 5;
        CREATE OR REPLACE SECRET smoke_duckdb_ai (
            TYPE duckdb_ai,
            API_KEY 'test-key',
            AI_PROVIDER 'openai'
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
        SELECT ai_classify('invoice overdue', 'billing, support') AS classification;
        SELECT ai_extract('name: DuckDB', 'name') AS extracted;
        SELECT ai_sql(
            'count rows',
            schema_context := 'CREATE TABLE smoke_table(id INTEGER)'
        ) AS generated_sql;
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
        FROM ai_fix_sql_line(
            'SEELECT 42',
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
        SELECT ai_embed('embed smoke', model := 'mock-embedding-model')[1] AS first_embedding_value;
        SELECT round(ai_similarity('same left', 'same right', model := 'mock-embedding-model'), 6) AS similarity;
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
        "mock-model",
        "0.25",
        "1.0",
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

    if len(MockProviderHandler.completion_requests) != 28:
        raise AssertionError(f"expected 28 completion requests, got {len(MockProviderHandler.completion_requests)}")
    if len(MockProviderHandler.authorization_headers) != 31:
        raise AssertionError(f"expected 31 auth headers, got {len(MockProviderHandler.authorization_headers)}")
    for header in MockProviderHandler.authorization_headers:
        if header != "Bearer test-key":
            raise AssertionError(f"unexpected authorization header: {header}")

    completion_request = MockProviderHandler.completion_requests[0]
    if completion_request["model"] != "mock-model":
        raise AssertionError(f"unexpected completion model: {completion_request}")
    if completion_request["messages"][0] != {"role": "system", "content": "answer briefly"}:
        raise AssertionError(f"unexpected completion system prompt: {completion_request}")
    if completion_request["messages"][1]["content"] != "hello from smoke":
        raise AssertionError(f"unexpected completion prompt: {completion_request}")
    if completion_request.get("temperature") != 0.2:
        raise AssertionError(f"unexpected completion temperature: {completion_request}")
    if completion_request.get("max_tokens") != 17:
        raise AssertionError(f"unexpected completion max_tokens: {completion_request}")

    structured_request = MockProviderHandler.completion_requests[1]
    response_format = structured_request.get("response_format")
    if response_format is None or response_format.get("type") != "json_schema":
        raise AssertionError(f"unexpected structured response format: {structured_request}")
    if response_format.get("json_schema", {}).get("schema", {}).get("type") != "object":
        raise AssertionError(f"unexpected structured response schema: {structured_request}")
    structured_prompt = structured_request["messages"][-1]["content"]
    if "Return only valid JSON" not in structured_prompt or "return a smoke JSON object" not in structured_prompt:
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

    task_prompts = [request["messages"][-1]["content"] for request in MockProviderHandler.completion_requests[9:16]]
    expected_prompt_fragments = [
        "Summarize the following text concisely",
        "positive, neutral, or negative",
        "Fix grammar, spelling, and punctuation",
        "Mask direct personal data",
        "Translate the following text to Dutch",
        "exactly one of these labels: billing, support",
        "Extraction request:\nname",
    ]
    for fragment, prompt in zip(expected_prompt_fragments, task_prompts):
        if fragment not in prompt:
            raise AssertionError(f"task prompt missing {fragment!r}: {prompt}")

    ai_sql_prompt = MockProviderHandler.completion_requests[16]["messages"][-1]["content"]
    if "Generate one DuckDB SQL SELECT statement" not in ai_sql_prompt:
        raise AssertionError(f"unexpected ai_sql prompt: {ai_sql_prompt}")
    if "CREATE TABLE smoke_table(id INTEGER)" not in ai_sql_prompt:
        raise AssertionError(f"ai_sql prompt missing schema context: {ai_sql_prompt}")
    ai_query_data_prompt = MockProviderHandler.completion_requests[17]["messages"][-1]["content"]
    if "Generate one DuckDB SQL SELECT statement" not in ai_query_data_prompt:
        raise AssertionError(f"unexpected ai_query_data prompt: {ai_query_data_prompt}")
    if "CREATE TABLE smoke_table(id INTEGER)" not in ai_query_data_prompt:
        raise AssertionError(f"ai_query_data prompt missing schema context: {ai_query_data_prompt}")
    ai_explain_sql_prompt = MockProviderHandler.completion_requests[18]["messages"][-1]["content"]
    if "Explain the DuckDB SQL query" not in ai_explain_sql_prompt:
        raise AssertionError(f"unexpected ai_explain_sql prompt: {ai_explain_sql_prompt}")
    if "SELECT 42" not in ai_explain_sql_prompt:
        raise AssertionError(f"ai_explain_sql prompt missing SQL: {ai_explain_sql_prompt}")
    ai_fix_sql_prompt = MockProviderHandler.completion_requests[19]["messages"][-1]["content"]
    if "Correct the DuckDB SQL query" not in ai_fix_sql_prompt:
        raise AssertionError(f"unexpected ai_fix_sql prompt: {ai_fix_sql_prompt}")
    if "SEELECT 42" not in ai_fix_sql_prompt:
        raise AssertionError(f"ai_fix_sql prompt missing broken SQL: {ai_fix_sql_prompt}")
    ai_fix_sql_line_prompt = MockProviderHandler.completion_requests[20]["messages"][-1]["content"]
    if "Correct one line in the DuckDB SQL query" not in ai_fix_sql_line_prompt:
        raise AssertionError(f"unexpected ai_fix_sql_line prompt: {ai_fix_sql_line_prompt}")
    if "Line to correct: 1" not in ai_fix_sql_line_prompt:
        raise AssertionError(f"ai_fix_sql_line prompt missing line number: {ai_fix_sql_line_prompt}")
    summarize_agg_prompt = MockProviderHandler.completion_requests[21]["messages"][-1]["content"]
    if "Summarize the following SQL aggregate input values" not in summarize_agg_prompt:
        raise AssertionError(f"unexpected ai_summarize_agg prompt: {summarize_agg_prompt}")
    if "first row" not in summarize_agg_prompt or "second row" not in summarize_agg_prompt:
        raise AssertionError(f"ai_summarize_agg prompt missing aggregate values: {summarize_agg_prompt}")
    agg_prompt = MockProviderHandler.completion_requests[22]["messages"][-1]["content"]
    if "Return the most important word" not in agg_prompt:
        raise AssertionError(f"unexpected ai_agg prompt: {agg_prompt}")
    if "duck" not in agg_prompt or "database" not in agg_prompt:
        raise AssertionError(f"ai_agg prompt missing aggregate values: {agg_prompt}")
    suppressed_prompt = MockProviderHandler.completion_requests[23]["messages"][-1]["content"]
    if suppressed_prompt != "sample suppressed":
        raise AssertionError(f"unexpected suppressed completion prompt: {suppressed_prompt}")
    retry_prompts = [request["messages"][-1]["content"] for request in MockProviderHandler.completion_requests[24:26]]
    if retry_prompts != ["retry once", "retry once"]:
        raise AssertionError(f"unexpected retry prompts: {retry_prompts}")
    if MockProviderHandler.retry_completion_attempts != 2:
        raise AssertionError(f"expected 2 retry attempts, got {MockProviderHandler.retry_completion_attempts}")
    otlp_prompt = MockProviderHandler.completion_requests[26]["messages"][-1]["content"]
    if otlp_prompt != "otlp log smoke":
        raise AssertionError(f"unexpected OTLP completion prompt: {otlp_prompt}")
    builtin_price_request = MockProviderHandler.completion_requests[27]
    if builtin_price_request.get("model") != "gpt-5.4-mini":
        raise AssertionError(f"unexpected builtin pricing model: {builtin_price_request}")
    if builtin_price_request["messages"][-1]["content"] != "builtin pricing smoke":
        raise AssertionError(f"unexpected builtin pricing prompt: {builtin_price_request}")

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

    if len(MockProviderHandler.embedding_requests) != 3:
        raise AssertionError(f"expected 3 embedding requests, got {len(MockProviderHandler.embedding_requests)}")
    embedding_request = MockProviderHandler.embedding_requests[0]
    if embedding_request["model"] != "mock-embedding-model":
        raise AssertionError(f"unexpected embedding model: {embedding_request}")
    if embedding_request["input"] != "embed smoke":
        raise AssertionError(f"unexpected embedding input: {embedding_request}")
    similarity_inputs = [request["input"] for request in MockProviderHandler.embedding_requests[1:]]
    if similarity_inputs != ["same left", "same right"]:
        raise AssertionError(f"unexpected similarity embedding inputs: {similarity_inputs}")

    if len(MockProviderHandler.log_requests) != 31:
        raise AssertionError(f"expected 31 log requests, got {len(MockProviderHandler.log_requests)}")
    completion_logs = [
        request for request in MockProviderHandler.log_requests if request.get("event") == "ai_completion"
    ]
    embedding_logs = [request for request in MockProviderHandler.log_requests if request.get("event") == "ai_embedding"]
    otlp_logs = [request for request in MockProviderHandler.log_requests if "resourceLogs" in request]
    if len(completion_logs) != 27 or len(embedding_logs) != 3:
        raise AssertionError(f"unexpected log events: {MockProviderHandler.log_requests}")
    if len(otlp_logs) != 1:
        raise AssertionError(f"expected 1 OTLP log request, got {otlp_logs}")
    logged_providers = {request.get("provider") for request in completion_logs}
    if not {"openai", "ollama", "claude"}.issubset(logged_providers):
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
        "ai.provider": "openai",
        "ai.protocol": "openai_chat",
        "ai.model": "mock-model",
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

    log_request = completion_logs[0]
    expected_log_fields = {
        "extension": "duckdb_ai",
        "event": "ai_completion",
        "provider": "openai",
        "protocol": "openai_chat",
        "model": "mock-model",
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
    estimated_cost = log_request.get("estimated_cost_usd")
    if estimated_cost is None or abs(estimated_cost - 0.000013) > 0.000000001:
        raise AssertionError(f"unexpected estimated cost: {log_request}")
    builtin_price_log = next((request for request in completion_logs if request.get("model") == "gpt-5.4-mini"), None)
    if builtin_price_log is None:
        raise AssertionError(f"missing builtin pricing log: {completion_logs}")
    builtin_estimated_cost = builtin_price_log.get("estimated_cost_usd")
    if builtin_estimated_cost is None or abs(builtin_estimated_cost - 0.00001875) > 0.000000001:
        raise AssertionError(f"unexpected builtin pricing cost: {builtin_price_log}")
    for completion_log in completion_logs:
        if "prompt" in completion_log or "response" in completion_log:
            raise AssertionError(f"log request unexpectedly included prompt/response text: {completion_log}")

    embedding_log_request = embedding_logs[0]
    expected_embedding_log_fields = {
        "extension": "duckdb_ai",
        "event": "ai_embedding",
        "provider": "openai",
        "protocol": "openai_embeddings",
        "model": "mock-embedding-model",
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
    if "input" in embedding_log_request:
        raise AssertionError(f"embedding log request unexpectedly included input text: {embedding_log_request}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--duckdb",
        type=Path,
        default=repo_root() / "build" / "release" / "duckdb",
        help="Path to the built DuckDB shell with duckdb_ai linked",
    )
    args = parser.parse_args()
    if not args.duckdb.exists():
        raise SystemExit(f"{args.duckdb} does not exist; run `GEN=ninja make release` first")

    server = HTTPServer(("127.0.0.1", 0), MockProviderHandler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        output = run_duckdb(args.duckdb, f"http://127.0.0.1:{port}")
        assert_smoke_result(output)
        provider_error_output = run_duckdb_provider_error(args.duckdb, f"http://127.0.0.1:{port}")
        assert_provider_error(provider_error_output)
    finally:
        server.shutdown()
        thread.join(timeout=5)

    print("mock provider smoke passed")


if __name__ == "__main__":
    main()
