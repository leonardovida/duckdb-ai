---
title: Use DuckDB AI from coding agents
sidebar_label: Agent guide
keywords: ["DuckDB AI agent", "LLM SQL for coding agents", "Claude Code DuckDB", "llms.txt", "DuckDB function discovery"]
description: Discover DuckDB AI SQL functions, configure model providers, preview requests without credentials, and verify integrations with local mocks.
---

# Use DuckDB AI from coding agents

Use this guide when an agent writes SQL against the `ai` extension or changes
its source. Start with the installed API: source documentation may describe
features that have not reached the DuckDB community package yet.

## Identify the package and installed API

| Name | Meaning |
| --- | --- |
| `duckdb-ai` | Repository and project name |
| `ai` | Extension name in `INSTALL` and `LOAD` |
| `ai_*` | SQL function prefix |
| `duckdb_ai_*` | DuckDB setting prefix |
| `TYPE duckdb_ai` | DuckDB secret type |

After installation, run these checks in the connection that will run AI SQL:

```sql
LOAD ai;

SELECT extension_name, extension_version, loaded
FROM duckdb_extensions()
WHERE extension_name = 'ai';

SELECT function_name, function_type, parameters, parameter_types, return_type
FROM duckdb_functions()
WHERE starts_with(function_name, 'ai_')
ORDER BY function_name;
```

Use [the function reference](functions.md) for named options and full signatures.
The runtime function catalog may display generic parameter names and separate
overloads. Table functions such as `ai_usage()` and `ai_complete_record()` belong
in `FROM`. `ai_extract_record()` and `ai_decide()` are scalars that return one
`STRUCT` per input row.

## Configure the intended provider

