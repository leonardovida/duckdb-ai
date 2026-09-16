#!/usr/bin/env python3
"""Process one bounded batch from CSV (source_id, prompt) into a local DuckDB file."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", default="duckdb", help="DuckDB shell with ai available")
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True, help="CSV with unique source_id and prompt strings")
    parser.add_argument("--provider", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--base-url", default="", help="Optional provider endpoint override, never credentials")
    parser.add_argument(
        "--config-version", required=True, help="Change when result-affecting policy or model revision changes"
    )
    parser.add_argument("--batch-size", type=int, default=100)
    args = parser.parse_args()
    if args.batch_size < 1 or not args.config_version.strip():
        parser.error("batch-size must be positive and config-version must be nonempty")
    # Include fixed generation settings if this example is adapted to add any.
    config = json.dumps([args.provider, args.model, args.base_url, args.config_version], ensure_ascii=False)
    fingerprint = hashlib.sha256(config.encode()).hexdigest()
    endpoint = f", base_url := {literal(args.base_url)}" if args.base_url else ""
    sql = f"""
LOAD ai;
CREATE TEMP TABLE job_input AS
SELECT source_id, prompt, sha256(prompt) AS prompt_hash
FROM read_csv({literal(args.input.resolve())}, header=true,
               types={{source_id: 'VARCHAR', prompt: 'VARCHAR'}});
SELECT CASE WHEN count(*) != count(DISTINCT source_id)
                 OR count(*) FILTER (WHERE source_id IS NULL OR prompt IS NULL OR source_id = '') > 0
            THEN error('input requires unique nonempty source_id and non-null prompt')
            ELSE 'input validated' END FROM job_input;
CREATE TABLE IF NOT EXISTS enrichment_results (
    source_id VARCHAR, prompt_hash VARCHAR, config_hash VARCHAR,
    config_version VARCHAR, prompt VARCHAR, response VARCHAR, error VARCHAR,
    attempts BIGINT, completed_at TIMESTAMPTZ,
    PRIMARY KEY (source_id, prompt_hash, config_hash)
);
BEGIN TRANSACTION;
CREATE TEMP TABLE pending AS
SELECT i.*, coalesce(r.attempts, 0) + 1 AS attempts
FROM job_input i LEFT JOIN enrichment_results r
  ON i.source_id = r.source_id AND i.prompt_hash = r.prompt_hash
 AND r.config_hash = {literal(fingerprint)}
WHERE r.source_id IS NULL OR r.error IS NOT NULL
ORDER BY coalesce(r.attempts, 0), i.source_id
LIMIT {args.batch_size};
CREATE TEMP TABLE batch_results AS
SELECT *, ai_try_complete(prompt, provider := {literal(args.provider)},
                          model := {literal(args.model)}, retry_count := 0{endpoint}) AS result
FROM pending;
INSERT OR REPLACE INTO enrichment_results
SELECT source_id, prompt_hash, {literal(fingerprint)}, {literal(args.config_version)},
       prompt, result.response, result.error, attempts, current_timestamp
FROM batch_results;
COMMIT;
SELECT count(*) AS attempted,
       count(*) FILTER (WHERE result.error IS NULL) AS succeeded,
       count(*) FILTER (WHERE result.error IS NOT NULL) AS failed
FROM batch_results;
"""
    subprocess.run([args.duckdb, "-batch", "-bail", str(args.database.resolve())], input=sql, text=True, check=True)


if __name__ == "__main__":
    main()
