---
sidebar_position: 3
---

# Provider guides

This page gives one simple end-to-end example for each supported provider.
Examples assume the extension is installed and loaded:

```sql
INSTALL ai FROM community;
LOAD ai;
```

(Or build from source with `GEN=ninja make release` and run
`./build/release/duckdb`.)

Each guide uses a DuckDB secret for provider, model, and base URL settings. The
API key is read from the matching environment variable when the provider call
runs. If you prefer a fully DuckDB-managed secret, add `API_KEY '...'` to the
`CREATE SECRET` statement.

After any provider call, inspect the local usage buffer:

```sql
SELECT function_name, provider, protocol, model, http_status, elapsed_ms
FROM ai_usage()
ORDER BY event_id DESC
LIMIT 1;
```

## Provider matrix

| Provider | Protocol | Default model | Credentials | Base URL behavior |
| --- | --- | --- | --- | --- |
| `ollama` | Ollama chat and embeddings | `llama3.2`; embeddings use `nomic-embed-text` | Optional `OLLAMA_API_KEY` | Defaults to `http://localhost:11434`; `OLLAMA_HOST` overrides it. |
| `openai` | OpenAI-compatible chat and embeddings | `gpt-4o-mini`; embeddings use `text-embedding-3-small` | `OPENAI_API_KEY` | Defaults to `https://api.openai.com/v1`. |
| `azure` | OpenAI-compatible chat and embeddings | `gpt-4o`; embeddings use `text-embedding-3-small` | `AZURE_OPENAI_API_KEY` | Appends `/openai/v1` to `AZURE_OPENAI_BASE_URL`, `AZURE_OPENAI_ENDPOINT`, or secret `BASE_URL` when needed. |
| `anthropic` / `claude` | Anthropic Messages | `claude-haiku-4-5` | `ANTHROPIC_API_KEY` or `CLAUDE_API_KEY` | Defaults to `https://api.anthropic.com/v1`. |
| `bedrock` | OpenAI-compatible chat | `openai.gpt-oss-120b-1:0` | `AWS_BEDROCK_API_KEY`, `AWS_BEARER_TOKEN_BEDROCK`, or `BEDROCK_API_KEY` | Set `AWS_REGION`, `AWS_BEDROCK_REGION`, `AWS_BEDROCK_BASE_URL`, or secret `BASE_URL`. |
| `cerebras` | OpenAI-compatible chat | `gpt-oss-120b` | `CEREBRAS_API_KEY` | Defaults to `https://api.cerebras.ai/v1`. |
| `cloudflare` / `workers_ai` | OpenAI-compatible chat and embeddings | `@cf/zai-org/glm-4.7-flash`; embeddings use `@cf/baai/bge-base-en-v1.5` | `CLOUDFLARE_API_KEY`, `CLOUDFLARE_API_TOKEN`, or `CLOUDFLARE_AUTH_TOKEN` | Derives the endpoint from `CLOUDFLARE_ACCOUNT_ID`, or accepts `CLOUDFLARE_WORKERS_AI_BASE_URL`, `CLOUDFLARE_AI_BASE_URL`, or secret `BASE_URL`. |
| `cohere` | OpenAI-compatible chat and embeddings | `command-a-plus-05-2026`; embeddings use `embed-v4.0` | `COHERE_API_KEY` | Defaults to `https://api.cohere.ai/compatibility/v1`. |
| `dashscope` / `qwen` | OpenAI-compatible chat and embeddings | `qwen-plus`; embeddings use `text-embedding-v4` | `DASHSCOPE_API_KEY`, `ALIBABA_API_KEY`, or `QWEN_API_KEY` | Defaults to `https://dashscope-intl.aliyuncs.com/compatible-mode/v1`; override with a workspace-specific base URL when needed. |
| `deepinfra` | OpenAI-compatible chat and embeddings | `meta-llama/Meta-Llama-3.1-8B-Instruct-Turbo`; embeddings use `BAAI/bge-large-en-v1.5` | `DEEPINFRA_API_KEY` | Defaults to `https://api.deepinfra.com/v1/openai`. |
| `fireworks` | OpenAI-compatible chat and embeddings | `accounts/fireworks/models/llama-v3p1-8b-instruct`; embeddings use `nomic-ai/nomic-embed-text-v1.5` | `FIREWORKS_API_KEY` | Defaults to `https://api.fireworks.ai/inference/v1`. |
| `gemini` / `gcp` / `google` | OpenAI-compatible chat and embeddings | `gemini-3.5-flash`; embeddings use `gemini-embedding-001` | `GEMINI_API_KEY` | Defaults to Google's OpenAI-compatible endpoint. |
| `github` / `github_models` | OpenAI-compatible chat and embeddings | `openai/gpt-4o`; embeddings use `openai/text-embedding-3-small` | `GITHUB_TOKEN`, `GITHUB_MODELS_TOKEN`, or `GITHUB_API_KEY` | Defaults to `https://models.github.ai/inference`. |
| `groq` | OpenAI-compatible chat | `openai/gpt-oss-20b` | `GROQ_API_KEY` | Defaults to `https://api.groq.com/openai/v1`. |
| `huggingface` / `hf` | OpenAI-compatible chat | `openai/gpt-oss-120b` | `HF_TOKEN`, `HUGGINGFACE_API_KEY`, or `HUGGING_FACE_HUB_TOKEN` | Defaults to `https://router.huggingface.co/v1`. |
| `hunyuan` / `tencent_hunyuan` | OpenAI-compatible chat through Tencent TokenHub | `hy3` | `HUNYUAN_API_KEY`, `TOKENHUB_API_KEY`, or `TENCENT_TOKENHUB_API_KEY` | Defaults to `https://tokenhub.tencentmaas.com/v1`; `TOKENHUB_BASE_URL` selects another TokenHub region. |
| `minimax` | OpenAI-compatible chat | `MiniMax-M2.7` | `MINIMAX_API_KEY` or `MINI_MAX_API_KEY` | Defaults to `https://api.minimax.io/v1`. |
| `mistral` | OpenAI-compatible chat and embeddings | `mistral-small-latest`; embeddings use `mistral-embed` | `MISTRAL_API_KEY` | Defaults to `https://api.mistral.ai/v1`. |
| `moonshot` / `kimi` | OpenAI-compatible chat | `kimi-k2.7-code` | `MOONSHOT_API_KEY` or `KIMI_API_KEY` | Defaults to `https://api.moonshot.ai/v1`. |
| `nebius` / `nebius_token_factory` | OpenAI-compatible chat | `meta-llama/Meta-Llama-3.1-70B-Instruct` | `NEBIUS_API_KEY` or `TOKEN_FACTORY_API_KEY` | Defaults to `https://api.tokenfactory.nebius.com/v1`. |
| `nvidia` / `nvidia_nim` | OpenAI-compatible chat | `meta/llama-3.3-70b-instruct` | `NVIDIA_API_KEY` | Defaults to `https://integrate.api.nvidia.com/v1`. |
| `zai` / `zhipu` | OpenAI-compatible chat and embeddings | `glm-4.7-flash`; embeddings use `embedding-3` | `ZAI_API_KEY` | Defaults to `https://api.z.ai/api/paas/v4`. |
| `deepseek` | OpenAI-compatible chat | `deepseek-v4-flash` | `DEEPSEEK_API_KEY` | Defaults to `https://api.deepseek.com`. |
| `openrouter` | OpenAI-compatible chat and embeddings | `openai/gpt-4o-mini`; embeddings use `openai/text-embedding-3-small` | `OPENROUTER_API_KEY` | Defaults to `https://openrouter.ai/api/v1`. |
| `databricks` | OpenAI-compatible chat | `databricks-gpt-oss-120b` | `DATABRICKS_TOKEN` | Derives `/serving-endpoints` from `DATABRICKS_HOST`, or accepts full Model Serving, AI Gateway, or chat-completions URLs. |
| `snowflake` | OpenAI-compatible chat | `claude-sonnet-4-5` | `SNOWFLAKE_PAT` or `SNOWFLAKE_TOKEN` | Derives `/api/v2/cortex/v1` from Snowflake account URL, host, or account id. |
| `perplexity` | OpenAI-compatible chat | `sonar` | `PERPLEXITY_API_KEY` | Defaults to `https://api.perplexity.ai`. |
| `poe` | OpenAI-compatible chat | `GPT-5.4` | `POE_API_KEY` | Defaults to `https://api.poe.com/v1`. |
| `qianfan` / `ernie` | OpenAI-compatible chat | `ernie-4.5-turbo-128k` | `QIANFAN_API_KEY`, `BAIDU_QIANFAN_API_KEY`, or `BAIDU_API_KEY` | Defaults to `https://qianfan.baidubce.com/v2`. |
| `sambanova` | OpenAI-compatible chat | `Meta-Llama-3.3-70B-Instruct` | `SAMBANOVA_API_KEY` | Defaults to `https://api.sambanova.ai/v1`. |
| `siliconflow` | OpenAI-compatible chat | `Qwen/Qwen2.5-72B-Instruct` | `SILICONFLOW_API_KEY` | Defaults to `https://api.siliconflow.com/v1`. |
| `together` | OpenAI-compatible chat and embeddings | `meta-llama/Llama-3.3-70B-Instruct-Turbo`; embeddings use `BAAI/bge-base-en-v1.5` | `TOGETHER_API_KEY` | Defaults to `https://api.together.xyz/v1`. |
| `stepfun` / `step` | OpenAI-compatible chat | `step-3.7-flash` | `STEPFUN_API_KEY` or `STEP_API_KEY` | Defaults to `https://api.stepfun.ai/v1`. |
| `vercel` / `vercel_ai_gateway` | OpenAI-compatible chat and embeddings | `openai/gpt-4o-mini`; embeddings use `openai/text-embedding-3-small` | `AI_GATEWAY_API_KEY`, `VERCEL_AI_GATEWAY_API_KEY`, or `VERCEL_OIDC_TOKEN` | Defaults to `https://ai-gateway.vercel.sh/v1`. |
| `vertex` / `google_vertex` | OpenAI-compatible chat | `google/gemini-2.5-flash` | `VERTEX_AI_ACCESS_TOKEN`, `GOOGLE_CLOUD_ACCESS_TOKEN`, or `VERTEX_API_KEY` | Derives the endpoint from `GOOGLE_CLOUD_PROJECT`, or accepts `VERTEX_AI_BASE_URL`, `GOOGLE_VERTEX_BASE_URL`, or secret `BASE_URL`. |
| `volcengine` / `doubao` | OpenAI-compatible chat | `doubao-seed-2-1-pro-260628` | `VOLCENGINE_API_KEY`, `ARK_API_KEY`, or `DOUBAO_API_KEY` | Defaults to `https://ark.cn-beijing.volces.com/api/v3`. |
| `xai` / `grok` | OpenAI-compatible chat | `grok-4.5` | `XAI_API_KEY` | Defaults to `https://api.x.ai/v1`. |
| `openai_privacy_filter` | Dedicated redaction endpoint | `openai/privacy-filter` | Optional `OPENAI_PRIVACY_FILTER_API_KEY` | Defaults to `http://localhost:8080` and calls `POST /redact`. |
| `openai_compatible` / `local` | OpenAI-compatible chat and embeddings | `gpt-4o-mini`; embeddings use `text-embedding-3-small` | Optional `OPENAI_COMPATIBLE_API_KEY` | Requires `BASE_URL` or `OPENAI_COMPATIBLE_BASE_URL`. |
| `llamacpp` / `llama.cpp` | OpenAI-compatible chat and embeddings | `default` (llama-server answers with its loaded model) | Optional `LLAMACPP_API_KEY` (`llama-server --api-key`) | Defaults to `http://localhost:8080/v1`. Embeddings need `llama-server --embeddings`. |

