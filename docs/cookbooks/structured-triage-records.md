---
sidebar_position: 5
---

# Extract typed records from model output

Use this cookbook when you want a model to return structured JSON and project the
result into typed DuckDB columns.

## Prerequisites

- Configure a completion provider.
- Create the [sample `support_tickets` table](support-ticket-data.md) if you want
  local ticket context for your prompt.

## Create one triage record

`ai_complete_record` takes a prompt and a JSON Schema. Top-level schema
properties become DuckDB columns.

```sql
SELECT *
FROM ai_complete_record(
    'Extract a support triage profile for the query regression ticket.',
    '{
      "type": "object",
      "properties": {
        "product_area": {"type": "string"},
        "needs_engineering": {"type": "boolean"},
        "urgency_score": {"type": "integer"},
        "recommended_owner": {"type": "string"}
      },
      "required": ["product_area", "needs_engineering", "urgency_score"]
    }',
    provider := 'openai',
    model := 'gpt-4o-mini'
);
```

Result shape:

| Column | Type |
| --- | --- |
| `product_area` | `VARCHAR` |
| `needs_engineering` | `BOOLEAN` |
| `urgency_score` | `BIGINT` |
| `recommended_owner` | `VARCHAR` |

## Prepare table context for the prompt

`ai_complete_record` is a table function, so its prompt is bound as a constant
argument. For a row-specific record, first prepare the prompt text you want to
send:

```sql
SELECT
    'Extract a support triage profile from this ticket: ' ||
    subject || chr(10) ||
    body || chr(10) ||
    internal_note AS prompt
FROM support_tickets
WHERE ticket_id = 1004;
```

Then use that text as the prompt argument to `ai_complete_record`.

## Choose this over JSON text when types matter

Use this pattern when downstream SQL needs typed fields:

```sql
WITH triage AS (
    SELECT *
    FROM ai_complete_record(
        'Extract a support triage profile for ticket 1004.',
        '{
          "type": "object",
          "properties": {
            "product_area": {"type": "string"},
            "needs_engineering": {"type": "boolean"},
            "urgency_score": {"type": "integer"}
          },
          "required": ["product_area", "needs_engineering", "urgency_score"]
        }',
        provider := 'openai',
        model := 'gpt-4o-mini'
    )
)
SELECT *
FROM triage
WHERE needs_engineering
  AND urgency_score >= 7;
```
