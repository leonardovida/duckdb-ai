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
  GEN=ninja make format-check
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

### DuckDB 2.0 releases (`repo.ref`)

`main` builds against DuckDB 2.0.0, the catalog's default line since the
[version transition notice](https://github.com/duckdb/community-extensions/issues/3081).

- Edit `extensions/ai/description.yml`:
  - `extension.version`: the new version.
  - `repo.ref`: the exact release merge commit SHA.
  - Keep `repo.variegata` pinned to the last DuckDB 1.5.6 release (below).
- Link the release and the exact-commit CI run in the PR description.
- Verify the terminal upstream workflow and a fresh isolated
  `INSTALL ai FROM community; LOAD ai;` with DuckDB 2.0.0 before claiming catalog
  publication. Read back `extension_version` from `duckdb_extensions()`.

PR title example: `ai: update to vX.Y.Z for DuckDB 2.0`.

### DuckDB 1.5.6 maintenance releases (`repo.variegata`)

0.8.4 (`17c3633c2ab4a1944358b676e84e0a8b41cd2c8d`) is the last release built
against DuckDB 1.5.6. A 1.5.6 fix ships from a maintenance branch cut from that
tag, with the submodules and CI pinned to DuckDB 1.5.6 / `v1.5-variegata`:

- Set `extension.version` to the maintenance version and `repo.variegata` to its
  exact merge commit SHA; leave `repo.ref` on the 2.0 line.
- Start the PR description with an explicit request to build and deploy the
  DuckDB 1.5.6 / `variegata` line, linking the release, exact-commit CI and
  artifact verification. Legacy publication is not automatic and needs a
  maintainer's supported-version build with matching tooling and tests enabled.
- Verify a fresh isolated `INSTALL ai FROM community; LOAD ai;` with DuckDB 1.5.6.

PR title example: `ai: update v1.5.6/variegata to vX.Y.Z`.

Do not change or bypass required checks to publish an unsupported build.
