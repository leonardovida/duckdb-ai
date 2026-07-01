# Distribution Status

The checked-in GitHub Actions workflow currently builds and runs quality checks
for `duckdb_ai` against DuckDB `v1.5.4` with `extension-ci-tools`
`v1.5-variegata`.

It does not publish extension binaries. The inherited `scripts/extension-upload.sh`
helper is present for future custom repository publishing, but no workflow calls
it yet and no signing or S3 target is configured.

## Current release stance

- Build-only CI is enabled through
  `.github/workflows/MainDistributionPipeline.yml`.
- Public binary distribution is not configured.
- The initial preview source release is `0.1.0` in `extension_config.cmake`.
- The repository should stay in source-first mode until the SQL function surface
  and provider configuration contract stabilize.

## First distribution target

Use a custom unsigned extension repository first.

That path matches the current state of the project: the SQL function surface and
provider configuration contract are still moving, and this extension has network,
credential, logging, and model-provider semantics that should settle before a
DuckDB community extension submission. A custom repository lets early users test
explicit builds while keeping install instructions honest about trust and binary
provenance.

Defer the DuckDB community extension flow until:

- the public SQL function names and named options are stable,
- provider credentials and logging behavior have completed at least one
  end-to-end user review,
- macOS and Linux builds are validated against the pinned DuckDB release, and
- the release process has a signed or otherwise clearly documented binary
  provenance story.

## Publishing gap

The next publishing step is not to enable the inherited upload script directly.
First add a release workflow that:

- builds the same DuckDB version as CI,
- runs SQLLogic and mock-provider smoke tests,
- signs or clearly labels unsigned extension binaries,
- uploads artifacts to the selected custom repository, and
- publishes matching install instructions for the artifact URL layout.

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
- Keep preview/custom repository instructions separate from any future DuckDB
  community extension submission instructions.

The first stable release can still use the custom unsigned repository path if
that provenance is clearly documented. A DuckDB community extension submission
is a separate distribution milestone, not a prerequisite for `1.0.0`.
