#!/usr/bin/env python3
"""Independent labeled fixtures exercise acceptance gates without provider calls."""

import json
import runpy
from pathlib import Path


def run():
    root = Path(__file__).resolve().parents[2]
    evaluator = runpy.run_path(str(root / "examples/jev_batch_evaluation.py"))
    fixture = json.loads((root / "test/fixtures/classification_quality.json").read_text())
    metrics = evaluator["classification_metrics"](fixture["rows"], fixture["labels"])
    assert metrics["accuracy"] == 4 / 6
    assert metrics["coverage"] == 5 / 6
    assert metrics["per_class"]["billing"]["recall"] == 1 / 3
    gate = evaluator["check_quality_gate"]
    runs = [{"batch_size": 1, "metrics": metrics}]
    assert gate(runs, {"accuracy": 0.6, "macro_f1": 0.7, "coverage": 0.8, "class_recall": 0.3}, fixture["labels"])[
        "passed"
    ]
    rejected = gate(runs, {"accuracy": 0.7, "macro_f1": 0.8, "coverage": 0.9, "class_recall": 0.5}, fixture["labels"])
    assert not rejected["passed"] and len(rejected["failures"]) == 4
    assert not gate(runs, {}, ["unobserved"])["passed"]
    assert not gate([], {}, fixture["labels"])["passed"]
    missing = evaluator["classification_metrics"](
        [{"label": "billing", "prediction": None, "failed": True}], fixture["labels"]
    )
    assert not gate([{"batch_size": 8, "metrics": missing}], {"accuracy": 0.01}, ["billing"])["passed"]
    print("accuracy gate smoke passed: multilingual, minority, out-of-domain, failures and absent classes")


if __name__ == "__main__":
    run()
