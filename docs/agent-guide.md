---
title: Use DuckDB AI from coding agents
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
in `FROM`; `ai_extract_record()` is a scalar returning a `STRUCT` per input row.

## Configure the intended provider

Read [the provider matrix](provider-guides.md#provider-matrix), then choose the
provider, an available model, and any required base URL. Use an embedding model
for `ai_embed` and `ai_similarity`; `ai_rerank` uses a completion model.

Keep keys in a process environment or a secret manager. If a named DuckDB secret
is used, pass `secret := 'name'` explicitly to make the intended configuration
clear. Do not put credentials into prompts, `request_options`, source files, or
test output. The hosted example in the [repository README](https://github.com/leonardovida/duckdb-ai#use-openai-or-another-hosted-model) uses
`OPENAI_API_KEY` from the process environment.

Use explicit per-call `provider` and `model` options for isolated examples.
For repeated queries, configure session defaults or a named secret. Consult
[configuration precedence](best-practices.md) before combining them.

## Preview before making a model call

This query constructs JSON locally and needs no API key:

```sql
SELECT ai_completion_request_json(
    'Reply OK',
    provider := 'openai',
    model := 'gpt-4o-mini',
    max_tokens := 64
);
```

Check the model, messages, and token-limit field. `ai_embedding_request_json`
provides the equivalent preview for embeddings. A correct preview proves request
construction, not authentication, model availability, or successful inference.
Provider-specific discovery helpers, where available in the installed version,
may fetch a remote catalog. Treat that as a network call and verify it against
the provider documentation before using it in an agent workflow.

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

## Verify source changes

Read the repository's [AGENTS.md](https://github.com/leonardovida/duckdb-ai/blob/main/AGENTS.md) and
[CONTRIBUTING.md](https://github.com/leonardovida/duckdb-ai/blob/main/CONTRIBUTING.md) before editing. Keep tests deterministic:

```sh
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

The repository's smoke tests use deterministic local HTTP fixtures for selected
provider wire contracts. Run the smoke scripts present in your checkout and
inspect their manifests before relying on them as coverage claims. These mocks
exercise selected wire contracts, not all possible responses from a service.

For docs changes, run `npm run typecheck` and `npm run build` from `website/`.
Report the version or commit tested and distinguish local mock checks, live
checks, hosted CI, and published artifacts. Do not describe an unmerged change
as available in community installs.