For guidance on choosing providers, credentials, logging, cost, throughput, and
PII workflows, see [Best practices](best-practices.md).

## Ollama

Use Ollama for local models without a hosted API key.

```sh
ollama serve
ollama pull llama3.2
ollama pull nomic-embed-text
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET ollama_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'ollama',
    MODEL 'llama3.2'
);

SELECT ai_complete(
    'Write one sentence about why DuckDB is useful for analytics.',
    secret := 'ollama_ai',
    timeout_seconds := 120
) AS answer;

SELECT ai_embed(
    'DuckDB local analytics',
    secret := 'ollama_ai',
    model := 'nomic-embed-text'
)[1] AS first_embedding_value;
```

If Ollama runs on a non-default host, set `OLLAMA_HOST` before starting DuckDB or
add `BASE_URL 'http://host:11434'` to the secret.

## OpenAI

```sh
export OPENAI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-4o-mini'
);

SELECT ai_complete(
    'Summarize DuckDB in one short sentence.',
    secret := 'openai_ai'
) AS answer;

SELECT ai_complete_json(
    'Return a JSON object with keys "name" and "kind" for DuckDB.',
    secret := 'openai_ai',
    response_schema := '{
      "type": "object",
      "properties": {
        "name": {"type": "string"},
        "kind": {"type": "string"}
      },
      "required": ["name", "kind"]
    }'
) AS profile_json;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'openai_ai',
    model := 'text-embedding-3-small'
)[1] AS first_embedding_value;
```

