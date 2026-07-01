# Releasing

`duckdb_ai` can publish preview releases before 1.0, but stable
releases need a stronger compatibility and provenance contract. Treat any tag
before `1.0.0` as an explicit preview build.

## Versioning

Use semantic versions with pre-1.0 compatibility rules:

- `0.x.0` for visible SQL API changes, provider option changes, output schema
  behavior changes, or logging payload changes.
- `0.x.y` patch releases for bug fixes, documentation fixes, test updates, and
  provider compatibility fixes that preserve the SQL API.
- `1.0.0` only after the stable-release readiness gates below are satisfied.
- After `1.0.0`, use normal semantic versioning:
  - `MAJOR` for breaking SQL function, named-option, result-schema, logging
    payload, or secret-scope changes.
  - `MINOR` for additive SQL functions, providers, named options, catalog rows,
    settings, and non-breaking logging fields.
  - `PATCH` for bug fixes, provider compatibility fixes, documentation, tests,
    and cost-catalog refreshes that do not change existing behavior.

Update the extension version in `extension_config.cmake` before tagging a
release:

```cmake
duckdb_extension_load(duckdb_ai
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    EXTENSION_VERSION 0.x.y
)
```

## Required gates

Run these before any preview or stable tag:

```sh
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

Run at least one live smoke before publishing binaries:

- local Ollama for local model mechanics, or
- one credentialed remote provider when provider headers, error handling,
  credentials, structured output, or logging changed.

See `docs/SMOKE_TESTING.md` for the smoke commands.
Record platform validation evidence in `docs/VALIDATION.md` when these gates
are run for a release candidate.

## Stable API contract

After `1.0.0`, keep these public surfaces backward-compatible within a major
version:

- SQL function names, argument order, named-option names, option types, default
  values, and NULL/error behavior.
- Table-function output column names, column order, and DuckDB logical types.
- DuckDB settings names, types, defaults, and disabling semantics.
- DuckDB Secret type name, secret field names, redaction behavior, and automatic
  provider-scope lookup rules.
- Provider request protocols where the extension promises compatibility with a
  provider API family.
- Usage-log field names, event names, privacy defaults, and `ai_usage()` output
  schema.
- JSON validation guarantees that are documented in `README.md`.

Allowed non-breaking changes in a stable minor or patch release:

- adding a new SQL function, provider, named option, DuckDB setting, or usage-log
  field,
- adding rows to `ai_models()` or refreshing existing price values and
  `last_reviewed`,
- improving provider error messages without removing existing provider/status
  context,
- expanding local JSON Schema validation when previously valid output remains
  valid under documented behavior, and
- changing provider default models only in a minor release and only when release
  notes call it out.

Breaking changes require a major version unless they only affect an experimental
surface that was explicitly marked experimental in that release series.

## Stable release readiness

Do not tag `1.0.0` until all of these are true:

1. The public SQL surface in `README.md` has had an API review against
   `test/sql/duckdb_ai.test` and `test/smoke/mock_provider_smoke.py`.
2. `docs/WORKPLAN.md` has no incomplete items that affect the public SQL API,
   provider semantics, logging/cost contract, distribution, or release process.
3. macOS and Linux validation evidence in `docs/VALIDATION.md` matches the
   DuckDB submodule and `extension-ci-tools` versions used for the tag.
4. At least one local Ollama smoke and one credentialed remote-provider smoke
   have been run for the release candidate.
5. The selected binary distribution path in `docs/DISTRIBUTION.md` has install
   instructions, artifact URLs, and a clear statement of whether binaries are
   signed or unsigned.
6. Release notes include upgrade guidance, supported DuckDB version, platform
   coverage, provider smoke coverage, and known limitations.

## Preview release checklist

1. Confirm `docs/WORKPLAN.md` accurately marks any incomplete API or platform
   work.
2. Update `extension_config.cmake` from `0.0.0-dev` to the preview version.
3. Run the required gates.
4. Build release artifacts from the same DuckDB submodule revision used by CI.
5. Record whether artifacts are signed. If they are unsigned, label them as
   unsigned in release notes and install instructions.
6. Tag the release.
7. Publish binaries only through the selected custom unsigned extension
   repository flow described in `docs/DISTRIBUTION.md`.

## Stable release checklist

1. Complete the stable release readiness gates above.
2. Update `extension_config.cmake` to the stable version.
3. Rerun the required gates and record fresh validation evidence.
4. Create release notes using the stable release-note requirements below.
5. Tag the release only after CI is green for the tag candidate commit.
6. Publish binaries through the documented distribution path.
7. Smoke install the published artifact from a clean DuckDB shell before
   announcing the release.

## Release notes

Preview release notes should call out:

- new SQL functions or named options,
- changed provider defaults or request payloads,
- logging payload changes,
- output validation behavior changes,
- known platform coverage, and
- any provider smoke tests run with real credentials.

Stable release notes must also call out:

- supported DuckDB version and extension CI toolchain,
- whether artifacts are signed or explicitly unsigned,
- install command or repository URL,
- upgrade notes for any changed defaults, providers, settings, or log fields,
- compatibility promises for the release series, and
- known limitations, especially provider pricing freshness and non-goals such as
  full JSON Schema draft parity.