Read [the provider matrix](provider-guides.md#provider-matrix), then choose the
provider, an available model, and any required base URL. Use an embedding model
for `ai_embed` and `ai_similarity`; `ai_rerank` uses a completion model.

Keep keys in a process environment or a secret manager. If a named DuckDB secret
is used, pass `secret := 'name'` explicitly to make the intended configuration
clear. Do not put credentials into prompts, `request_options`, source files, or
test output. The hosted example in the [repository README](https://github.com/leonardovida/duckdb-ai#connect-a-hosted-provider) uses
`OPENAI_API_KEY` from the process environment.

Use explicit per-call `provider` and `model` options for isolated examples.
For repeated queries, configure session defaults or a named secret. Session
settings outrank the `MODEL` stored in a secret, so a leftover
`SET duckdb_ai_model` sends that model to whatever provider the secret names.
Check [the resolution order](functions.md#provider-settings-and-secrets)
before combining them, and `RESET` settings you no longer need.

For `ai_decide`, the session settings `duckdb_ai_model` and
`duckdb_ai_base_url` are ignored, and so is the `MODEL` of a chat provider's
secret. Pass `model := ...` or set `<PROVIDER>_DECISION_MODEL` (for example
`OLLAMA_DECISION_MODEL`). Only `typesafe`, `cloudflare`, `perplexity`,
`ollama` (0.35 or later) and `systemone` support it.

## Preview before making a model call

This query constructs JSON locally and needs no API key:

```sql
SELECT ai_completion_request_json(
    'Reply OK',
    provider := 'openai',
    model := 'gpt-5.6-luna',
    max_tokens := 64
);
```

Check the model, messages, and token-limit field. `ai_embedding_request_json`
provides the equivalent preview for embeddings. A correct preview proves request
construction, not authentication, model availability, or successful inference.

For an authorized live check, use one short prompt and a bounded `max_tokens`.
Reasoning models may spend the budget before producing visible text; use their
documented reasoning controls rather than repeatedly increasing the cap.
Inspect `ai_usage()` for status and token counts. A missing-key or insufficient-
credit response does not verify a successful completion.

## Handle results and data access deliberately

- Use `ai_try_complete` for `STRUCT(response, error)` results when individual
  failures should not abort a batch. Materialize results before separating
  successful and failed rows to avoid repeating model calls.
- Use `ai_complete_json` with `response_schema` for validated JSON, or the record
  functions for typed output. `ai_extract` alone does not establish a JSON Schema
  contract.
- Limit SQL assistant context with `include_tables` and review any sampled data
  before sending it to a hosted provider. `ai_sql` returns SQL text;
  `ai_query_data` runs generated SQL as a subquery. Read-only validation does not
  replace DuckDB permissions or external-access restrictions.
- Loading the extension does not start inference. Model calls and configured
  logging endpoints can send data outside the process. A locally hosted model
  is private only within the deployment and logging configuration you chose.

See [security and data flow](security-data-flow.md) for the data sent by each
function family, and [runtime behavior](runtime-behavior.md) for retries,
rate limits, caching, and cancellation.

## Avoid repeated or unbounded model calls

- Model functions run once per input row. Bound the input in a subquery, for
  example `FROM (SELECT * FROM t ORDER BY id LIMIT 100)`. An outer `OFFSET` can
  still evaluate model calls for the rows it skips, so paginate inside the
  input subquery.
- Model functions are `VOLATILE`: re-running a query, or referencing the same
  call in several places, calls the provider again. Save results with
  `CREATE TABLE ... AS SELECT ...` and read fields from that table.
- `cache := true` (or `SET duckdb_ai_cache = true`) adds an opt-in in-memory
  response cache for the current DuckDB instance. It does not persist across
  processes.
- Use `ai_count_tokens` and `ai_recommended_batch_size` to size batches before
  calling a rate-limited provider.

## Chain several AI steps

When a task needs more than one model call per row:

- First try to merge steps: `ai_decide` answers several typed questions in one
  request, and `ai_extract_record` returns several fields in one call. Keep
  `ai_redact` as its own step.
- Call each model function once per row in a subquery, then read its fields
  outside: `SELECT r.response, r.error FROM (SELECT ai_try_complete(x) AS r ...)`.
  Writing `(ai_try_complete(x)).response, (ai_try_complete(x)).error` calls the
  model twice.
- Do not reuse a model call's alias in the same `SELECT`
  (`SELECT ai_redact(x) AS clean, ai_summarize(clean)` is a binder error). Use a
  subquery or a step table.
- Pass NULL to skip a row: NULL inputs make no call. `coalesce(x, '')` turns a
  skip into a paid request for an empty string (or an error for `ai_complete`).
- Feed the next step only successful rows (`WHERE error IS NULL`), and put
  conditional steps behind `CASE` or `WHERE`; only matching rows are sent.
- Save every step to a table keyed by row, input hash and a version string, and
  insert only missing rows, so reruns do not pay again.

The [chain AI steps cookbook](cookbooks/chain-ai-steps.md) has a complete,
tested pipeline.

## Machine-readable documentation

The documentation site publishes two plain-text files for LLMs and agents,
regenerated on every docs build:

- [`llms.txt`](https://leonardovida.github.io/duckdb-ai/llms.txt): an index of
  every page with a one-line description.
- [`llms-full.txt`](https://leonardovida.github.io/duckdb-ai/llms-full.txt):
  the full Markdown text of every page in one file.

Fetch `llms-full.txt` when you need the complete function reference in context.
It documents the source on `main`, so confirm the installed version as shown
above before relying on a recently added function.

## Verify source changes

Read the repository's [AGENTS.md](https://github.com/leonardovida/duckdb-ai/blob/main/AGENTS.md) and
[CONTRIBUTING.md](https://github.com/leonardovida/duckdb-ai/blob/main/CONTRIBUTING.md) before editing. Keep tests deterministic:

```sh
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

`test/smoke/mock_provider_smoke.py` checks request and response shapes against
local HTTP mocks. It does not prove that a live provider accepts the request or
returns every possible response.

For docs changes, run `npm run typecheck` and `npm run build` from `website/`.
Report the version or commit tested and distinguish local mock checks, live
checks, hosted CI, and published artifacts. Do not describe an unmerged change
as available in community installs.
