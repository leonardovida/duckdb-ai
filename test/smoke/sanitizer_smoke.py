#!/usr/bin/env python3
"""Run owned-code ASan/UBSan regressions using an uninstrumented DuckDB host."""

import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

from repository_regression_smoke import run as regressions
from repository_pass2_smoke import run as pass2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    args = parser.parse_args()
    runtime = subprocess.check_output(["gcc", "-print-file-name=libasan.so"], text=True).strip()
    if not Path(runtime).is_file():
        raise RuntimeError("GCC AddressSanitizer runtime is unavailable")
    load = "LOAD '" + str(args.extension.resolve()).replace("'", "''") + "';"
    with tempfile.TemporaryDirectory(prefix="ai-sanitizer-") as directory:
        wrapper = Path(directory) / "duckdb"
        command = [
            "env",
            "LD_PRELOAD=" + runtime,
            "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1",
            "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1",
            str(args.duckdb.resolve()),
            "-unsigned",
            "-cmd",
            load,
        ]
        wrapper.write_text("#!/bin/sh\nexec " + " ".join(shlex.quote(value) for value in command) + ' "$@"\n')
        wrapper.chmod(0o700)
        regressions(wrapper)
        pass2(wrapper)
        subprocess.run(
            ["python3", str(Path(__file__).with_name("reliability_smoke.py")), "--duckdb", str(wrapper)], check=True
        )
    print("owned C++ ASan/UBSan checks passed")


if __name__ == "__main__":
    main()
