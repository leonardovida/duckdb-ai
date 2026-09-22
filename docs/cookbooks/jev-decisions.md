---
sidebar_position: 7
---

# Turn rows into typed Jev decisions

Use `ai_jev` to classify, rate and flag table rows in one call. You write the
criteria as SQL values and get a `STRUCT` back. Its fields work directly in
`SELECT`, `WHERE`, aggregations and Parquet exports, without parsing JSON.

The function batches up to 32 non-null rows per request by default. It requires
a build containing `ai_jev`. Older builds can use `ai_classify`, `ai_filter` or
[the native JSON API](../functions.md#native-provider-json).

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
CREATE TABLE tickets AS
SELECT * FROM (VALUES
    (1, 'I was charged twice. Please refund the duplicate.'),
    (2, 'Production imports are blocked with no workaround.'),
    (3, NULL)
) AS t(id, body);
```

## Define the decisions and save the results

Give each decision a field name and criteria. Descriptions define the decision,
so make them specific. A field name such as `urgent` is a SQL output name, not
an instruction to the model.

```sql
CREATE TABLE ticket_decisions AS
SELECT id, ai_jev(body, {
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
}, model := 'jev-1.13.0') AS decision
FROM tickets;
```

This statement calls Jev and can incur charges. Saving the result in a table
means the queries below read saved values instead of calling the provider again.
Use a persistent DuckDB database file to keep that table across sessions.

The result has this shape:

| Field | SQL type | Meaning |
| --- | --- | --- |
| `decision.department` | `VARCHAR` | One of the three declared labels |
| `decision.department_confidence` | `DOUBLE` | Choice confidence when provided |
| `decision.urgency` | `DOUBLE` | Weighted rubric position, here from 0 to 2 |
| `decision.urgency_confidence` | `DOUBLE` | Score confidence when provided |
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

The threshold `0.8` is illustrative. Tune it on labeled data. A score is a
rubric position, not a probability. A Noul is the probability of yes, which is
also different from Choice confidence.

## Control batching and failures

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

## Choose the appropriate interface

- Use `ai_jev` for typed choices, scores and yes/no probabilities across rows.
- Keep `ai_classify` or `ai_filter` for existing single-decision queries. Their
  public behavior remains unchanged.
- Use `ai_provider_call` when you need full probability distributions, the
  resolved model version, custom question instructions or shared state. It
  returns raw JSON and does not automatically combine separate SQL rows.
- Use a generative provider for summaries, free-text extraction and SQL
  generation. Keep arithmetic, date comparisons and counting in DuckDB.

See the [function reference](../functions.md#ai_jevtext-questions-) for criteria,
options and validation rules. The local mock suite checks request counts, row
mapping, types and errors. It does not establish live prediction quality,
latency or token savings.

The compact typed-question interface was inspired by
[`colliber/duckdb-jev`](https://github.com/colliber/duckdb-jev). This extension uses
its own existing provider runtime and adds row batching. TypeSafe documents
[structured instructions](https://docs.typesafe.ai/primitives/advanced),
[model limits](https://docs.typesafe.ai/models) and
[known model limitations](https://docs.typesafe.ai/model-jaggedness/jev-1.13).
