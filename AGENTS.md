# Repository Instructions

This repository contains the `duckdb_ai` DuckDB extension. Keep changes scoped,
source-backed, and validated against the local DuckDB extension tooling.

## Project Layout

- `src/` contains the C++ extension implementation and public headers.
- `test/sql/duckdb_ai.test` contains deterministic SQLLogic coverage.
- `test/smoke/mock_provider_smoke.py` covers mock HTTP provider behavior without
  external network calls.
- `docs/` is the Docusaurus docs source.
- `website/` contains the Docusaurus site configuration and frontend.
- `duckdb/` and `extension-ci-tools/` are submodules/vendor tooling; avoid
  editing them unless the task explicitly requires it.
- `local-docs/` is an ignored local research corpus and should stay out of git.

## Build And Test

Use the repo Makefile from the root:

```sh
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

The formatter gate uses DuckDB's formatter. On this machine, the formatter tools
are available in `/tmp/duckdb_ai_format_venv/bin`:

```sh
PATH=/tmp/duckdb_ai_format_venv/bin:$PATH GEN=ninja make format-check
PATH=/tmp/duckdb_ai_format_venv/bin:$PATH GEN=ninja make format-fix
```

For docs changes, also run from `website/`:

```sh
npm run typecheck
npm run build
```

## Implementation Guidance

- Prefer DuckDB extension patterns already used in `src/duckdb_ai_extension.cpp`
  and `src/duckdb_ai_provider.cpp`.
- Keep SQL function names, argument order, named options, table result schemas,
  and usage-log fields stable unless the task explicitly changes the public API.
- Keep provider credentials out of SQL arguments. Use environment variables or
  `TYPE duckdb_ai` DuckDB secrets.
- Keep deterministic tests free of live network calls. Use
  `ai_request_json(...)` and mock-provider tests for request-shape and provider
  behavior.
- Only run live Ollama or remote-provider smoke tests when the task explicitly
  asks for them or provides credentials/context.
- If Linux container validation is needed on this machine, prefer a copy under
  `/Users/leov/Documents/...` and set `CMAKE_BUILD_PARALLEL_LEVEL=1` to avoid
  local Docker/Colima memory pressure.

## Documentation Guidance

- Document public SQL functions in `docs/functions.md` using DuckDB-style
  reference sections: overview table, description, example, and result shape.
- Keep `README.md` as the high-level usage entry point and link to deeper docs
  instead of duplicating the full function reference.
- When adding a Docusaurus page, wire it through `website/sidebars.ts` and run
  the website typecheck/build.
