---
sidebar_position: 4
title: "Evaluate a prompt or model on a labeled sample before a batch run"
sidebar_label: "Evaluate before a batch run"
description: "Compare prompts or models on a small labeled sample in DuckDB SQL: accuracy, per-label errors, confidence thresholds and estimated cost per correct answer, before paying for a full batch."
keywords: ["LLM evaluation SQL", "prompt evaluation", "model comparison DuckDB", "cost per correct answer", "ai_model_prices"]
---

# Evaluate a prompt or model on a labeled sample before a batch run

Use this cookbook before you run a prompt over a whole table. You label a small
sample by hand, run each candidate prompt or model on it, and compare accuracy,
mistakes and cost in SQL. Each run is saved in a table, so you pay for every
candidate once and can rerun the analysis for free.

## Prerequisites

- A completion provider. The examples use OpenAI `gpt-5.6-luna`, so set
  `OPENAI_API_KEY` in the environment before starting DuckDB. See the
  [provider guides](../provider-guides.md).
- Optional, for the decision-model candidate: Ollama 0.35 or later and
  `ollama pull nimble`.
- The [sample `support_tickets` table](support-ticket-data.md).
- A persistent database file, such as `duckdb eval.duckdb`, if you want to keep
  the results across sessions.

Turn on the built-in price list, so `ai_usage()` fills `estimated_cost_usd`.
Leave the response cache (`duckdb_ai_cache`) off, its default, so every
candidate pays for its own requests and the cost comparison is fair:

```sql
SET duckdb_ai_use_builtin_model_prices = true;
```

## Create the labeled sample

Write down the answer you expect for each row. The four sample tickets keep
this page short. For a real decision, label 50 to 200 rows that cover every
label and the hard cases you know about. Keep each candidate under 1,024 rows,
because `ai_usage()` keeps only the latest 1,024 events.

```sql
CREATE OR REPLACE TABLE eval_gold AS
SELECT *
FROM (
    VALUES
        (1001, 'integration'),
        (1002, 'billing'),
        (1003, 'documentation'),
        (1004, 'performance')
) AS t(ticket_id, expected_label);

CREATE OR REPLACE TABLE eval_sample AS
SELECT
    t.ticket_id,
    t.subject || chr(10) || t.body AS ticket_text,
    g.expected_label
FROM support_tickets AS t
JOIN eval_gold AS g USING (ticket_id);
```

## Run each candidate once and save it

Change one thing per candidate, such as the prompt or the model, so you know
what caused a difference. For each candidate:

1. Clear the usage buffer.
2. Save the predictions with `CREATE TABLE IF NOT EXISTS ... AS`. If the table
   already exists, DuckDB skips the statement and makes no calls, so rerunning
   the block never pays twice.
3. Save that candidate's usage events.

Candidate A sends the labels only:

```sql
SELECT * FROM ai_clear_usage();

CREATE TABLE IF NOT EXISTS eval_run_a AS
SELECT
    'luna-labels-only' AS candidate,
    ticket_id,
    ai_classify(
        ticket_text,
        ['billing', 'performance', 'integration', 'documentation', 'other'],
        provider := 'openai',
        model := 'gpt-5.6-luna',
        on_error := 'null'
    ) AS predicted_label
FROM eval_sample;

CREATE TABLE IF NOT EXISTS eval_usage_a AS
SELECT 'luna-labels-only' AS candidate, *
FROM ai_usage();
```

Candidate B keeps the model and changes the prompt. It adds a description per
label and one instruction:

```sql
SELECT * FROM ai_clear_usage();

CREATE TABLE IF NOT EXISTS eval_run_b AS
SELECT
    'luna-described' AS candidate,
    ticket_id,
    ai_classify(
        ticket_text,
        ['billing', 'performance', 'integration', 'documentation', 'other'],
        label_descriptions := '{
          "billing": "invoices, payments, refunds, billing contacts",
          "performance": "slow queries, timeouts, regressions",
          "integration": "exports, imports, connections to other tools",
          "documentation": "requests for examples or docs",
          "other": "anything else"
        }',
        instructions := 'The ticket may be in any language.',
        provider := 'openai',
        model := 'gpt-5.6-luna',
        on_error := 'null'
    ) AS predicted_label
FROM eval_sample;

CREATE TABLE IF NOT EXISTS eval_usage_b AS
SELECT 'luna-described' AS candidate, *
FROM ai_usage();
```

