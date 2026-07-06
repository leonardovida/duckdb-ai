#!/usr/bin/env bash
# Preview the function/settings tables that duckdb.org generates for the
# community extension page (https://duckdb.org/community_extensions/extensions/ai),
# without publishing a release.
#
# Replicates the table-generation queries from
# duckdb/community-extensions scripts/generate_md.sh: it loads the locally
# built extension into a vanilla DuckDB binary and diffs duckdb_functions(),
# duckdb_settings(), and duckdb_types() against a clean instance.
#
# Usage:
#   scripts/preview_community_docs.sh            # generate build/community_docs/*.md
#   scripts/preview_community_docs.sh --check    # diff against test/community_docs_snapshot/
#   scripts/preview_community_docs.sh --update   # refresh the committed snapshot
#
# Requirements:
#   - built extension: build/release/extension/ai/ai.duckdb_extension
#     (GEN=ninja make release)
#   - a vanilla `duckdb` binary on PATH (or DUCKDB_BIN=...) whose version
#     matches the submodule; the bundled build/release/duckdb cannot be used
#     because it statically links the extension into the "before" snapshot.
set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT="$ROOT/build/release/extension/ai/ai.duckdb_extension"
DUCKDB_BIN="${DUCKDB_BIN:-duckdb}"
DUCKDB="$DUCKDB_BIN -init /dev/null"
OUT="$ROOT/build/community_docs"
SNAPSHOT="$ROOT/test/community_docs_snapshot"
MODE="${1:-generate}"

if [ ! -f "$EXT" ]; then
    echo "error: $EXT not found; run GEN=ninja make release first" >&2
    exit 1
fi
if ! command -v "$DUCKDB_BIN" >/dev/null; then
    echo "error: vanilla duckdb binary not found (set DUCKDB_BIN)" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT"

for thing in functions settings types; do
    $DUCKDB pre.db -c "CREATE OR REPLACE TABLE $thing AS FROM duckdb_$thing();"
    $DUCKDB -unsigned post.db -c "LOAD '$EXT'; CREATE OR REPLACE TABLE $thing AS FROM duckdb_$thing();"
done

# Same queries as community-extensions generate_md.sh (jekyll_format_function
# escapes {{ }} in examples for the Jekyll renderer on duckdb.org).
$DUCKDB post.db -c "CREATE MACRO jekyll_format_function(f) AS f.regexp_replace('({{|}})','{% raw %}\\1{% endraw %}', 'g');"
$DUCKDB post.db -c "ATTACH 'pre.db'; CREATE OR REPLACE TABLE fun_no_overload AS SELECT function_name, function_type, split_part(description, chr(10), 1) as description, comment, CASE WHEN examples = [] THEN '' ELSE '[' || examples.list_transform(lambda x: x.jekyll_format_function()).list_reduce(lambda x, y : x || ', ' || y) || ']' END AS examples FROM (FROM (SELECT function_name, function_type, description, comment, examples FROM functions ORDER BY function_name, function_type) EXCEPT (SELECT function_name, function_type, description, comment, examples FROM pre.functions ORDER BY function_name, function_type)) GROUP BY ALL ORDER BY function_name, function_type;"
$DUCKDB post.db -markdown -c "FROM fun_no_overload;" > functions.md
$DUCKDB post.db -c "ATTACH 'pre.db'; CREATE OR REPLACE TABLE new_settings AS FROM ( SELECT * EXCLUDE (value) FROM settings ORDER BY name) EXCEPT (SELECT * EXCLUDE (value) FROM pre.settings ORDER BY name) ORDER BY name;"
$DUCKDB post.db -markdown -c "FROM new_settings;" > settings.md
rm -f pre.db post.db

case "$MODE" in
--check)
    diff -u "$SNAPSHOT/functions.md" "$OUT/functions.md"
    diff -u "$SNAPSHOT/settings.md" "$OUT/settings.md"
    echo "community docs snapshot is up to date"
    ;;
--update)
    mkdir -p "$SNAPSHOT"
    cp "$OUT/functions.md" "$OUT/settings.md" "$SNAPSHOT/"
    echo "snapshot updated in $SNAPSHOT"
    ;;
*)
    echo "generated $OUT/functions.md and $OUT/settings.md"
    ;;
esac
