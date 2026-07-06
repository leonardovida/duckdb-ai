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

The duckdb.org page and `INSTALL ai FROM community` binaries only update when
the pinned commit in [duckdb/community-extensions](https://github.com/duckdb/community-extensions)
moves. Open a small PR there:

- Edit `extensions/ai/description.yml`:
  - `extension.version`: the new version.
  - `repo.ref`: the release merge commit SHA on `main`.
- PR title convention: `ai: bump to vX.Y.Z`.

Once merged, community CI rebuilds the extension for every platform and
regenerates the docs page (including the Added Functions / Added Settings
tables) from the new binary.
