#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


class MockProviderHandler(BaseHTTPRequestHandler):
    completion_requests = []
    embedding_requests = []
    privacy_filter_requests = []
    ollama_requests = []
    claude_requests = []
    log_requests = []
    authorization_headers = []
    openrouter_referer_headers = []
    openrouter_title_headers = []
    claude_api_keys = []
    claude_versions = []
    retry_completion_attempts = 0

    @classmethod
    def reset(cls):
        cls.completion_requests.clear()
        cls.embedding_requests.clear()
        cls.privacy_filter_requests.clear()
        cls.ollama_requests.clear()
        cls.claude_requests.clear()
        cls.log_requests.clear()
        cls.authorization_headers.clear()
        cls.openrouter_referer_headers.clear()
        cls.openrouter_title_headers.clear()
        cls.claude_api_keys.clear()
        cls.claude_versions.clear()
        cls.retry_completion_attempts = 0

    def do_POST(self):
        length = int(self.headers.get("content-length", "0"))
        body = self.rfile.read(length).decode("utf-8")

        if self.path.endswith("/chat/completions"):
            self.authorization_headers.append(self.headers.get("authorization"))
            if self.headers.get("x-openrouter-title") or self.headers.get("http-referer"):
                self.openrouter_referer_headers.append(self.headers.get("http-referer"))
                self.openrouter_title_headers.append(self.headers.get("x-openrouter-title"))
            request = json.loads(body)
            self.completion_requests.append(request)
            prompt = request["messages"][-1]["content"]
            full_prompt = "\n".join(message["content"] for message in request["messages"])
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
                content = '["billing, overdue","performance"]'
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
            self._send_json(payload)
            return

        if self.path == "/embeddings":
            self.authorization_headers.append(self.headers.get("authorization"))
            request = json.loads(body)
            self.embedding_requests.append(request)
            inputs = request.get("input")
            input_count = len(inputs) if isinstance(inputs, list) else 1
            payload = {
                "data": [{"embedding": [0.25, -0.5, 1.25]} for _ in range(input_count)],
                "usage": {
                    "prompt_tokens": 2 * input_count,
                    "total_tokens": 2 * input_count,
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
        SET duckdb_ai_completion_model = 'mock-completion-model';
        SET duckdb_ai_task_model = 'mock-task-model';
        SET duckdb_ai_aggregate_model = 'mock-aggregate-model';
        SET duckdb_ai_embedding_model = 'mock-embedding-default';
        SET duckdb_ai_sql_assistant_model = 'mock-sql-model';
        SET duckdb_ai_base_url = '{base_url}';
        SET duckdb_ai_log_endpoint = '{base_url}/log';
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
            MODEL 'databricks-llama-4-maverick'
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
            model := 'databricks-llama-4-maverick'
        ) AS databricks_completion;
        SELECT ai_complete(
            'hello snowflake',
            secret := 'smoke_snowflake_ai',
            provider := 'snowflake',
            model := 'claude-sonnet-4-5'
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
        "https://api.hunyuan.cloud.tencent.com/v1",
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
        "databricks-llama-4-maverick",
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
    if len(MockProviderHandler.authorization_headers) != 44:
        raise AssertionError(f"expected 44 auth headers, got {len(MockProviderHandler.authorization_headers)}")
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
    if completion_request.get("max_tokens") != 17:
        raise AssertionError(f"unexpected completion max_tokens: {completion_request}")

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
    if databricks_request.get("model") != "databricks-llama-4-maverick":
        raise AssertionError(f"unexpected Databricks model: {databricks_request}")
    if databricks_request["messages"][-1]["content"] != "hello databricks":
        raise AssertionError(f"unexpected Databricks prompt: {databricks_request}")
    snowflake_request = MockProviderHandler.completion_requests[30]
    if snowflake_request.get("model") != "claude-sonnet-4-5":
        raise AssertionError(f"unexpected Snowflake model: {snowflake_request}")
    if snowflake_request["messages"][-1]["content"] != "hello snowflake":
        raise AssertionError(f"unexpected Snowflake prompt: {snowflake_request}")
    try_complete_request = MockProviderHandler.completion_requests[31]
    if try_complete_request["messages"][-1]["content"] != "try complete smoke":
        raise AssertionError(f"unexpected try completion prompt: {try_complete_request}")
    if try_complete_request.get("max_tokens") != 5:
        raise AssertionError(f"unexpected try completion max tokens: {try_complete_request}")
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

    if len(MockProviderHandler.embedding_requests) != 9:
        raise AssertionError(f"expected 9 embedding requests, got {len(MockProviderHandler.embedding_requests)}")
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
    similarity_inputs = [request["input"] for request in MockProviderHandler.embedding_requests[3:5]]
    if similarity_inputs != ["same left", "same right"]:
        raise AssertionError(f"unexpected similarity embedding inputs: {similarity_inputs}")
    similarity_models = [request["model"] for request in MockProviderHandler.embedding_requests[3:5]]
    if similarity_models != ["mock-embedding-default", "mock-embedding-default"]:
        raise AssertionError(f"unexpected similarity embedding models: {similarity_models}")
    constant_similarity_inputs = [request["input"] for request in MockProviderHandler.embedding_requests[5:]]
    if constant_similarity_inputs.count("constant query") != 1:
        raise AssertionError(f"expected one constant-side embedding, got {constant_similarity_inputs}")
    if set(constant_similarity_inputs) != {"constant query", "candidate one", "candidate two", "candidate three"}:
        raise AssertionError(f"unexpected constant similarity embedding inputs: {constant_similarity_inputs}")

    log_deadline = time.time() + 5
    while len(MockProviderHandler.log_requests) < 47 and time.time() < log_deadline:
        time.sleep(0.05)
    if len(MockProviderHandler.log_requests) != 47:
        raise AssertionError(f"expected 47 log requests, got {len(MockProviderHandler.log_requests)}")
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
        (request for request in completion_logs if request.get("model") == "databricks-llama-4-maverick"), None
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
        provider_metadata_output = run_duckdb_provider_metadata(args.duckdb)
        assert_provider_metadata(provider_metadata_output)
        output = run_duckdb(args.duckdb, f"http://127.0.0.1:{port}")
        assert_smoke_result(output)
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
    finally:
        server.shutdown()
        thread.join(timeout=5)

    print("mock provider smoke passed")


if __name__ == "__main__":
    main()
