---
sidebar_position: 7
---

# Evaluate several decisions in one Jev request

Use TypeSafe's Jev when the outputs are known labels, yes/no probabilities, or
ratings against an ordered rubric. This recipe evaluates three ticket decisions
in one request, then applies routing rules in SQL.

## Prerequisites

Build a version of the extension containing TypeSafe support. Set
`TYPESAFE_API_KEY` in the environment before starting DuckDB, then load `ai`.
These examples use DuckDB's `json` extension for request construction and response
projection. Install and load `json` if your DuckDB distribution does not include it.

```sql
LOAD ai;
LOAD json;
CREATE OR REPLACE SECRET jev_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'typesafe'
);

CREATE TEMP TABLE jev_tickets AS
SELECT * FROM (VALUES
    (1, 'I was charged twice for order A-104. Please refund the duplicate.'),
    (2, 'Our production import has failed for three days and there is no workaround.')
) AS t(ticket_id, body);
```

## Ask all independent questions together

The native request contains the model, one shared `state`, and a map of named
`questions`. The request below uses a pinned model so a moving alias cannot
silently change a threshold-based workflow. Check TypeSafe's
[model list](https://docs.typesafe.ai/models) before choosing a version.

```sql
CREATE TEMP TABLE jev_evaluations AS
SELECT ticket_id,
    ai_provider_call(
        json_object(
            'model', 'jev-1.13.0',
            'state', json_object('ticket', body),
            'questions', '{
                "department": {
                    "type": "choice",
                    "instructions": "Which team should handle the ticket?",
                    "criteria": {
                        "billing": "Payments, duplicate charges, invoices, refunds",
                        "technical": "Bugs, outages, data imports, integrations",
                        "other": "None of the above"
                    }
                },
                "urgency": {
                    "type": "score",
                    "instructions": "How urgent is the issue described in the ticket?",
                    "criteria": [
                        "Routine question with no blocked work",
                        "Degraded work with a workaround",
                        "Production work is blocked with no workaround"
                    ]
                },
                "refund_requested": {
                    "type": "noul",
                    "instructions": "Does the ticket explicitly request a refund?"
                }
            }'::JSON
        )::VARCHAR,
        secret := 'jev_ai',
        max_concurrent_requests := 4
    ) AS response
FROM jev_tickets;
```

This makes **one initial HTTP request per ticket**, carrying three questions,
instead of three separate calls per ticket. Retries can add requests. Jev
processes the shared state once and evaluates the questions in parallel. Separate
SQL expressions are not automatically combined. The temporary table saves the
response so projecting several fields does not rerun inference.

## Keep the probabilities and route in SQL

```sql
WITH decisions AS (
    SELECT ticket_id,
        response::JSON ->> '$.model' AS model_version,
        response::JSON ->> '$.answers.department.choice' AS department,
        (response::JSON ->> '$.answers.department.confidence')::DOUBLE AS confidence,
        (response::JSON ->> '$.answers.urgency.score')::DOUBLE AS urgency,
        (response::JSON ->> '$.answers.refund_requested.noul')::DOUBLE AS refund_probability
    FROM jev_evaluations
)
SELECT *,
    CASE
        WHEN confidence IS NULL OR confidence < 0.8 THEN 'review'
        WHEN department = 'technical' AND urgency >= 1.5 THEN 'engineering'
        WHEN department = 'billing' AND refund_probability >= 0.9 THEN 'refund review'
        ELSE department
    END AS suggested_queue
FROM decisions;
```

Output includes the actual model version, selected department, confidence,
urgency score, refund probability, and suggested queue. The thresholds above are
illustrative. Tune them on labeled examples from your workload. A `noul` is the
probability of yes. A `score` is a probability-weighted position on the supplied
levels, here from 0 to 2, and is not a probability. Choice confidence is a
separate measure from the winning option's probability.

## Use the same pattern for other analytical work

| Workflow | State and questions | Work to keep in SQL |
| --- | --- | --- |
| Search reranking | Shortlist passages first, then ask one Noul per candidate: "Does passages[0] directly answer the query?" | Join answer IDs back to candidate IDs, sort by probability, and retain the original text. |
| Entity matching | Two product records, then separate questions for same product, same variant, and conflicting evidence | Generate candidates using exact keys or blocking rules. Keep unmatched and ambiguous pairs for review. |
| Document quality checks | A document plus separate questions for missing prerequisites, unsupported claims, and ambiguous instructions | Aggregate flags, preserve evidence text, and track changes across document versions. |
| Lightweight model routing | A request plus a Choice among known handlers and a Noul for whether the request fits any supported task | Send only uncertain or generative work to a larger model. Keep handler execution and authorization in application code. |

For candidate reranking, put the candidate's index or stable field reference in
its **instructions**. TypeSafe does not send question IDs to the model, so a key
such as `candidate_17` alone does not tell it which passage to evaluate.

## Reduce work before increasing concurrency

Filter rows and select only relevant text fields before inference. Deduplicate
identical input if the result can safely be reused. Bundle independent questions
against the same state, then persist the raw response for reuse. Increase
`max_concurrent_requests` gradually within your account's limits. Use
`ai_usage()` to inspect calls, latency, retries, tokens, and cache hits.

As of September 18, 2026, TypeSafe documents 64k tokens per request and 32k for
state plus the largest single question. These are token limits, not character
limits. Its published limits are 1,200 requests/minute and 250,000 tokens/second,
subject to change. Keep large candidate sets bounded and respect both budgets.
Bundling reduces repeated state and HTTP requests. Actual latency and accuracy
improvements require measurement on your data, network, and account.

Jev does not generate summaries, SQL, or arbitrary extracted strings. Keep exact
arithmetic, date comparisons, and counting in DuckDB. TypeSafe also documents
weaker performance on irrelevant long context and adversarial input. Typed
answers can still be wrong, so measure decision quality as well as speed.

Sources: [HTTP API](https://docs.typesafe.ai/api),
[parallel questions](https://docs.typesafe.ai/patterns/fan-out),
[confidence](https://docs.typesafe.ai/confidence), and
[known model limitations](https://docs.typesafe.ai/model-jaggedness/jev-1.13).
