---
sidebar_position: 16
title: "Route rows by decision confidence"
sidebar_label: "Route by decision confidence"
description: "Answer several typed questions per row with ai_decide, accept confident answers, and send low-confidence rows to a chat-model fallback or a review table in DuckDB SQL."
keywords: ["ai_decide", "confidence threshold", "LLM routing SQL", "human review queue", "decision model vs chat model"]
---

# Route rows by decision confidence

Use this cookbook when you label many rows and want to act automatically only
on the answers the model is sure about. A decision model answers several
fixed-choice questions per row in one call and returns a confidence or a
probability with each answer. You accept the confident rows, send the
uncertain ones to a chat model that also explains its answer, and put the rest
in a review table.

## Prerequisites

- A decision model. The examples use the local Ollama model `nimble`, which
  needs Ollama 0.35 or later and `ollama pull nimble`. To use another decision
  provider (`typesafe`, `cloudflare`, `perplexity` or `systemone`), change
  `provider` and `model`. See [decision models](../provider-guides.md#decision-models).
- A chat model for the fallback step. The examples use OpenAI `gpt-5.6-luna`,
  so set `OPENAI_API_KEY` in the environment before starting DuckDB.
- The [sample `support_tickets` table](support-ticket-data.md).

## Decision model or chat model?

| | Decision model: `ai_decide` | Chat model: `ai_classify`, `ai_extract_record` |
| --- | --- | --- |
| What you get back | A label from your list with a confidence, a level index, or a probability of yes | Text or JSON, checked against your labels or JSON Schema |
| Several questions about one row | One request answers all of them | One request per function call, or one JSON Schema with several fields |
| Confidence for a threshold | Yes | No |
| Free text, such as a reason or a summary | No | Yes |
| Providers | `typesafe`, `cloudflare`, `perplexity`, `ollama` 0.35 or later, `systemone` | Any completion provider |

Use a decision model for fixed-choice questions at volume, when you want to set
your own threshold. Use a chat model when you need an explanation or other free
text, or when you only have a chat provider. This page uses both: the decision
model for every row, and the chat model only for the rows it is unsure about.

## Ask several questions in one call

Each field of the second argument is one question. A `MAP` of labels is a
choice, a list is an ordered score, and a `MAP` with the keys `true` and
`false` is a yes/no question. The `{instructions, criteria}` form adds an
instruction to one question. Save the result in a table, so the queries after
this one read saved values instead of calling the model again:

```sql
CREATE OR REPLACE TABLE route_decisions AS
SELECT
    ticket_id,
    ai_decide(
        subject || chr(10) || body,
        {
            team: MAP {
                'billing': 'Invoices, payments, refunds and billing contacts',
                'performance': 'Slow or failing queries, timeouts and regressions',
                'integration': 'Exports, imports and connections to other tools',
                'other': 'Anything else, including documentation requests'
            },
            severity: [
                'Routine question',
                'Degraded work with a workaround',
                'Work is blocked'
            ],
            urgent: {
                instructions: 'Judge urgency from the business impact the customer describes.',
                criteria: MAP {
                    'true': 'Needs action today',
                    'false': 'Can wait for the normal queue'
                }
            }
        },
        provider := 'ollama',
        model := 'nimble',
        on_error := 'null'
    ) AS decision
FROM support_tickets;
```

Result: `decision` is a
`STRUCT(team VARCHAR, team_confidence DOUBLE, severity DOUBLE, severity_confidence DOUBLE, urgent DOUBLE)`:

| Field | Meaning |
| --- | --- |
| `team` | One of the four labels |
| `team_confidence` | Confidence in that label, from 0 to 1. NULL if the provider does not return one. |
| `severity` | Level index from 0 (`Routine question`) to 2 (`Work is blocked`) |
| `severity_confidence` | Confidence in that level |
| `urgent` | Probability that the answer is yes, from 0 to 1 |

With Ollama, this sends one request per ticket: 4 requests for the sample
table. The model is named explicitly because `ai_decide` ignores the session
setting `duckdb_ai_model`, which names a chat model.

## Choose a threshold and route each row

Pick a confidence threshold and give every row a route:

- `auto`: the decision model is confident, so use its answer.
- `fallback`: the decision model is unsure, so ask a chat model.
- `review`: the decision failed, so a person looks at it.

```sql
SET VARIABLE min_team_confidence = 0.8;

CREATE OR REPLACE TABLE route_assignments AS
SELECT
    ticket_id,
    decision.team AS team,
    decision.team_confidence AS team_confidence,
    decision.severity AS severity,
    decision.urgent AS urgent_probability,
    CASE
        WHEN decision IS NULL THEN 'review'
        WHEN decision.team_confidence >= getvariable('min_team_confidence') THEN 'auto'
        ELSE 'fallback'
    END AS route
FROM route_decisions;

SELECT route, count(*) AS tickets
FROM route_assignments
GROUP BY route
ORDER BY route;
```

A NULL `team_confidence` fails the comparison, so that row goes to `fallback`.
The threshold `0.8` is only a starting point. Confidence is the model's own
score, not a measured accuracy. Pick the threshold from a labeled sample, as
shown in [evaluate before a batch run](evaluate-before-batch.md).

## Send uncertain rows to a chat model

The `WHERE` clause runs before the model call, so only `fallback` rows reach
the chat model. `ai_extract_record` returns a typed `STRUCT`, and the JSON
Schema `enum` limits `team` to the same four labels. A reply outside the list
fails validation and, with `on_error := 'null'`, becomes NULL:

```sql
CREATE OR REPLACE TABLE route_fallback AS
SELECT
    ticket_id,
    answer.team AS team,
    answer.reason AS reason
FROM (
    SELECT
        r.ticket_id,
        ai_extract_record(
            'Choose the support team for this ticket and give a one-sentence reason.'
            || chr(10) || t.subject || chr(10) || t.body,
            '{
              "type": "object",
              "properties": {
                "team": {"type": "string", "enum": ["billing", "performance", "integration", "other"]},
                "reason": {"type": "string"}
              },
              "required": ["team", "reason"]
            }',
            provider := 'openai',
            model := 'gpt-5.6-luna',
            on_error := 'null'
        ) AS answer
    FROM route_assignments AS r
    JOIN support_tickets AS t USING (ticket_id)
    WHERE r.route = 'fallback'
);
```

Result: one row per fallback ticket with `team VARCHAR` and `reason VARCHAR`.
If you do not need the reason,
`ai_classify(text, ['billing', 'performance', 'integration', 'other'])` returns
the label alone.

## Combine the answers and fill the review table

Accept the confident decisions and the successful fallback answers:

```sql
CREATE OR REPLACE TABLE route_final AS
SELECT
    r.ticket_id,
    CASE WHEN r.route = 'auto' THEN r.team ELSE f.team END AS team,
    CASE WHEN r.route = 'auto' THEN 'decision model' ELSE 'chat fallback' END AS answered_by,
    r.severity,
    r.urgent_probability >= 0.5 AS urgent
FROM route_assignments AS r
LEFT JOIN route_fallback AS f USING (ticket_id)
WHERE r.route = 'auto'
   OR f.team IS NOT NULL;
```

Send everything else to people: failed decisions, failed fallbacks, and rows
whose urgency probability is close to 0.5. A row can be in both tables when its
team is settled but its urgency is not.

```sql
CREATE OR REPLACE TABLE route_review AS
SELECT
    r.ticket_id,
    r.route,
    r.team AS decision_team,
    r.team_confidence,
    r.urgent_probability,
    f.team AS fallback_team,
    f.reason AS fallback_reason
FROM route_assignments AS r
LEFT JOIN route_fallback AS f USING (ticket_id)
WHERE r.route = 'review'
   OR (r.route = 'fallback' AND f.team IS NULL)
   OR r.urgent_probability BETWEEN 0.4 AND 0.6;
```

To be stricter, also review fallback rows where `f.team` differs from the
decision model's low-confidence `r.team`.

## Check the calls

```sql
SELECT
    function_name,
    model,
    count(*) AS requests,
    count(*) FILTER (WHERE status <> 'ok') AS failures
FROM ai_usage()
WHERE function_name IN ('ai_decide', 'ai_extract_record')
GROUP BY ALL
ORDER BY function_name;
```

Result: one `ai_decide` request per non-NULL ticket, plus one
`ai_extract_record` request per fallback row. A failed request also shows its
message in the `error` column of `ai_usage()`.

## Limits

- **No capture mode.** `on_error` accepts only `'fail'` (the default) and
  `'null'`. A NULL `decision` does not say why: a NULL input also returns NULL.
  Read the reason from `ai_usage()` rows with `function_name = 'ai_decide'` and
  `status = 'error'`.
- **One request per row, except TypeSafe.** `ollama`, `cloudflare`,
  `perplexity` and `systemone` send one request per non-NULL row, run
  concurrently up to `max_concurrent_requests`. Only `typesafe` batches rows,
  up to 32 per request (`batch_size`). See [typed decisions](jev-decisions.md).
- **Question limits.** A choice takes 1 to 255 labels, and a score takes 2 to
  10 levels. Providers enforce their own caps on top of that: for example,
  Ollama accepts at most 26 options or levels and 64 questions per request.
  Split a large question set across two calls.
- **Constant questions.** Labels, levels and instructions must be constants,
  so they cannot change per row.
- **Model settings.** `ai_decide` ignores `duckdb_ai_model` and
  `duckdb_ai_base_url`. Set the model with `model :=` or an environment
  variable such as `OLLAMA_DECISION_MODEL`.
- **No generated text.** Generation options such as `temperature` are
  rejected, and the model cannot explain its answer. Use the chat fallback when
  you need a reason.

## Clean up

```sql
DROP TABLE IF EXISTS route_review;
DROP TABLE IF EXISTS route_final;
DROP TABLE IF EXISTS route_fallback;
DROP TABLE IF EXISTS route_assignments;
DROP TABLE IF EXISTS route_decisions;
```

## Related

- [Typed decisions](jev-decisions.md): save, filter and export decision fields,
  and batch rows with TypeSafe.
- [Evaluate before a batch run](evaluate-before-batch.md): measure accuracy and
  cost on a labeled sample before choosing a model or a threshold.
- [Chain several AI steps](chain-ai-steps.md): save each step and rerun only
  what is missing.
- [`ai_decide` reference](../functions.md#ai_decidestate-questions-)
