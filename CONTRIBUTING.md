# Contributing

Thanks for improving the `ai` extension. This repository follows DuckDB extension
tooling, keeps public SQL behavior stable, and makes provider interactions
deterministic in the default test suite.

## Before You Start

- Keep changes scoped to the issue you are solving.
- Avoid editing `duckdb/` and `extension-ci-tools/` unless the change explicitly
  needs upstream extension infrastructure updates.
- Keep `local-docs/` and other local corpora out of git.
- Do not commit provider credentials, API keys, tokens, or generated secrets.
- Prefer environment variables or `TYPE duckdb_ai` secrets for provider
  configuration.
- Do not add live-provider network calls to deterministic tests.

## Project Layout

- `src/` contains the C++ extension, SQL function registration, and provider
  request handling.
- `test/sql/duckdb_ai.test` contains SQLLogicTest coverage for deterministic
  function behavior.
- `test/smoke/mock_provider_smoke.py` runs the Python mock-provider smoke test.
- `docs/` contains source documentation for the extension.
- `website/` contains the Docusaurus documentation site.
- `duckdb/` and `extension-ci-tools/` are vendored extension build
  dependencies.

## Local Validation

Run extension checks from the repository root:

```bash
PATH=/tmp/duckdb_ai_format_venv/bin:$PATH GEN=ninja make format-check
GEN=ninja make tidy-check-ai
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
python3 test/smoke/accuracy_gate_smoke.py
python3 test/smoke/reliability_smoke.py --library build/release/src/libduckdb.so
```

Run documentation site checks from `website/`:

```bash
npm run typecheck
npm run build
```

If a command cannot be run in your environment, note the reason in the pull
request.

## SQL API Changes

- Preserve public function names, named options, result schemas, and defaults
  unless the change intentionally updates the SQL API.
- Add or update SQLLogicTest coverage when function behavior, option parsing, or
  result shape changes.
- Prefer `ai_request_json` and deterministic mock-provider paths for request
  shape coverage.
- Keep live Ollama or remote-provider checks optional unless the task explicitly
  requires them.
- Update `README.md` for high-level user-facing changes.
- Update `docs/functions.md` for every public SQL function that is added,
  removed, or changed.

## Documentation Changes

- Use DuckDB-style function documentation: overview table, signature,
  description, example, result shape, and option details where relevant.
- Keep `README.md` focused on setup and common examples.
- Put complete function reference material in `docs/functions.md`.
- Wire new documentation pages through `website/sidebars.ts`.
- Run the Docusaurus typecheck and build when changing the docs site.

## Pull Request Checklist

- The change is scoped and avoids unrelated formatting churn.
- Relevant tests, builds, or docs checks pass locally.
- Public SQL API changes are documented.
- No credentials, generated secrets, or local-only corpora are committed.
- Any skipped validation is called out with a concrete reason.

## Owned-code sanitizer checks (Linux/GCC)

After building the normal release core, instrument only the loadable extension:

```sh
cmake -G Ninja -S duckdb -B build/release -DAI_ENABLE_SANITIZERS=ON
cmake --build build/release --target ai_loadable_extension -j2
python3 test/smoke/sanitizer_smoke.py --duckdb /path/to/vanilla-duckdb-v1.5.5 --extension build/release/extension/ai/ai.duckdb_extension
```

The host must use DuckDB v1.5.5. Static DuckDB linkage is required for C++ RTTI;
the two project-owned sources are instrumented while core/vendor code remains
uninstrumented. Restore `AI_ENABLE_SANITIZERS=OFF` and rebuild before publishing
normal extension artifacts. CI performs this after the regular checks and also
runs macOS/Windows builds through the upstream distribution tooling.
