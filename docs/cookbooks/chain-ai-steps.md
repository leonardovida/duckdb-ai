---
sidebar_position: 3
title: "Chain several AI steps in SQL"
sidebar_label: "Chain AI steps"
description: "Run multi-step LLM pipelines in DuckDB SQL: avoid repeated model calls, skip rows that failed upstream, branch with CASE, and checkpoint each step so reruns only retry what is missing."
keywords: ["DuckDB LLM pipeline", "chain AI functions", "multi-step enrichment", "ai_try_complete", "ai_decide", "resumable SQL"]
---

# Chain several AI steps in SQL

Use this cookbook when one model call is not enough: for example, translate
a ticket, then triage it, then write a note for the urgent ones. Every step is
plain SQL, and every step is saved, so a rerun only calls the model for rows
that are new, changed, or failed last time.

Prerequisites:

- Steps 1 and 3 use the completion provider and model from your session
  settings (see the [quickstart](../index.md#first-query)). For a hosted
  provider, set its key in the environment, for example `OPENAI_API_KEY`.
- Step 2 uses a different model: the local Ollama decision model `nimble`
  through `ai_decide`. It needs Ollama 0.35 or later and `ollama pull nimble`.
  `ai_decide` ignores the session model settings, so the chat model from steps
  1 and 3 is never sent to it.

## How chained calls behave

These behaviors are covered by the extension's tests, so you can rely on them:

| Pattern | Model calls |
| --- | --- |
| A NULL input to any model function | None. The result is NULL. |
| A step reads only rows where the previous step succeeded | Only those rows. Failed rows are never sent again downstream. |
| `CASE WHEN <condition> THEN ai_...(x) END` | Only rows where the condition is true. |
| `WHERE <condition>` before a model function in `SELECT` | Only rows that pass the filter. |
| Reading several fields of a call from a subquery: `SELECT r.a, r.b FROM (SELECT ai_...(x) AS r ...)` | One per row. |

Avoid these patterns:

| Pattern | Problem | Write instead |
| --- | --- | --- |
| `(ai_try_complete(x)).response, (ai_try_complete(x)).error` | Two calls per row. | Call once in a subquery and read both fields outside it. |
| `SELECT ai_redact(x) AS clean, ai_summarize(clean)` | Binder error: an alias with side effects cannot be reused in the same `SELECT`. | Compute `clean` in a subquery or an earlier step table. |
| `ai_summarize(coalesce(x, ''))` | An empty string still sends a paid request (only `ai_complete` raises an error for it). | Pass NULL through. NULL inputs make no call. |
| `... LIMIT 100 OFFSET 1000` on the outer query | `OFFSET` can still call the model for the skipped rows. | Page inside the input subquery. |
| Rerunning a `CREATE OR REPLACE TABLE ... AS SELECT ai_...` | Every row is sent again. | Insert only rows missing from the step table, as below. |

## Combine steps before chaining them

Every step costs one request per row, so first check whether one call can do
the work of several:

- [`ai_decide`](../functions.md#ai_decidestate-questions-) answers several
  typed questions (team, urgency, sentiment) in one request instead of separate
  `ai_classify`, `ai_filter` and `ai_score` calls. See
  [typed decisions](jev-decisions.md) and
  [routing by confidence](decision-routing.md).
- `ai_extract_record` with a multi-field JSON Schema returns a category, a
  score and a short summary in one call.

Keep a step separate when its input must be different: for example, run
`ai_redact` on its own so that raw text never reaches a general model.

## Create sample data

This page uses its own small `tickets` table with a non-English row and a NULL
body, so you can see both cases being handled.

```sql
CREATE OR REPLACE TABLE tickets AS
SELECT * FROM (VALUES
    (1, 'Le paiement a été débité deux fois.'),
    (2, 'The dashboard is down for every user since 9am.'),
    (3, 'Can you update the billing email on our account?'),
    (4, NULL)
) AS t(ticket_id, body);

SET VARIABLE pipeline_version = 'triage-v1';
```

`pipeline_version` names the prompts and models in use. Change it when you
change a prompt or a model, and the next run recomputes every row.

## Step 1: normalize the text, keeping errors

Each step table stores its output and the error, keyed by row, input hash and
pipeline version, so a changed ticket or a new version is recomputed in every
step. The input subquery selects only rows without a successful
result, so a rerun retries failures and skips finished rows. `LIMIT` stays
inside the input subquery to bound each run.

```sql
CREATE TABLE IF NOT EXISTS step_normalize (
    ticket_id BIGINT,
    input_hash VARCHAR,
    version VARCHAR,
    english VARCHAR,
    error VARCHAR,
    PRIMARY KEY (ticket_id, input_hash, version)
);

INSERT OR REPLACE INTO step_normalize
SELECT ticket_id, input_hash, getvariable('pipeline_version'), result.response, result.error
FROM (
    SELECT ticket_id,
           md5(body) AS input_hash,
           ai_try_complete('Translate this support ticket to plain English. Return only the text: ' || body,
                           max_tokens := 200) AS result
    FROM (
        SELECT t.*
        FROM tickets t
        ANTI JOIN (SELECT * FROM step_normalize WHERE error IS NULL) done
            ON done.ticket_id = t.ticket_id
           AND done.input_hash = md5(t.body)
           AND done.version = getvariable('pipeline_version')
        WHERE t.body IS NOT NULL
        ORDER BY t.ticket_id
        LIMIT 500
    )
);
```

## Step 2: answer several questions in one call

`ai_decide` reads only rows that step 1 translated successfully, and asks two
questions in a single request per row. `team` comes back as a label with
`team_confidence`, and `urgent` as the probability of yes. With
`on_error := 'null'`, a failed request leaves `team` NULL, so the next run
retries that row.

```sql
CREATE TABLE IF NOT EXISTS step_triage (
    ticket_id BIGINT,
    input_hash VARCHAR,
    version VARCHAR,
    team VARCHAR,
    team_confidence DOUBLE,
    urgent DOUBLE,
    PRIMARY KEY (ticket_id, input_hash, version)
);

INSERT OR REPLACE INTO step_triage
SELECT ticket_id, input_hash, version, d.team, d.team_confidence, d.urgent
FROM (
    SELECT n.ticket_id, n.input_hash, n.version,
           ai_decide(n.english, {
               team: MAP {'billing': 'Payments, invoices and refunds',
                          'technical': 'Errors, outages and slow queries',
                          'other': 'Anything else'},
               urgent: MAP {'true': 'Needs action today', 'false': 'Can wait'}
           }, provider := 'ollama', on_error := 'null') AS d
    FROM step_normalize n
    ANTI JOIN (SELECT * FROM step_triage WHERE team IS NOT NULL) done
        USING (ticket_id, input_hash, version)
    WHERE n.error IS NULL AND n.version = getvariable('pipeline_version')
);
```

## Step 3: call a model only where it is needed

The filter runs before the model call, so only urgent technical tickets are
summarized.

```sql
CREATE TABLE IF NOT EXISTS step_escalation (
    ticket_id BIGINT,
    input_hash VARCHAR,
    version VARCHAR,
    note VARCHAR,
    PRIMARY KEY (ticket_id, input_hash, version)
);

INSERT OR REPLACE INTO step_escalation
SELECT ticket_id, input_hash, version, ai_summarize(english, on_error := 'null')
FROM (
    SELECT n.ticket_id, n.input_hash, n.version, n.english
    FROM step_triage tr
    JOIN step_normalize n USING (ticket_id, input_hash, version)
    ANTI JOIN (SELECT * FROM step_escalation WHERE note IS NOT NULL) done
        USING (ticket_id, input_hash, version)
    WHERE n.error IS NULL
      AND tr.version = getvariable('pipeline_version')
      AND tr.team = 'technical'
      AND tr.urgent >= 0.5
);
```

## Read the results and check progress

The first query joins the step tables back to the current ticket text, so a
ticket whose body changed shows only its latest result. The second counts the
model calls of this run by function.

```sql
SELECT n.ticket_id, n.english, tr.team, tr.urgent, e.note, n.error
FROM step_normalize n
LEFT JOIN step_triage tr USING (ticket_id, input_hash, version)
LEFT JOIN step_escalation e USING (ticket_id, input_hash, version)
SEMI JOIN tickets t
    ON t.ticket_id = n.ticket_id
   AND md5(t.body) = n.input_hash
WHERE n.version = getvariable('pipeline_version')
ORDER BY n.ticket_id;

SELECT function_name, count(*) AS calls, count(*) FILTER (status <> 'ok') AS failures,
       sum(total_tokens) AS tokens
FROM ai_usage()
GROUP BY function_name
ORDER BY function_name;
```

Run the same statements again to continue. Rows that already succeeded are not
sent again, so a second run only retries failed rows and picks up new tickets.
`ai_usage()` lives in memory for the current DuckDB instance and keeps only the
latest 1,024 events. Copy it into a table after each run if you need cost
history, as in [usage and cost monitoring](usage-cost-observability.md).

## Options per step

- Shared options, such as the provider, timeouts and retries, belong in
  session settings (`SET duckdb_ai_retry_count = 2`).
- Options that differ by step go on the call. To change them without editing
  the query, read them from a variable: run `SET VARIABLE summary_tokens = 120;`
  first, then pass `max_tokens := getvariable('summary_tokens')`. An unset
  variable is NULL, and `max_tokens` rejects NULL.
- `CREATE EXTERNAL MODEL` profiles bundle a provider, model and credential under
  one name, so each step can use `profile := 'small_model'` or
  `profile := 'large_model'`.

For a Python driver that loops over batches and stops on a budget, see
[resumable enrichment](resumable-enrichment.md). Before a large run, check the
prompts and models on a labeled sample as shown in
[evaluate before a batch run](evaluate-before-batch.md).

## Clean up

The step tables are meant to persist between runs. To start over, or to remove
the example, run `DROP TABLE IF EXISTS step_escalation, step_triage,
step_normalize, tickets;` in the same database.