## Azure OpenAI

Use your Azure OpenAI resource URL and deployment name. The extension appends
`/openai/v1` to the base URL when needed.

```sh
export AZURE_OPENAI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET azure_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'azure',
    BASE_URL 'https://my-resource.openai.azure.com',
    MODEL 'gpt-4o'
);

SELECT ai_complete(
    'Explain DuckDB to a data analyst in one sentence.',
    secret := 'azure_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'azure_ai',
    model := 'text-embedding-3-small'
)[1] AS first_embedding_value;
```

If your Azure deployment names differ from the model names above, use the
deployment name in `MODEL` or `model := ...`.

## Claude / Anthropic

```sh
export ANTHROPIC_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET claude_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'anthropic',
    MODEL 'claude-haiku-4-5'
);

SELECT ai_complete(
    'Explain DuckDB in one concise paragraph.',
    secret := 'claude_ai',
    max_tokens := 160
) AS answer;

SELECT ai_summarize(
    'DuckDB is an in-process analytical database built for fast local queries.',
    secret := 'claude_ai',
    max_tokens := 80
) AS summary;
```

Claude is configured for completion calls. Embeddings are not configured for this
provider.

## Gemini

Gemini uses Google's OpenAI-compatible endpoint.

```sh
export GEMINI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET gemini_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'gemini',
    MODEL 'gemini-3.5-flash'
);

SELECT ai_complete(
    'Write one sentence about DuckDB extensions.',
    secret := 'gemini_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'gemini_ai',
    model := 'gemini-embedding-001'
)[1] AS first_embedding_value;
```

