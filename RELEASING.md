# Releasing

Checklist for cutting a release and publishing it to the DuckDB community
extension repository.

## 1. Prepare the release

- [ ] Move `## Unreleased` items in `CHANGELOG.md` under a new
      `## X.Y.Z - YYYY-MM-DD` heading. Before `1.0.0`, minor versions may
      change the SQL API; patch versions must preserve it.
- [ ] Bump `EXTENSION_VERSION` in `extension_config.cmake`.
- [ ] Build and validate:

  ```sh
  GEN=ninja make release
  GEN=ninja make test
  python3 test/smoke/mock_provider_smoke.py
  python3 test/smoke/extension_upload_smoke.py
  PATH=/tmp/duckdb_ai_format_venv/bin:$PATH GEN=ninja make format-check
  ```

- [ ] Check the community docs page won't regress: the duckdb.org extension
      page is generated from the built binary's `duckdb_functions()` metadata,
      so preview it locally and refresh the snapshot if function docs changed:

  ```sh
  scripts/preview_community_docs.sh --check   # or --update after intentional changes
  ```

- [ ] Land the release PR on `main`.

## 2. Tag and publish the GitHub release

```sh
git tag -a vX.Y.Z -m "vX.Y.Z" <merge-commit>
git push origin vX.Y.Z
gh release create vX.Y.Z --title "vX.Y.Z" --notes-from-tag
```

Use the CHANGELOG section as the release notes body.

## 3. Publish to the DuckDB community extension repository

The duckdb.org page and `INSTALL ai FROM community` binaries only update after
publication through [duckdb/community-extensions](https://github.com/duckdb/community-extensions).
A GitHub release and verified distribution bundles do not establish community
catalog availability.

### DuckDB 1.5.6 maintenance releases

The catalog now targets DuckDB 2.0 by default. For this repository's DuckDB
1.5.6 release line, follow the upstream
[version transition notice](https://github.com/duckdb/community-extensions/issues/3081):

- Edit `extensions/ai/description.yml`:
  - `extension.version`: the new maintenance version.
  - `repo.variegata`: the exact release merge commit SHA.
  - Keep `repo.ref` unchanged for a release that only supports DuckDB 1.5.6.
- Start the PR description with an explicit request to build and deploy the
  DuckDB 1.5.6 / `variegata` line. Link the release, exact-commit CI and immutable
  artifact verification.
- Request the maintainer's supported-version build and deployment with matching
  DuckDB 1.5.6 CI tooling/tag and tests enabled. Legacy publication is not
  automatic, and requires upstream permissions.
- Verify the terminal upstream workflow and a fresh isolated
  `INSTALL ai FROM community; LOAD ai;` with DuckDB 1.5.6 before claiming catalog
  publication. Read back `extension_version` from `duckdb_extensions()`.

PR title example: `ai: update v1.5.6/variegata to vX.Y.Z`.

### Newer DuckDB release lines

Update `repo.ref` only after the extension has been made compatible with the
catalog's current default DuckDB version and its corresponding checks pass.
The current 2.0 C++ API port is separate from a 1.5.6 maintenance release. Do not
change or bypass required checks to publish an unsupported build.
