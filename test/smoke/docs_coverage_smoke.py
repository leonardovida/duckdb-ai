#!/usr/bin/env python3
"""Check that every registered ai_* function and duckdb_ai_* setting is documented."""

import argparse
import json
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REFERENCE = ROOT / "docs/functions.md"


def query(duckdb, sql):
    result = subprocess.run(
        [str(duckdb), "-batch", "-bail", "-json", "-init", os.devnull],
        input="LOAD ai;" + sql,
        text=True,
        capture_output=True,
        timeout=60,
    )
    assert result.returncode == 0, result.stderr
    return [row["name"] for row in json.loads(result.stdout or "[]")]


def run(duckdb):
    reference = REFERENCE.read_text()
    functions = query(
        duckdb,
        "SELECT DISTINCT function_name AS name FROM duckdb_functions() "
        "WHERE starts_with(function_name, 'ai_') ORDER BY 1;",
    )
    settings = query(
        duckdb,
        "SELECT name FROM duckdb_settings() WHERE starts_with(name, 'duckdb_ai') ORDER BY 1;",
    )
    assert functions and settings, (functions, settings)
    missing = [f"function {name}" for name in functions if f"`{name}(" not in reference]
    missing += [f"setting {name}" for name in settings if f"`{name}`" not in reference]
    assert not missing, f"Not documented in {REFERENCE.relative_to(ROOT)}: " + ", ".join(missing)
    print(f"Docs coverage smoke passed ({len(functions)} functions, {len(settings)} settings)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=ROOT / "build/release/duckdb")
    run(parser.parse_args().duckdb)