Provider aliases `gcp`, `google`, and `google_gemini` also resolve to Gemini.

## Mistral

```sh
export MISTRAL_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET mistral_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'mistral',
    MODEL 'mistral-small-latest'
);

SELECT ai_complete(
    'Give one practical use case for DuckDB and AI functions.',
    secret := 'mistral_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'mistral_ai',
    model := 'mistral-embed'
)[1] AS first_embedding_value;
```

## Z.ai

```sh
export ZAI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET zai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'zai',
    MODEL 'glm-4.7-flash'
);

SELECT ai_complete(
    'Summarize DuckDB in one sentence.',
    secret := 'zai_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'zai_ai',
    model := 'embedding-3'
)[1] AS first_embedding_value;
```

Provider aliases `zai` and `zhipu` resolve to the same provider.

## DeepSeek

```sh
export DEEPSEEK_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET deepseek_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'deepseek',
    MODEL 'deepseek-v4-flash'
);

SELECT ai_complete(
    'Write one sentence about using SQL with language models.',
    secret := 'deepseek_ai'
) AS answer;

SELECT ai_classify(
    'Customer says the invoice is overdue.',
    'billing, support, sales, other',
    secret := 'deepseek_ai'
) AS category;
```

DeepSeek is configured for completion calls. Embeddings are not configured for
this provider.

## OpenRouter

```sh
export OPENROUTER_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET openrouter_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openrouter',
    MODEL 'openai/gpt-4o-mini'
);

SELECT ai_complete(
    'Explain DuckDB extensions in one sentence.',
    secret := 'openrouter_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'openrouter_ai',
    model := 'openai/text-embedding-3-small'
)[1] AS first_embedding_value;
```

OpenRouter model names include the upstream provider prefix, for example
`openai/gpt-4o-mini`.

Set `OPENROUTER_HTTP_REFERER` and `OPENROUTER_X_TITLE` to send OpenRouter's
optional attribution headers.

## Groq

```sh
export GROQ_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET groq_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'groq',
    MODEL 'openai/gpt-oss-20b'
);

SELECT ai_complete(
    'Explain vectorized SQL execution in one sentence.',
    secret := 'groq_ai'
) AS answer;
```

Groq is configured for completion calls. Embeddings are not configured for this
provider.

## Together AI

```sh
export TOGETHER_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET together_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'together',
    MODEL 'meta-llama/Llama-3.3-70B-Instruct-Turbo'
);

SELECT ai_complete(
    'Write one sentence about local-first analytics.',
    secret := 'together_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'together_ai',
    model := 'BAAI/bge-base-en-v1.5'
)[1] AS first_embedding_value;
```

## Fireworks AI

```sh
export FIREWORKS_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET fireworks_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'fireworks',
    MODEL 'accounts/fireworks/models/llama-v3p1-8b-instruct'
);

SELECT ai_complete(
    'Give one reason to enrich data inside SQL.',
    secret := 'fireworks_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'fireworks_ai',
    model := 'nomic-ai/nomic-embed-text-v1.5'
)[1] AS first_embedding_value;
```

## DeepInfra

