---
sidebar_position: 1
slug: /
---

# duckdb_ai documentation

`duckdb_ai` is a DuckDB extension that lets SQL call AI model providers. It
currently focuses on deterministic request shaping, provider metadata,
completion and embedding calls, structured JSON validation, generated read-only
SQL, usage logging, and local validation evidence.

## Start here

- [SQL function reference](functions.md): every scalar, aggregate, and table
  function exposed by the extension, with examples and result shapes.
- [Provider guides](provider-guides.md): end-to-end examples for Ollama, OpenAI,
  Azure OpenAI, Claude, Gemini, Mistral, Z.ai, DeepSeek, OpenRouter, and local
  OpenAI-compatible gateways.
- [Validation](VALIDATION.md): local build, test, and smoke evidence for the
  pinned DuckDB extension build.
- [Smoke testing](SMOKE_TESTING.md): deterministic mock-provider checks plus
  optional live Ollama and remote provider commands.
- [Distribution status](DISTRIBUTION.md): current source-first release stance
  and the next publishing gaps.
- [Releasing](RELEASING.md): preview and stable release gates.
- [Updating](UPDATING.md): DuckDB submodule update notes.
- [Work plan](WORKPLAN.md): implemented phases and remaining context.

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
