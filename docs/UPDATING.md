# Updating DuckDB

`duckdb_ai` builds against DuckDB's extension C++ API, so DuckDB
submodule updates should be treated as compatibility work, not just dependency
bump chores.

## Update checklist

1. Pick the DuckDB release target.
2. Update the `duckdb` submodule to that release tag.
3. Update `extension-ci-tools` to the matching release branch.
4. Update `.github/workflows/MainDistributionPipeline.yml` so
   `duckdb_version`, `ci_tools_version`, and the reusable workflow refs match
   the chosen DuckDB release family.
5. Run:

```sh
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

6. Run at least one live provider smoke before publishing binaries. Local
   Ollama is enough for provider-call mechanics; use one remote provider when
   credential handling or protocol code changed.

## API compatibility checks

If the build fails after a DuckDB bump, inspect the changed API surface in this
order:

- DuckDB release notes: https://github.com/duckdb/duckdb/releases
- DuckDB extension patch history:
  https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions
- Git history for the changed DuckDB header or source file.

Keep fixes targeted to the API drift. Avoid unrelated function-surface changes
in the same update.