```sh
export DEEPINFRA_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET deepinfra_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'deepinfra',
    MODEL 'meta-llama/Meta-Llama-3.1-8B-Instruct-Turbo'
);

SELECT ai_complete(
    'Summarize why open-weight hosted inference is useful.',
    secret := 'deepinfra_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'deepinfra_ai',
    model := 'BAAI/bge-large-en-v1.5'
)[1] AS first_embedding_value;
```

## Cerebras

```sh
export CEREBRAS_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET cerebras_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'cerebras',
    MODEL 'gpt-oss-120b'
);

SELECT ai_complete(
    'Explain fast inference for analytical workflows in one sentence.',
    secret := 'cerebras_ai'
) AS answer;
```

Cerebras is configured for completion calls. Embeddings are not configured for
this provider.

## Cohere

```sh
export COHERE_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET cohere_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'cohere',
    MODEL 'command-a-plus-05-2026'
);

SELECT ai_complete(
    'Classify this support note as billing, support, sales, or other.',
    secret := 'cohere_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'cohere_ai',
    model := 'embed-v4.0'
)[1] AS first_embedding_value;
```

## Cloudflare Workers AI

Cloudflare Workers AI's OpenAI-compatible endpoint is account scoped. Set
`CLOUDFLARE_ACCOUNT_ID`, or provide a full `BASE_URL`.

```sh
export CLOUDFLARE_API_KEY='...'
export CLOUDFLARE_ACCOUNT_ID='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET cloudflare_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'workers_ai',
    MODEL '@cf/zai-org/glm-4.7-flash'
);

SELECT ai_complete(
    'Explain edge-hosted inference in one sentence.',
    secret := 'cloudflare_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'cloudflare_ai',
    model := '@cf/baai/bge-base-en-v1.5'
)[1] AS first_embedding_value;
```

Aliases `cloudflare`, `workers_ai`, `cloudflare_workers_ai`, and
`cloudflare_ai` resolve to the same provider.

## Alibaba Cloud Model Studio / DashScope

DashScope exposes Qwen models through an OpenAI-compatible endpoint. The default
base URL uses the existing international compatible-mode endpoint; use a secret
`BASE_URL`, `DASHSCOPE_BASE_URL`, or `ALIBABA_MODEL_STUDIO_BASE_URL` for a
workspace-specific endpoint.

```sh
export DASHSCOPE_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET dashscope_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'qwen',
    MODEL 'qwen-plus'
);

SELECT ai_complete(
    'Write one sentence about analytics agents.',
    secret := 'dashscope_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'dashscope_ai',
    model := 'text-embedding-v4'
)[1] AS first_embedding_value;
```

Aliases `dashscope`, `qwen`, `alibaba`, `alibaba_model_studio`, and
`model_studio` resolve to the same provider.

## Nebius Token Factory

```sh
export NEBIUS_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET nebius_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'nebius_token_factory',
    MODEL 'meta-llama/Meta-Llama-3.1-70B-Instruct'
);

SELECT ai_complete(
    'Summarize why managed open-weight inference matters.',
    secret := 'nebius_ai'
) AS answer;
```

Nebius is configured for completion calls. Embeddings are not configured for
this provider.

## SambaNova Cloud

```sh
export SAMBANOVA_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET sambanova_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'sambanova',
    MODEL 'Meta-Llama-3.3-70B-Instruct'
);

SELECT ai_complete(
    'Explain fast open-weight inference in one sentence.',
    secret := 'sambanova_ai'
) AS answer;
```

Aliases `sambanova`, `sambanova_ai`, `samba_nova`, and `sambacloud` resolve to
the same provider. Embeddings are not configured for this provider.

## SiliconFlow

```sh
export SILICONFLOW_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET siliconflow_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'siliconflow',
    MODEL 'Qwen/Qwen2.5-72B-Instruct'
);

SELECT ai_complete(
    'Write one sentence about open model hosting.',
    secret := 'siliconflow_ai'
) AS answer;
```

Aliases `siliconflow` and `silicon_flow` resolve to the same provider.
Embeddings are not configured for this provider.

## Vercel AI Gateway

```sh
export AI_GATEWAY_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET vercel_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'vercel_ai_gateway',
    MODEL 'openai/gpt-4o-mini'
);

SELECT ai_complete(
    'Explain model gateway routing in one sentence.',
    secret := 'vercel_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'vercel_ai',
    model := 'openai/text-embedding-3-small'
)[1] AS first_embedding_value;
```

Aliases `vercel`, `vercel_ai_gateway`, `vercel_gateway`, and `ai_gateway`
resolve to the same provider.

## Moonshot AI / Kimi

