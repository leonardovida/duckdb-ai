---
sidebar_position: 1
slug: /
---

# duckdb-ai documentation

duckdb-ai is a DuckDB extension that lets SQL call AI model providers. It
currently focuses on deterministic request shaping, provider metadata,
completion and embedding calls, structured JSON validation, generated read-only
SQL, usage logging, and local validation evidence.

## Start here

- [SQL function reference](functions.md): every scalar, aggregate, and table
  function exposed by the extension, with examples and result shapes.
- [Cookbooks](cookbooks/index.md): practical workflows over local tables,
  including text enrichment, similarity, structured records, and SQL generation.
- [Provider guides](provider-guides.md): end-to-end examples for Ollama, OpenAI,
  Azure OpenAI, Claude, Gemini, Mistral, Z.ai, DeepSeek, OpenRouter,
  Databricks, Snowflake Cortex REST, OpenAI Privacy Filter, and local
  OpenAI-compatible gateways.
- [Best practices](best-practices.md): provider selection, secrets, model
  defaults, structured output, SQL safety, redaction, logging, throughput, cost,
  and release validation guidance.
- [Runtime behavior](runtime-behavior.md): function stability, per-database
  runtime state, response caching, provider concurrency, cancellation, retries,
  egress allowlisting, and JSON parsing.
- [Security and data flow](security-data-flow.md): egress controls, per-function
  data flow, logging defaults, proxy/TLS notes, and vulnerability reporting.
- [Validation](VALIDATION.md): local build, test, and smoke evidence for the
  pinned DuckDB extension build.
- [Smoke testing](SMOKE_TESTING.md): deterministic mock-provider checks plus
  optional live Ollama and remote provider commands.
- [Distribution status](DISTRIBUTION.md): current source-build status,
  community-preview target, and the next publishing gaps.
- [Releasing](RELEASING.md): preview and stable release gates.
- [Updating](UPDATING.md): DuckDB submodule update notes.

## Local site development

The Docusaurus site lives in `website/`, while this `docs/` directory is the
published docs source.

```sh
cd website
npm install
npm run start
npm run build
```

GitHub Pages deployment is handled by `.github/workflows/deploy-docs.yml` after
changes merge to `main`.