To compare models instead, copy candidate A, give it a new candidate name and
table names, and change only `model :=`, for example to `gpt-5.4-mini`.

Candidate C is optional. It uses a decision model, which also returns a
confidence for each label, so you can test thresholds later:

```sql
SELECT * FROM ai_clear_usage();

CREATE TABLE IF NOT EXISTS eval_run_c AS
SELECT
    'nimble-decision' AS candidate,
    ticket_id,
    d.label AS predicted_label,
    d.label_confidence AS confidence
FROM (
    SELECT
        ticket_id,
        ai_decide(
            ticket_text,
            {label: MAP {
                'billing': 'Invoices, payments, refunds, billing contacts',
                'performance': 'Slow queries, timeouts, regressions',
                'integration': 'Exports, imports, connections to other tools',
                'documentation': 'Requests for examples or docs',
                'other': 'Anything else'
            }},
            provider := 'ollama',
            model := 'nimble',
            on_error := 'null'
        ) AS d
    FROM eval_sample
);

CREATE TABLE IF NOT EXISTS eval_usage_c AS
SELECT 'nimble-decision' AS candidate, *
FROM ai_usage();
```

Combine the saved runs. If you skipped candidate C, remove its two lines:

```sql
CREATE OR REPLACE VIEW eval_predictions AS
SELECT * FROM eval_run_a
UNION ALL BY NAME
SELECT * FROM eval_run_b
UNION ALL BY NAME
SELECT * FROM eval_run_c;

CREATE OR REPLACE VIEW eval_usage AS
SELECT * FROM eval_usage_a
UNION ALL BY NAME
SELECT * FROM eval_usage_b
UNION ALL BY NAME
SELECT * FROM eval_usage_c;
```

Every query below reads these saved tables and makes no provider calls.

## Compare accuracy

A failed call leaves `predicted_label` NULL and counts as wrong:

```sql
SELECT
    p.candidate,
    count(*) AS sample_rows,
    count(*) FILTER (WHERE p.predicted_label = s.expected_label) AS correct,
    count(*) FILTER (WHERE p.predicted_label IS NULL) AS failed,
    round(count(*) FILTER (WHERE p.predicted_label = s.expected_label) / count(*), 3) AS accuracy
FROM eval_predictions AS p
JOIN eval_sample AS s USING (ticket_id)
GROUP BY p.candidate
ORDER BY accuracy DESC;
```

Result: one row per candidate with `accuracy DOUBLE` from 0 to 1.

## Find the errors per label

Overall accuracy can hide a label that always fails. Count the correct answers
per expected label:

```sql
SELECT
    p.candidate,
    s.expected_label,
    count(*) AS sample_rows,
    count(*) FILTER (WHERE p.predicted_label = s.expected_label) AS correct
FROM eval_predictions AS p
JOIN eval_sample AS s USING (ticket_id)
GROUP BY ALL
ORDER BY s.expected_label, p.candidate;
```

Then read the mistakes themselves:

```sql
SELECT
    p.candidate,
    s.ticket_id,
    s.expected_label,
    p.predicted_label,
    s.ticket_text
FROM eval_predictions AS p
JOIN eval_sample AS s USING (ticket_id)
WHERE p.predicted_label IS DISTINCT FROM s.expected_label
ORDER BY s.ticket_id, p.candidate;
```

## Estimate the cost per correct answer

`estimated_cost_usd` comes from `ai_model_prices()`. Local models and models
without a price row get NULL, so the query also counts unpriced requests.
Events with unknown token counts report a NULL `total_tokens`, which `sum()`
skips.