```sh
export MOONSHOT_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET kimi_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'kimi',
    MODEL 'kimi-k2.7-code'
);

SELECT ai_complete(
    'Explain code-oriented model inference in one sentence.',
    secret := 'kimi_ai'
) AS answer;
```

Aliases `moonshot`, `kimi`, `moonshot_ai`, and `kimi_api` resolve to the same
provider. Embeddings are not configured for this provider.

## Baidu Qianfan / ERNIE

```sh
export QIANFAN_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET qianfan_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'ernie',
    MODEL 'ernie-4.5-turbo-128k'
);

SELECT ai_complete(
    'Write one sentence about enterprise AI platforms.',
    secret := 'qianfan_ai'
) AS answer;
```

Aliases `qianfan`, `baidu`, `baidu_qianfan`, `ernie`, and `wenxin` resolve to
the same provider. Embeddings are not configured for this provider.

## Tencent TokenHub / Hunyuan

```sh
export TOKENHUB_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET hunyuan_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'tencent_hunyuan',
    MODEL 'hy3'
);

SELECT ai_complete(
    'Summarize governed model APIs in one sentence.',
    secret := 'hunyuan_ai'
) AS answer;
```

Aliases `hunyuan`, `tencent`, and `tencent_hunyuan` resolve to the same
provider. `HUNYUAN_API_KEY` and `TENCENT_HUNYUAN_API_KEY` remain accepted for
existing configurations. Set `TOKENHUB_BASE_URL` for the international or a
custom regional TokenHub endpoint. Embeddings are not configured for this
provider.

## StepFun

```sh
export STEPFUN_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET stepfun_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'step',
    MODEL 'step-3.7-flash'
);

SELECT ai_complete(
    'Write one sentence about fast chat models.',
    secret := 'stepfun_ai'
) AS answer;
```

Aliases `stepfun`, `step`, and `step_fun` resolve to the same provider.
Embeddings are not configured for this provider.

## MiniMax

```sh
export MINIMAX_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET minimax_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'minimax',
    MODEL 'MiniMax-M2.7'
);

SELECT ai_complete(
    'Explain model choice for SQL enrichment in one sentence.',
    secret := 'minimax_ai'
) AS answer;
```

Aliases `minimax` and `mini_max` resolve to the same provider. Embeddings are
not configured for this provider.

## Poe

```sh
export POE_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET poe_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'poe',
    MODEL 'GPT-5.4'
);

SELECT ai_complete(
    'Summarize model routing in one sentence.',
    secret := 'poe_ai'
) AS answer;
```

Aliases `poe` and `poe_api` resolve to the same provider. Embeddings are not
configured for this provider.

## Volcengine Ark / Doubao

```sh
export VOLCENGINE_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET doubao_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'doubao',
    MODEL 'doubao-seed-2-1-pro-260628'
);

SELECT ai_complete(
    'Explain hosted model APIs in one sentence.',
    secret := 'doubao_ai'
) AS answer;
```

Aliases `volcengine`, `volcano_engine`, `volcengine_ark`, `doubao`, and `ark`
resolve to the same provider. Embeddings are not configured for this provider.

## Hugging Face

```sh
export HF_TOKEN='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET huggingface_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'huggingface',
    MODEL 'openai/gpt-oss-120b'
);

SELECT ai_complete(
    'Write one sentence about open-source model routing.',
    secret := 'huggingface_ai'
) AS answer;
```

Aliases `hf`, `hugging_face`, `huggingface_hub`, and `hf_inference` resolve to
`huggingface`. Embeddings are not configured by default; use
`openai_compatible` with an embedding-capable router endpoint if needed.

## GitHub Models

```sh
export GITHUB_TOKEN='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET github_models_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'github_models',
    MODEL 'openai/gpt-4o'
);

SELECT ai_complete(
    'Write one concise test-case title for an AI SQL helper.',
    secret := 'github_models_ai'
) AS answer;
```

Aliases `github`, `github_models`, `github-models`, and `gh_models` resolve to
the same provider.

## xAI / SpaceXAI

```sh
export XAI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET xai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'xai',
    MODEL 'grok-4.5'
);

SELECT ai_complete(
    'Summarize this SQL migration risk in one sentence.',
    secret := 'xai_ai'
) AS answer;
```

Aliases `xai`, `x.ai`, `x-ai`, and `grok` resolve to the same provider.
Embeddings are not configured for this provider.

## Perplexity

```sh
export PERPLEXITY_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET perplexity_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'perplexity',
    MODEL 'sonar'
);

SELECT ai_complete(
    'Give one current consideration for managed model APIs.',
    secret := 'perplexity_ai'
) AS answer;
```

