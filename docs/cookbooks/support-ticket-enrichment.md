---
sidebar_position: 3
---

# Enrich support tickets with AI text functions

Use this cookbook when you have text columns in a local table and want to add AI
summaries, labels, extraction, redaction, or translations in SQL.

## Prerequisites

- Build and load the extension.
- Configure a completion provider with settings or a `TYPE duckdb_ai` secret.
- Create the [sample `support_tickets` table](support-ticket-data.md).

## Summarize each ticket

Combine the columns that give the model enough context:

```sql
SELECT
    ticket_id,
    ai_summarize(subject || chr(10) || body) AS ticket_summary
FROM support_tickets;
```

## Summarize by customer

Use the aggregate form when several rows belong to the same account:

```sql
SELECT
    customer_id,
    ai_summarize_agg(subject || ': ' || body ORDER BY created_at) AS summary
FROM support_tickets
GROUP BY customer_id;
```

## Classify tickets

Keep labels short and mutually exclusive:

```sql
SELECT
    ticket_id,
    ai_classify(
        subject || chr(10) || body,
        'billing, performance, integration, documentation, other'
    ) AS category
FROM support_tickets;
```

## Filter rows with a natural-language predicate

Use `ai_filter` when keyword logic would be too brittle:

```sql
SELECT *
FROM support_tickets
WHERE ai_filter(
    subject || chr(10) || body || chr(10) || internal_note,
    'mentions urgent production impact or needs engineering follow-up'
);
```

## Extract compact JSON

Use `ai_extract` for lightweight structured values that can stay in a JSON text
column:

```sql
SELECT
    ticket_id,
    ai_extract(
        body,
        'Return compact JSON with product_area, customer_request, and urgency'
    ) AS extracted_json
FROM support_tickets;
```

Use [`ai_complete_record`](structured-triage-records.md) instead when you need
typed DuckDB columns.

## Redact internal notes

Redact notes before sharing operational context outside the team:

```sql
SELECT
    ticket_id,
    ai_redact(internal_note) AS redacted_internal_note
FROM support_tickets;
```

## Translate customer-facing text

Translate only the rows that need it:

```sql
SELECT
    ticket_id,
    ai_translate(subject || ': ' || body, 'Dutch') AS dutch_update
FROM support_tickets
WHERE language = 'en';
```

## Inspect usage

After running provider calls, inspect recent usage events:

```sql
SELECT provider, model, request_tokens_estimated, output_tokens_estimated, elapsed_ms
FROM ai_usage()
ORDER BY event_id DESC
LIMIT 10;
```
