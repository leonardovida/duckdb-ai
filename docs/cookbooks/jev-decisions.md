---
sidebar_position: 15
title: "Turn rows into typed decisions"
sidebar_label: "Typed decisions"
description: "Classify, score and flag DuckDB table rows with ai_decide and TypeSafe Jev, and get typed STRUCT fields you can filter, aggregate and export without parsing JSON."
keywords: ["ai_decide", "ai_jev", "TypeSafe Jev", "LLM classification DuckDB", "decision model SQL"]
---

# Turn rows into typed decisions

Use `ai_decide` to classify, rate and flag table rows in one call. You write the
questions as SQL values and get a `STRUCT` back. Its fields work directly in
`SELECT`, `WHERE`, aggregations and Parquet exports, without parsing JSON.

This page uses TypeSafe Jev (`provider := 'typesafe'`), the only decision
provider that batches rows: up to 32 non-null rows per request by default.
`ai_jev(x, q)` is shorthand for `ai_decide(x, q, provider := 'typesafe')`. It
does not accept the `provider` or `profile` options or the
`{instructions, criteria}` question form.

:::tip Other decision models
The first three sections (configure, define, downstream SQL) also work with
another decision provider. Change `provider := 'typesafe'` to `'ollama'`
(Ollama 0.35 or later, after `ollama pull nimble`) or `'cloudflare'` (Clef),
and change or drop `model`. The batching section and the evaluator script apply
only to TypeSafe: other providers send one request per row, and `batch_size`
has no effect on them. See [decision models](../provider-guides.md#decision-models).
:::

## Configure Jev

Set `TYPESAFE_API_KEY` in the environment before starting DuckDB. Keep credentials
out of SQL literals and query history. The provider also supports
[`TYPE duckdb_ai` secrets](../provider-guides.md#typesafe-jev).

```sh
export TYPESAFE_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;
CREATE OR REPLACE TABLE decision_tickets AS
SELECT * FROM (VALUES
    (1, 'I was charged twice. Please refund the duplicate.'),
    (2, 'Production imports are blocked with no workaround.'),
    (3, NULL)
) AS t(id, body);
```

This page uses its own `decision_tickets` table, with a NULL row, instead of the
shared sample data.

## Define the decisions and save the results

Give each decision a field name and criteria. Descriptions define the decision,
so make them specific. A field name such as `urgent` is a SQL output name, not
an instruction to the model.

```sql
CREATE OR REPLACE TABLE ticket_decisions AS
SELECT id, ai_decide(body, {
    department: MAP {
        'billing': 'Payments, duplicate charges, invoices, refunds',
        'technical': 'Bugs, outages, data imports, integrations',
        'other': 'None of the above'
    },
    urgency: [
        'Routine question with no blocked work',
        'Degraded work with a workaround',
        'Production work is blocked with no workaround'
    ],
    refund_requested: MAP {
        'true': 'Explicitly asks for money to be returned',
        'false': 'Does not explicitly ask for money to be returned'
    }
}, provider := 'typesafe', model := 'jev-1.13.0') AS decision
FROM decision_tickets;
```

This statement calls Jev and can incur charges. Saving the result in a table
means the queries below read saved values instead of calling the provider again.
Use a persistent DuckDB database file to keep that table across sessions.

The result has this shape:

| Field | SQL type | Meaning |
| --- | --- | --- |
| `decision.department` | `VARCHAR` | One of the three declared labels |
| `decision.department_confidence` | `DOUBLE` | Confidence in the label, when the provider returns it |
| `decision.urgency` | `DOUBLE` | Weighted rubric position, here from 0 to 2 |
| `decision.urgency_confidence` | `DOUBLE` | Confidence in the score, when the provider returns it |
| `decision.refund_requested` | `DOUBLE` | Probability of yes, from 0 to 1 |

A NULL input produces a NULL `decision` without a provider call. For example,
ticket 3 remains in the saved table with a NULL decision. Model answers for the
other tickets depend on the model, so the field types and bounds are guaranteed,
not specific predictions.

## Use ordinary SQL downstream

Select individual fields and apply your own routing thresholds:

```sql
SELECT id, decision.department AS department,
       decision.urgency AS urgency,
       decision.refund_requested AS refund_probability
FROM ticket_decisions
WHERE decision.department_confidence >= 0.8;
```

Keep uncertain or missing results available for review:

```sql
SELECT id, decision
FROM ticket_decisions
WHERE decision IS NULL
   OR decision.department_confidence IS NULL
   OR decision.department_confidence < 0.8;
```

Count decisions or export a flat, typed dataset:

```sql
SELECT decision.department AS department, count(*) AS tickets
FROM ticket_decisions
GROUP BY ALL;

COPY (
    SELECT id, decision.* FROM ticket_decisions
) TO 'ticket_decisions.parquet' (FORMAT PARQUET);
```

The threshold `0.8` is illustrative. Tune it on labeled data, as in
[evaluate before a batch run](evaluate-before-batch.md), and see
[route rows by decision confidence](decision-routing.md) for a fallback when
confidence is low. A score is a rubric position, not a probability. A yes/no
answer (TypeSafe calls this question type a Noul) is the probability of yes,
which is also different from a choice confidence.

## Control batching and failures

This section applies to `provider := 'typesafe'` and `ai_jev` only.
`batch_size := 32` is the default row cap. Use `batch_size := 1` for a
one-row comparison. Batching happens within DuckDB execution chunks, so a query
may send several underfilled requests. NULL rows consume no request slots.

Each row becomes one question per declared decision. With the three decisions
above, 32 rows means 96 questions in one request. Every question carries its own
record in structured instructions. Shared state is empty, so unrelated rows
are not put into the context shared by every question.

`max_request_bytes` limits the serialized request body. The function splits a
batch before exceeding it, and rejects a row that cannot fit by itself. It never
truncates text. The byte limit is not a tokenizer or a guarantee that Jev's
context window is satisfied. Smaller batches and shorter relevant inputs are
the first remedies for a provider context-limit error.

By default a failed request or invalid answer fails the SQL query. Use
`on_error := 'null'` if you prefer a NULL result for affected rows. All rows in
a failed batch are affected. NULL alone does not distinguish a provider failure
from a NULL input, so keep the source text or check usage errors when that
matters. Retries can repeat the entire request. A later failed batch does not
undo earlier external calls or charges, even if SQL rolls back the table write.

Use the existing `ai_usage()` and `ai_usage_summary()` tables to inspect requests,
retries and token usage. Batching reduces initial HTTP requests, but does not
guarantee proportional cost or latency improvements. Data and criteria are
repeated per question, particularly when asking several decisions per row.

## Evaluate batch size on labeled data

Before choosing a batch size for a job, compare predictions on a representative
sample with human-reviewed labels. Use the source-checkout example
`examples/jev_batch_evaluation.py` for a single choice question. It calls
`ai_jev`, so it only measures TypeSafe. It compares batch sizes 1, 8, 16 and 32
using the same rows, criteria and explicit model. It does not evaluate score or
yes/no questions. In particular, a criteria map containing exactly `true` and
`false` is rejected because it selects yes/no mode. To measure accuracy and
cost with any provider in plain SQL, see
[evaluate before a batch run](evaluate-before-batch.md).

Create `tickets.csv` with unique, non-empty identifiers, text and expected labels:

```csv
id,text,label
1,I was charged twice. Please refund the duplicate.,billing
2,Production imports are blocked with no workaround.,technical
```

Create `criteria.json` with the allowed labels and their descriptions:

```json
{
  "billing": "Payments, duplicate charges, invoices, refunds",
  "technical": "Bugs, outages, data imports, integrations"
}
```

Configure `TYPESAFE_API_KEY` as above. The example requires explicit live-call opt-in because it sends the sample
four times and may incur charges:

```sh
python3 examples/jev_batch_evaluation.py \
  --duckdb ./build/release/duckdb \
  --input tickets.csv \
  --criteria criteria.json \
  --model jev-1.13.0 \
  --timeout-seconds 3600 \
  --allow-live > evaluation.json
```

The timeout applies to each batch-size run and defaults to one hour. Increase
it for slow endpoints or larger samples. If a later run fails or times out,
the command exits nonzero and writes the completed comparisons with
`complete: false` and an error. The interrupted run is not saved, and it may
already have incurred charges. Successful reports have `complete: true`.

The report retains predictions by identifier for inspection. Accuracy counts
missing predictions as incorrect. It also reports answer coverage, per-class
support, precision, recall, F1, and a confusion matrix with a separate failed
count for each actual class. Failures reduce recall and F1. Precision is `null`
when a class has no predictions; recall is `null` when it has no labeled examples.
Macro F1 averages the classes present in the labeled data, listed explicitly in
`macro_f1_labels`; unobserved classes remain visible in the per-class table.
These metrics compare predictions with the supplied ground-truth labels.
Agreement compares each run with batch size
1 among rows where both runs returned a prediction. Coverage counts show
how many rows each run answered. A missing prediction never counts as agreement.
Inspect coverage before interpreting agreement, since it excludes missing
pairs. Two rows are enough to check the workflow, but use a representative
sample larger than 32 rows to compare all four batch sizes.

The example runs each batch size in a fresh DuckDB process with one thread
and one provider request at a time, response caching disabled and no retries. It uses `on_error := 'null'` so a
failed request remains visible as missing predictions. Input is limited to
1,000 labeled rows to keep request operations within the usage buffer.
Request bodies can still split at the byte limit, so configured batch size is
an upper bound on rows per request. A row that cannot fit by itself is missing
without a provider request. If no requests are recorded at all, the command
fails with guidance to check credentials and request size.

Token usage is provider-reported. Totals remain unknown when an operation fails
or omits usage, with event counts indicating coverage. Missing usage is not
evidence of zero cost. Dropped usage events also make totals incomplete.
Wall time includes local process and SQL overhead, so it is not isolated API
latency. A single run does not establish a
speedup or prediction stability. Repeat comparisons and inspect disagreements
before adopting a batch size. Provider-side caching, service load and model
changes can affect results. The local mock test establishes the evaluator's
request accounting and metrics, not Jev's live quality or costs.

## Gate changes against labeled data

Supply acceptance thresholds to make the evaluator fail when any tested batch
size misses them:

```sh
python3 examples/jev_batch_evaluation.py --duckdb build/release/duckdb \
  --input labeled-tickets.csv --criteria criteria.json --model jev-1.13.0 \
  --allow-live --min-accuracy 0.95 --min-macro-f1 0.9 \
  --min-coverage 0.99 --min-class-recall 0.8
```

The JSON report contains `quality_gate`, including failing metrics and labels;
a failed gate exits with status 2. When gating is enabled, every criteria label
must have ground-truth examples by default. Repeated `--required-label` options
may explicitly select required classes. Use labels independent of the model's
training/teacher outputs, keep minority and out-of-domain cases, and group related
records when splitting datasets. Thresholds must reflect the application's error
costs. CI includes a hand-authored synthetic multilingual fixture that verifies
gate arithmetic and failure handling; it does not certify production model quality.

## Choose the appropriate interface

- Use `ai_decide` for typed choices, scores and yes/no probabilities across
  rows, with any decision provider. Add a per-question instruction with the
  `{instructions: '...', criteria: ...}` form. `ai_jev` is the TypeSafe
  shorthand.
- Keep `ai_classify` or `ai_filter` when you only have a chat provider, or for
  existing single-decision queries.
- Use `ai_provider_call` when you need full probability distributions, the
  resolved model version or shared state. It returns raw JSON and does not
  automatically combine separate SQL rows.
- Use a generative provider for summaries, free-text extraction and SQL
  generation. Keep arithmetic, date comparisons and counting in DuckDB.

See the [function reference](../functions.md#ai_decidestate-questions-) for
question types, options and validation rules. For multi-step pipelines that
save each step, see [chain several AI steps](chain-ai-steps.md). The local mock suite checks request counts, row
mapping, types and errors. It does not establish live prediction quality,
latency or token savings.

The compact typed-question interface was inspired by
[`colliber/duckdb-jev`](https://github.com/colliber/duckdb-jev). This extension uses
its own existing provider runtime and adds row batching. TypeSafe documents
[structured instructions](https://docs.typesafe.ai/primitives/advanced),
[model limits](https://docs.typesafe.ai/models) and
[known model limitations](https://docs.typesafe.ai/model-jaggedness/jev-1.13).

## Clean up

To remove the example, run `DROP TABLE IF EXISTS ticket_decisions, decision_tickets;`
and delete `ticket_decisions.parquet` from the directory where DuckDB was
started.