Perplexity is configured for completion calls. Embeddings are not configured for
this provider.

## NVIDIA NIM

```sh
export NVIDIA_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET nvidia_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'nvidia_nim',
    MODEL 'meta/llama-3.3-70b-instruct'
);

SELECT ai_complete(
    'Explain GPU-hosted inference in one sentence.',
    secret := 'nvidia_ai'
) AS answer;
```

Aliases `nvidia`, `nvidia_nim`, and `nim` resolve to the same provider.
Embeddings are not configured for this provider.

## Amazon Bedrock

Bedrock's OpenAI-compatible endpoint is regional. Set either a full base URL or
a region; the extension derives `https://bedrock-mantle.<region>.api.aws/v1`
from the region.

```sh
export AWS_BEARER_TOKEN_BEDROCK='...'
export AWS_REGION='us-east-1'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET bedrock_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'bedrock',
    MODEL 'openai.gpt-oss-120b-1:0'
);

SELECT ai_complete(
    'Summarize why governed enterprise inference matters.',
    secret := 'bedrock_ai'
) AS answer;
```

Aliases `bedrock`, `aws_bedrock`, `amazon_bedrock`, and `bedrock_mantle`
resolve to the same provider. Embeddings are not configured by default.

## Google Vertex AI

Vertex AI's OpenAI-compatible endpoint is project and location scoped. Set a
full base URL, or set `GOOGLE_CLOUD_PROJECT` and optionally
`GOOGLE_CLOUD_LOCATION`.

```sh
export GOOGLE_CLOUD_PROJECT='my-project'
export GOOGLE_CLOUD_LOCATION='global'
export VERTEX_AI_ACCESS_TOKEN="$(gcloud auth print-access-token)"
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET vertex_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'vertex',
    MODEL 'google/gemini-2.5-flash'
);

SELECT ai_complete(
    'Explain BigQuery and DuckDB together in one sentence.',
    secret := 'vertex_ai'
) AS answer;
```

Aliases `vertex`, `google_vertex`, `vertex_ai`, and `gcp_vertex` resolve to the
same provider. Embeddings are not configured for this provider.

## Databricks

Databricks Model Serving exposes chat endpoints through an OpenAI-compatible
API. Use a Databricks personal access token or service-principal token, and set
the model to the serving endpoint name.

```sh
export DATABRICKS_TOKEN='...'
export DATABRICKS_HOST='https://<workspace>.cloud.databricks.com'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET databricks_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'databricks',
    MODEL 'databricks-gpt-oss-120b'
);

SELECT ai_complete(
    'Explain Delta Lake in one sentence.',
    secret := 'databricks_ai'
) AS answer;
```

If `BASE_URL` is omitted, set `DATABRICKS_HOST`; the extension derives
`https://<workspace>/serving-endpoints`. You can also set a secret `BASE_URL`,
`DATABRICKS_BASE_URL`, or per-call `base_url := ...` to use a full
`/serving-endpoints`, `/ai-gateway/mlflow/v1`, or `/chat/completions` endpoint.
Aliases `mosaic`, `mosaic_ai`, and `databricks_ai` resolve to `databricks`.

## Snowflake Cortex REST

Snowflake Cortex REST exposes a Chat Completions API compatible with the OpenAI
request shape. Use a Snowflake Programmatic Access Token, OAuth token, or JWT
with a role that can call Cortex REST.

```sh
export SNOWFLAKE_PAT='...'
export SNOWFLAKE_ACCOUNT_URL='https://<account-identifier>.snowflakecomputing.com'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET snowflake_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'snowflake',
    MODEL 'claude-sonnet-4-5'
);

SELECT ai_complete(
    'Summarize why governed model inference matters.',
    secret := 'snowflake_ai'
) AS answer;
```

If `BASE_URL` is omitted, set `SNOWFLAKE_ACCOUNT_URL`, `SNOWFLAKE_HOST`, or
`SNOWFLAKE_ACCOUNT`; the extension derives
`https://<account>.snowflakecomputing.com/api/v2/cortex/v1`. You can also set a
secret `BASE_URL`, `SNOWFLAKE_BASE_URL`, or per-call `base_url := ...` to use a
full `/api/v2/cortex/v1` or `/chat/completions` endpoint. Snowflake model IDs
include values such as `claude-sonnet-4-5`, `llama4-maverick`, and
`llama3.3-70b`, depending on region and model access.

## OpenAI Privacy Filter