```sql
WITH quality AS (
    SELECT
        p.candidate,
        count(*) AS sample_rows,
        count(*) FILTER (WHERE p.predicted_label = s.expected_label) AS correct
    FROM eval_predictions AS p
    JOIN eval_sample AS s USING (ticket_id)
    GROUP BY p.candidate
),
cost AS (
    SELECT
        candidate,
        count(*) AS requests,
        sum(total_tokens) AS known_tokens,
        sum(estimated_cost_usd) AS cost_usd,
        count(*) FILTER (WHERE estimated_cost_usd IS NULL) AS unpriced_requests,
        avg(elapsed_ms) AS avg_elapsed_ms
    FROM eval_usage
    GROUP BY candidate
)
SELECT
    q.candidate,
    round(q.correct / q.sample_rows, 3) AS accuracy,
    c.requests,
    c.known_tokens,
    c.cost_usd,
    c.cost_usd / nullif(q.correct, 0) AS cost_per_correct_usd,
    c.cost_usd / q.sample_rows * 100000 AS est_cost_per_100k_rows_usd,
    c.unpriced_requests,
    c.avg_elapsed_ms
FROM quality AS q
JOIN cost AS c USING (candidate)
ORDER BY cost_per_correct_usd NULLS LAST;
```

`est_cost_per_100k_rows_usd` scales the sample cost linearly. It is a rough
guide: longer rows in the full table cost more. Check the full table's size
with `ai_count_tokens` as in
[production batch enrichment](production-batch-enrichment.md#estimate-a-starting-batch-size).

## Pick a confidence threshold

Candidate C stored a confidence per row. For each threshold, see how many rows
you would accept automatically and how accurate those rows are:

```sql
SELECT
    t.threshold,
    count(*) FILTER (WHERE p.confidence >= t.threshold) AS accepted_rows,
    count(*) FILTER (WHERE p.confidence >= t.threshold) / count(*) AS accepted_share,
    count(*) FILTER (WHERE p.confidence >= t.threshold AND p.predicted_label = s.expected_label)
        / nullif(count(*) FILTER (WHERE p.confidence >= t.threshold), 0) AS accepted_accuracy
FROM eval_run_c AS p
JOIN eval_sample AS s USING (ticket_id)
CROSS JOIN (VALUES (0.5), (0.7), (0.8), (0.9)) AS t(threshold)
GROUP BY t.threshold
ORDER BY t.threshold;
```

A higher threshold usually raises `accepted_accuracy` and lowers
`accepted_share`. The rows below the threshold go to a fallback or to review,
as in [route rows by decision confidence](decision-routing.md).

## Decide

1. Drop every candidate whose accuracy, or accuracy for an important label, is
   below what the job needs.
2. From the rest, pick the lowest `cost_per_correct_usd`.
3. Run the batch with exactly that prompt and model, and record the candidate
   name with the output, for example as the `pipeline_version` in
   [chain several AI steps](chain-ai-steps.md).

To test a new idea later, add a candidate D with its own tables and add it to
the two views. Candidates A to C are not called again.

## Clean up

```sql
DROP VIEW IF EXISTS eval_predictions;
DROP VIEW IF EXISTS eval_usage;
DROP TABLE IF EXISTS eval_run_a;
DROP TABLE IF EXISTS eval_run_b;
DROP TABLE IF EXISTS eval_run_c;
DROP TABLE IF EXISTS eval_usage_a;
DROP TABLE IF EXISTS eval_usage_b;
DROP TABLE IF EXISTS eval_usage_c;
DROP TABLE IF EXISTS eval_sample;
DROP TABLE IF EXISTS eval_gold;
```

## Related

- [Typed decisions](jev-decisions.md) includes a TypeSafe evaluator script that
  also reports precision, recall and F1 per label.
- [Monitor AI usage, failures, and cost](usage-cost-observability.md)
- [`ai_model_prices()` reference](../functions.md#ai_model_prices)
