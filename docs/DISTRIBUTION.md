# Distribution Status

The checked-in GitHub Actions workflow currently builds and runs quality checks
for `duckdb_ai` against DuckDB `v1.5.4` with `extension-ci-tools`
`v1.5-variegata`.

It does not publish extension binaries yet. Until the DuckDB community extension
submission is accepted, users should build and load the extension from source.

## Current release stance

- Pull-request build, SQLLogic, and mock-provider smoke CI is enabled through
  `.github/workflows/MainDistributionPipeline.yml`.
- Public binary distribution is not configured.
- The initial preview source release is `0.1.0` in `extension_config.cmake`.
- The extension name is `duckdb_ai` across the build target, loadable
  extension, secret type, and SQL docs.
- The repository is readying a `0.x` community-extension preview; the actual
  community repository submission is a manual follow-up.

## First distribution target

Use the DuckDB community extension repository for the first public binary
distribution. A community entry is the clearest install path for DuckDB users
and gives release signing/provenance through the DuckDB extension pipeline.

Before submitting the community PR, verify:

- the SQL function names and named options are intentionally stable for a `0.x`
  preview,
- README and docs do not claim `INSTALL duckdb_ai FROM community` until the
  catalog entry exists,
- macOS arm64/x64, Linux glibc, Linux musl, and Windows builds are green through
  the extension distribution workflow, and
- WASM is explicitly excluded for the first preview because this extension links
  libcurl for provider HTTP calls.

## Publishing gap

The next publishing step is the manual community-extension submission. Before
that submission, run or inspect a distribution workflow that:

- builds the same DuckDB version as CI,
- runs SQLLogic and mock-provider smoke tests,
- builds the target platform matrix listed above, and
- records the explicit WASM exclusion.

## Stable distribution requirements

Before `1.0.0`, the distribution path must be explicit enough that a user can
install a known binary without guessing provenance:

- Publish the DuckDB version, extension version, target platform, and artifact
  checksum for every binary.
- State whether the binary is signed. If it is unsigned, keep "unsigned" in the
  repository/install wording and release notes.
- Keep the artifact URL layout stable across patch releases in a release series.
- Document a clean install smoke command that loads the published extension and
  runs deterministic local checks such as `ai_provider_protocol('openai')`,
  `ai_models()`, and `ai_request_json(...)`.
- Keep source-build instructions separate from community install instructions
  until the community catalog entry exists.

After the community catalog entry exists, public docs should lead with:

```sql
INSTALL duckdb_ai FROM community;
LOAD duckdb_ai;
```