OpenAI Privacy Filter is an open-weight PII detection and masking model. The
extension calls it through a small HTTP service so the model can run either on
the same machine as DuckDB or in your own cloud deployment.

Use local hosting when unredacted PII should not leave the machine running
DuckDB:

```sh
# Host a wrapper around the openai/privacy-filter Python package.
# The wrapper should expose POST /redact.
export OPF_CHECKPOINT="$HOME/.opf/privacy_filter"
./serve-privacy-filter --host 127.0.0.1 --port 8080
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET privacy_filter_local (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai_privacy_filter',
    BASE_URL 'http://localhost:8080'
);

SELECT ai_redact(
    'email alice@example.com token fake-token',
    secret := 'privacy_filter_local'
) AS redacted_text;
```

Use cloud hosting when multiple DuckDB clients should share one managed Privacy
Filter deployment:

```sql
CREATE OR REPLACE SECRET privacy_filter_cloud (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai_privacy_filter',
    BASE_URL 'https://privacy-filter.example.com',
    API_KEY '...'
);

SELECT ai_redact(
    internal_note,
    secret := 'privacy_filter_cloud'
) AS redacted_note
FROM support_tickets;
```

The service contract is intentionally small:

```http
POST /redact
Content-Type: application/json
Authorization: Bearer ...    # optional

{"text":"email alice@example.com","model":"openai/privacy-filter"}
```

The response should include one of `redacted_text`, `masked_text`, `text`, or
`output`:

```json
{"redacted_text":"email [PRIVATE_EMAIL]"}
```

Aliases `privacy_filter`, `pii_filter`, and `opf` resolve to
`openai_privacy_filter`. The default base URL is `http://localhost:8080`; set
`OPENAI_PRIVACY_FILTER_BASE_URL`, `DUCKDB_AI_BASE_URL`, a secret `BASE_URL`, or a
per-call `base_url := ...` to point at a different local or cloud service. For
cloud auth, use a secret `API_KEY`, `OPENAI_PRIVACY_FILTER_API_KEY`, or
`DUCKDB_AI_API_KEY`.

## OpenAI-compatible / Local gateway

Use this path for vLLM, LM Studio, LiteLLM, Ollama's `/v1` endpoint, or any
gateway that exposes an OpenAI-compatible chat API.

For local Ollama's OpenAI-compatible endpoint:

```sh
ollama serve
ollama pull llama3.2
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET local_openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'local',
    BASE_URL 'http://localhost:11434/v1',
    MODEL 'llama3.2'
);

SELECT ai_complete(
    'Write one sentence about local AI models in DuckDB.',
    secret := 'local_openai_ai'
) AS answer;
```

For a hosted OpenAI-compatible gateway, add an API key:

```sh
export OPENAI_COMPATIBLE_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET gateway_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai_compatible',
    BASE_URL 'https://gateway.example/v1',
    MODEL 'provider/model-name'
);

SELECT ai_complete(
    'Write one sentence about DuckDB.',
    secret := 'gateway_ai'
) AS answer;
```

Aliases `local`, `openai_compatible`, `openai-compatible`, `local_openai`,
`local-models`, and `local_models` all use the OpenAI-compatible protocol.

## llama.cpp

Use the `llamacpp` provider (aliases `llama.cpp`, `llama-cpp`, `llama_cpp`,
`llama-server`, `llama_server`) for a local
[llama.cpp](https://github.com/ggml-org/llama.cpp) `llama-server`. It uses the
OpenAI-compatible protocol and defaults to `http://localhost:8080/v1`, so no
base URL setup is needed for a default server:

```sh
llama-server -m model.gguf --embeddings
./build/release/duckdb
```

```sql
LOAD ai;
SET duckdb_ai_provider = 'llama.cpp';

SELECT ai_complete('Write one sentence about DuckDB.') AS answer;
SELECT ai_embed('DuckDB');
```

The request `model` defaults to `default`; `llama-server` ignores it and
answers with its loaded model, so no model configuration is needed either.
Set `LLAMACPP_BASE_URL` for a non-default host or port, and `LLAMACPP_API_KEY`
when the server runs with `--api-key`. Embedding calls require starting
`llama-server` with `--embeddings`.

`response_schema := ...` uses llama.cpp's direct `response_format.schema`
request shape so `llama-server` can enforce the supplied JSON Schema.

For throughput, start `llama-server` with parallel slots (for example
`--parallel 4`) and match `max_concurrent_requests` so DuckDB keeps every slot
busy; llama.cpp reuses cached prompt prefixes automatically, so shared system
prompts stay cheap across rows.
