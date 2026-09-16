---
sidebar_position: 9
---

# Resume a local enrichment job

Use the source-checkout example `examples/resumable_enrichment.py` when a local
batch job must resume after closing DuckDB without calling the provider again
for saved successful rows. This is a single-writer example using the existing
`ai_try_complete` API and a persistent DuckDB database.

## Prepare the job

Use stable source identifiers and non-null prompts. Keep the checkpoint database
on durable local storage. Configure credentials through the provider's environment
variables before starting the process, following the [provider guide](../provider-guides.md).
Never put credentials in prompts or configuration versions.

The checkpoint identity includes the source identifier, prompt hash and explicit
configuration version. Change the configuration version when changing anything
that affects results, including provider, model, model revision, endpoint, prompt
policy or generation settings. A moving model alias cannot automatically invalidate
stored results. Changing the prompt itself creates a new identity.

From the repository root, create `input.csv`:

```csv
source_id,prompt
1,Summarize in one sentence: I was charged twice.
2,Summarize in one sentence: My query became slow.
```

Use Python 3 and a DuckDB shell with `ai` installed, or the source build at
`build/release/duckdb`. After configuring a provider, run one batch:

```sh
python3 examples/resumable_enrichment.py \
  --duckdb ./build/release/duckdb \
  --database ./enrichment.duckdb \
  --input ./input.csv \
  --provider ollama \
  --model qwen3.8:27b \
  --config-version triage-v1 \
  --batch-size 100
```

This command calls your configured model. The provider must be running or
accessible. Hosted providers may charge for these calls. The optional
`--base-url` overrides the endpoint, and credentials remain in environment
variables. Provider, model and explicit endpoint are included automatically in
the configuration fingerprint. Keep `--config-version` for changes the example
cannot detect, such as a model revision behind an alias.

## Resume and inspect results

Each invocation selects a bounded batch of pending work before evaluating the
model function. Results are materialized once, then saved. Run the same command
again to process remaining rows and retry failures. Reopening the same checkpoint
database retains successful rows and avoids repeating their calls.

A provider error remains eligible for a later attempt. A successful provider
response means the API returned text, not that the content is correct. Add the
validation required by your application before using model output for decisions.
Older prompt/configuration versions remain in the checkpoint database for inspection.
Untouched rows are selected before failed rows, followed by the least-attempted
failures, so permanent failures do not block new work. Each invocation prints
`attempted`, `succeeded` and `failed` counts. Zero attempted rows means every
current input identity already has a saved success.

Inspect the checkpoint with the same DuckDB shell:

```sh
./build/release/duckdb enrichment.duckdb -c \
  'SELECT source_id, config_version, response, error, attempts FROM enrichment_results ORDER BY source_id;'
```

This table includes historical versions. Filter by `config_hash` and
`prompt_hash` when adapting the example into a downstream join. The example
replaces a failed identity with its latest attempt, rather than keeping an
attempt-by-attempt audit log. Duplicate identifiers or missing prompts fail
before provider calls.

Run the local HTTP mock experiment without credentials or live model calls:

```sh
python3 test/smoke/resumable_enrichment_smoke.py --duckdb ./build/release/duckdb
```

On POSIX systems, it checks bounded batches, failure retries, persistence across
new processes, prompt/configuration invalidation, input rejection and rollback
after killing a process midway through a batch. The crash check also confirms
that the first provider call is repeated on restart when its result was not committed.

## Understand the limits

- Run one writer at a time. This example does not coordinate workers or schedule jobs.
- A crash after the provider responds but before the batch commits can repeat
  requests on restart. Database transactions cannot roll back an external model
  call or its cost. Smaller batches reduce the amount of uncommitted work.
- The database stores prompts, responses and error text. Apply the same access
  and retention controls as the source data. A prompt hash is an identity aid,
  not a privacy boundary.
- Failed rows can be retried indefinitely across invocations. Inspect persistent
  errors before repeatedly running the job, and use a separate remediation queue
  for permanent failures when adapting this example.

For object-storage exports and usage capture, see
[production batch enrichment](production-batch-enrichment.md) and
[usage monitoring](usage-cost-observability.md).
