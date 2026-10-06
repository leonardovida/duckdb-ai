---
sidebar_position: 11
title: "Enrich support tickets with AI text functions"
sidebar_label: "Enrich text columns"
description: "Summarize, classify, filter, extract, redact and translate text columns with LLM functions in DuckDB SQL."
keywords: ["DuckDB LLM classification", "ai_summarize", "ai_classify", "ai_translate"]
---

# Enrich support tickets with AI text functions

Use this cookbook when you have text columns in a local table and want to add AI
summaries, labels, extraction, redaction, or translations in SQL.

## Prerequisites

- Build and load the extension.
- Configure a completion provider with session settings or a `TYPE duckdb_ai`
  secret. Hosted providers read their key from the environment, for example
  `OPENAI_API_KEY`. See the [provider guides](../provider-guides.md).
- Create the [sample `support_tickets` table](support-ticket-data.md).

Every function on this page sends one request per non-NULL row.

## Summarize each ticket

Combine the columns that give the model enough context:

```sql
SELECT
    ticket_id,
    ai_summarize(subject || chr(10) || body) AS ticket_summary
FROM support_tickets;
```

Result: one `ticket_summary VARCHAR` per ticket.

## Summarize by customer

Use the aggregate form when several rows belong to the same account:

```sql
SELECT
    customer_id,
    ai_summarize_agg(subject || ': ' || body ORDER BY created_at) AS summary
FROM support_tickets
GROUP BY customer_id;
```

Result: one `summary VARCHAR` per customer.

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

Result: `category VARCHAR`, always one of the listed labels.

## Filter rows with a natural-language predicate

Use `ai_filter` when keyword logic would be too brittle. Send only the columns
the model needs: `internal_note` contains an email address, so it stays out of
the prompt until it has been [redacted](#redact-internal-notes).

```sql
SELECT *
FROM support_tickets
WHERE ai_filter(
    subject || chr(10) || body,
    'mentions urgent production impact or needs engineering follow-up'
);
```

## Answer several questions in one call

The classify and filter steps above each send one request per row. With a
decision-model provider (`typesafe`, `cloudflare`, `perplexity`, `ollama` 0.35
or later, or `systemone`), one `ai_decide` call answers both questions and
returns a confidence for the label and a probability for the yes/no question.
You then choose the thresholds in SQL. See
[route rows by decision confidence](decision-routing.md). If you only have a
chat provider, keep `ai_classify` and `ai_filter`.

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

Result: `extracted_json VARCHAR`. The function does not check the JSON against a
schema.

Use [`ai_extract_record`](../functions.md#ai_extract_recordtext-response_schema-model-provider)
to get one typed `STRUCT` per row instead. See
[typed records from model output](structured-triage-records.md).

## Redact internal notes

Redact notes before sharing operational context outside the team:

```sql
SELECT
    ticket_id,
    ai_redact(internal_note) AS redacted_internal_note
FROM support_tickets;
```

## Translate customer text

Translate only the rows that need it:

```sql
SELECT
    ticket_id,
    language,
    ai_translate(subject || ': ' || body, 'English') AS english_text
FROM support_tickets
WHERE language <> 'en';
```

## Inspect usage

After running provider calls, inspect recent usage events:

```sql
SELECT function_name, provider, model, prompt_tokens, completion_tokens, total_tokens, elapsed_ms
FROM ai_usage()
ORDER BY event_id DESC
LIMIT 10;
```

`ai_usage()` keeps the latest 1,024 events for the current DuckDB instance. See
[usage and cost monitoring](usage-cost-observability.md) to keep them longer.
