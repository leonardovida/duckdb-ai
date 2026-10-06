---
sidebar_position: 14
title: "Extract typed records from model output"
sidebar_label: "Typed records from model output"
description: "Extract structured JSON from LLM output and project it into typed DuckDB columns with ai_extract_record and ai_complete_record."
keywords: ["LLM structured output DuckDB", "JSON Schema extraction", "ai_extract_record", "ai_complete_record"]
---

# Extract typed records from model output

Use this cookbook when you want a model to return structured JSON and use the
result as typed DuckDB values, without parsing JSON text yourself.

There are two functions:

| Function | Input | Result | Use it for |
| --- | --- | --- | --- |
| `ai_extract_record(text, schema)` | A column, one call per row | One `STRUCT` per row | Table rows |
| `ai_complete_record(prompt, schema)` | A constant prompt | A one-row table | One-off, self-contained prompts |

Both validate the response against the JSON Schema before returning it.

## Prerequisites

- Configure a completion provider. The examples use OpenAI `gpt-5.6-luna`, so
  set `OPENAI_API_KEY` in the environment before starting DuckDB. See the
  [provider guides](../provider-guides.md).
- Create the [sample `support_tickets` table](support-ticket-data.md).

## Extract one record per row

`ai_extract_record` takes the row text and a constant JSON Schema. Top-level
schema properties become fields of the returned `STRUCT`. Call it once in a
subquery and read the fields outside it, so each row costs one request:

```sql
SELECT
    ticket_id,
    triage.product_area,
    triage.needs_engineering,
    triage.urgency_score,
    triage.recommended_owner
FROM (
    SELECT
        ticket_id,
        ai_extract_record(
            subject || chr(10) || body,
            '{
              "type": "object",
              "properties": {
                "product_area": {"type": "string"},
                "needs_engineering": {"type": "boolean"},
                "urgency_score": {"type": "integer", "minimum": 0, "maximum": 10},
                "recommended_owner": {"type": "string"}
              },
              "required": ["product_area", "needs_engineering", "urgency_score"]
            }',
            provider := 'openai',
            model := 'gpt-5.6-luna'
        ) AS triage
    FROM support_tickets
);
```

Result shape:

| Column | Type |
| --- | --- |
| `ticket_id` | `INTEGER` |
| `product_area` | `VARCHAR` |
| `needs_engineering` | `BOOLEAN` |
| `urgency_score` | `BIGINT` |
| `recommended_owner` | `VARCHAR` |

The `STRUCT` itself orders its fields alphabetically, so `triage.*` returns
`needs_engineering, product_area, recommended_owner, urgency_score`. Name the
fields, as above, when column order matters. Schema `integer` fields become
`BIGINT`. A property that is not `required` can be NULL.

## Save the records, then filter on typed fields

Save the records in a table first. Later queries then read saved values
instead of calling the model again:

```sql
CREATE OR REPLACE TABLE ticket_triage AS
SELECT ticket_id, triage
FROM (
    SELECT
        ticket_id,
        ai_extract_record(
            subject || chr(10) || body,
            '{
              "type": "object",
              "properties": {
                "product_area": {"type": "string"},
                "needs_engineering": {"type": "boolean"},
                "urgency_score": {"type": "integer", "minimum": 0, "maximum": 10}
              },
              "required": ["product_area", "needs_engineering", "urgency_score"]
            }',
            provider := 'openai',
            model := 'gpt-5.6-luna',
            on_error := 'null'
        ) AS triage
    FROM support_tickets
);

SELECT ticket_id, triage.product_area, triage.urgency_score
FROM ticket_triage
WHERE triage.needs_engineering
  AND triage.urgency_score >= 7;
```

With `on_error := 'null'`, a failed or invalid response leaves `triage` NULL
for that row instead of failing the statement. Find those rows with
`WHERE triage IS NULL`.

## Create one record from a self-contained prompt

`ai_complete_record` is a table function. Its prompt must be a constant: a
column or a subquery is rejected. Use it when the prompt already contains
everything the model needs:

```sql
SELECT *
FROM ai_complete_record(
    'Extract a support triage profile from this ticket. '
    || 'Subject: Query regression after upgrade. '
    || 'Body: Dashboard query that used to finish in 8 seconds now times out after the upgrade.',
    '{
      "type": "object",
      "properties": {
        "product_area": {"type": "string"},
        "needs_engineering": {"type": "boolean"},
        "urgency_score": {"type": "integer", "minimum": 0, "maximum": 10},
        "recommended_owner": {"type": "string"}
      },
      "required": ["product_area", "needs_engineering", "urgency_score"]
    }',
    provider := 'openai',
    model := 'gpt-5.6-luna'
);
```

Result: one row with columns `needs_engineering BOOLEAN`, `product_area
VARCHAR`, `recommended_owner VARCHAR` and `urgency_score BIGINT`. `SELECT *`
returns them in alphabetical order, not in schema order.

## Related

- [Chain several AI steps](chain-ai-steps.md) feeds typed records into later
  steps and reruns without paying twice.
- [Typed decisions](jev-decisions.md) return labels, scores and probabilities
  from a decision model instead of generated JSON.
- [`ai_extract_record` reference](../functions.md#ai_extract_recordtext-response_schema-model-provider)

Remove the example table when you are done:

```sql
DROP TABLE IF EXISTS ticket_triage;
```
