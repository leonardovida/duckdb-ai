---
sidebar_position: 12
---

# Normalize messy documents into structured records

Use this cookbook when operational inputs arrive as mixed JSON, Avro, Excel, or
CSV/Parquet files and need to become one typed enrichment table.

The production pattern is:

1. Read each source with explicit lineage.
2. Normalize all inputs to `document_id`, `source_uri`, `title`, and `body`.
3. Call the model with row-level failure capture.
4. Parse successful JSON into typed columns.
5. Export successes and rejected rows separately.

## Prerequisites

- Configure a completion provider with a `TYPE duckdb_ai` secret.
- Install file-format extensions used by your sources.
- Use object-storage secrets when reading files from S3-compatible storage.

```sql
INSTALL ai FROM community;
LOAD ai;

INSTALL httpfs;
LOAD httpfs;
INSTALL avro;
LOAD avro;
INSTALL excel;
LOAD excel;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-4o-mini'
);

SET VARIABLE run_id = 'document-intake-2026-07-07T100000Z';
```

## Read JSON documents

For newline-delimited JSON, keep the source filename for lineage:

```sql
CREATE OR REPLACE TEMP TABLE raw_json_documents AS
WITH raw AS (
    SELECT
        *,
        row_number() OVER () AS source_row_number
    FROM read_json(
        's3://support-prod/raw/documents/*.jsonl',
        format = 'newline_delimited',
        filename = true,
        union_by_name = true
    )
)
SELECT
    coalesce(id::VARCHAR, md5(filename || ':' || source_row_number::VARCHAR)) AS document_id,
    filename AS source_uri,
    'json' AS source_type,
    title::VARCHAR AS title,
    body::VARCHAR AS body,
    created_at::TIMESTAMP AS created_at
FROM raw;
```

## Read Avro documents

Avro schemas are converted to DuckDB columns. Keep filenames when source paths
carry partition or producer information.

```sql
CREATE OR REPLACE TEMP TABLE raw_avro_documents AS
SELECT
    event_id::VARCHAR AS document_id,
    filename AS source_uri,
    'avro' AS source_type,
    subject::VARCHAR AS title,
    message::VARCHAR AS body,
    event_time::TIMESTAMP AS created_at
FROM read_avro(
    's3://support-prod/raw/events/*.avro',
    filename = true
);
```

## Read Excel handoffs

For inconsistent spreadsheets, start with all text and cast only after
inspection.

```sql
CREATE OR REPLACE TEMP TABLE raw_excel_documents AS
SELECT
    ticket_id::VARCHAR AS document_id,
    's3://support-prod/raw/handoffs/customer_escalations.xlsx' AS source_uri,
    'xlsx' AS source_type,
    subject::VARCHAR AS title,
    notes::VARCHAR AS body,
    try_cast(created_at AS TIMESTAMP) AS created_at
FROM read_xlsx(
    's3://support-prod/raw/handoffs/customer_escalations.xlsx',
    sheet = 'Escalations',
    all_varchar = true,
    ignore_errors = true
);
```

## Normalize the document stream

Keep only rows with enough text for extraction. Store the original source and
type so rejected rows can be traced back.

```sql
CREATE OR REPLACE TEMP TABLE normalized_documents AS
SELECT * FROM raw_json_documents
UNION ALL
SELECT * FROM raw_avro_documents
UNION ALL
SELECT * FROM raw_excel_documents;

CREATE OR REPLACE TEMP TABLE document_batch AS
SELECT
    getvariable('run_id') AS run_id,
    document_id,
    source_uri,
    source_type,
    coalesce(title, '') AS title,
    coalesce(body, '') AS body,
    created_at
FROM normalized_documents
WHERE length(coalesce(title, '') || coalesce(body, '')) >= 20;
```

## Extract structured records

Ask for JSON and preserve row-level errors. Keep the response schema beside the
prompt contract in source control.

```sql
CREATE OR REPLACE TEMP TABLE document_extract_attempts AS
SELECT
    run_id,
    document_id,
    source_uri,
    source_type,
    ai_try_complete(
        'Extract a support document record as JSON with keys '
        || 'category, customer_request, urgency_score, contains_pii. '
        || 'urgency_score must be an integer from 1 to 5.'
        || chr(10) || 'Title: ' || title
        || chr(10) || 'Body: ' || body,
        secret := 'openai_ai',
        response_format := 'json_object',
        retry_count := 2,
        retry_backoff_ms := 1000
    ) AS result
FROM document_batch;
```

Project successful JSON into typed columns:

```sql
CREATE OR REPLACE TEMP TABLE document_records AS
SELECT
    run_id,
    document_id,
    source_uri,
    source_type,
    result.response::JSON->>'category' AS category,
    result.response::JSON->>'customer_request' AS customer_request,
    try_cast(result.response::JSON->>'urgency_score' AS INTEGER) AS urgency_score,
    try_cast(result.response::JSON->>'contains_pii' AS BOOLEAN) AS contains_pii,
    result.response::JSON AS response_json,
    now() AS loaded_at
FROM document_extract_attempts
WHERE result.error IS NULL;
```

Capture rejected rows:

```sql
CREATE OR REPLACE TEMP TABLE document_rejected_rows AS
SELECT
    run_id,
    document_id,
    source_uri,
    source_type,
    result.error AS error,
    now() AS loaded_at
FROM document_extract_attempts
WHERE result.error IS NOT NULL;
```

## Export structured records

Persist records and rejected rows separately:

```sql
COPY document_records
TO 's3://support-prod/ai/document_records/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);

COPY document_rejected_rows
TO 's3://support-prod/ai/document_rejected/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);
```

## Learn more

- [DuckDB JSON loading](https://duckdb.org/docs/current/data/json/loading_json)
- [DuckDB Avro extension](https://duckdb.org/docs/current/core_extensions/avro)
- [DuckDB Excel extension](https://duckdb.org/docs/current/core_extensions/excel)
- [Reading multiple files](https://duckdb.org/docs/current/data/multiple_files/overview)
