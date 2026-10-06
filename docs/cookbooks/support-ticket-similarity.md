---
sidebar_position: 12
title: "Compare support tickets with embeddings"
sidebar_label: "Similarity with embeddings"
description: "Find semantically similar rows in DuckDB with ai_embed, ai_similarity and list_cosine_similarity, then rerank a short list with ai_rerank."
keywords: ["DuckDB embeddings", "ai_similarity", "list_cosine_similarity", "semantic similarity SQL"]
---

# Compare support tickets with embeddings

Use this cookbook to find semantically similar support tickets from text columns.
`ai_similarity` embeds both inputs with the same model and returns cosine
similarity as a `DOUBLE` from -1 to 1.

## Prerequisites

- Configure an embedding-capable provider. The examples use OpenAI
  `text-embedding-3-small`, so set `OPENAI_API_KEY` in the environment before
  starting DuckDB. See the [provider guides](../provider-guides.md).
- Create the [sample `support_tickets` table](support-ticket-data.md).

## Rank ticket pairs

Compare every ticket pair and rank the closest matches:

```sql
SELECT
    left_ticket.ticket_id AS left_ticket_id,
    right_ticket.ticket_id AS right_ticket_id,
    ai_similarity(
        left_ticket.subject || chr(10) || left_ticket.body,
        right_ticket.subject || chr(10) || right_ticket.body,
        provider := 'openai',
        model := 'text-embedding-3-small'
    ) AS similarity
FROM support_tickets AS left_ticket
JOIN support_tickets AS right_ticket
    ON left_ticket.ticket_id < right_ticket.ticket_id
ORDER BY similarity DESC;
```

## Compare against a target description

Use a fixed description when you want to find rows that match a theme:

```sql
SELECT
    ticket_id,
    subject,
    ai_similarity(
        subject || chr(10) || body,
        'production incident blocking an important business workflow',
        provider := 'openai',
        model := 'text-embedding-3-small'
    ) AS similarity
FROM support_tickets
ORDER BY similarity DESC;
```

## Keep batches bounded

Pairwise comparison grows quickly. For larger tables, filter first:

```sql
SELECT
    left_ticket.ticket_id AS left_ticket_id,
    right_ticket.ticket_id AS right_ticket_id,
    ai_similarity(
        left_ticket.subject || chr(10) || left_ticket.body,
        right_ticket.subject || chr(10) || right_ticket.body,
        provider := 'openai',
        model := 'text-embedding-3-small'
    ) AS similarity
FROM support_tickets AS left_ticket
JOIN support_tickets AS right_ticket
    ON left_ticket.ticket_id < right_ticket.ticket_id
WHERE left_ticket.priority = 'high'
   OR right_ticket.priority = 'high'
ORDER BY similarity DESC;
```

## Store embeddings once

`ai_similarity` embeds both of its inputs on every call, so each run of the
queries above pays for the embeddings again. When you search the same rows more
than once, embed each row once, save the vectors in a table, and compare them
with DuckDB's `list_cosine_similarity`. That comparison runs locally and costs
nothing.

Embed every ticket once. `ai_embed` returns a `DOUBLE[]`. With
`on_error := 'null'`, a failed row gets a NULL embedding instead of failing the
statement:

```sql
CREATE OR REPLACE TABLE ticket_embeddings AS
SELECT
    ticket_id,
    subject,
    body,
    ai_embed(
        subject || chr(10) || body,
        provider := 'openai',
        model := 'text-embedding-3-small',
        on_error := 'null'
    ) AS embedding
FROM support_tickets;
```

Embed the search text once and keep it in a variable:

```sql
SET VARIABLE ticket_query = 'production incident blocking a business workflow';
SET VARIABLE ticket_query_embedding = (
    SELECT ai_embed(
        getvariable('ticket_query'),
        provider := 'openai',
        model := 'text-embedding-3-small'
    )
);
```

Rank the stored vectors. This query makes no provider calls, so you can rerun
it as often as you like:

```sql
SELECT
    ticket_id,
    subject,
    list_cosine_similarity(embedding, getvariable('ticket_query_embedding')) AS similarity
FROM ticket_embeddings
WHERE embedding IS NOT NULL
ORDER BY similarity DESC
LIMIT 3;
```

Result: the top 3 tickets with `similarity DOUBLE`. Both vectors must come
from the same embedding model, because vectors of different lengths cannot be
compared.

The same table also replaces the pairwise query, with no extra calls:

```sql
SELECT
    a.ticket_id AS left_ticket_id,
    b.ticket_id AS right_ticket_id,
    list_cosine_similarity(a.embedding, b.embedding) AS similarity
FROM ticket_embeddings AS a
JOIN ticket_embeddings AS b
    ON a.ticket_id < b.ticket_id
ORDER BY similarity DESC;
```

### Rerank the short list

Embedding similarity is cheap but coarse. To reorder the best few matches,
call `ai_rerank` on the short list only. The `LIMIT` sits inside the subquery,
so the completion model is called for 3 rows, not for the whole table:

```sql
SELECT
    ticket_id,
    subject,
    similarity,
    ai_rerank(
        getvariable('ticket_query'),
        subject || chr(10) || body,
        provider := 'openai',
        model := 'gpt-5.6-luna'
    ) AS rerank_score
FROM (
    SELECT
        ticket_id,
        subject,
        body,
        list_cosine_similarity(embedding, getvariable('ticket_query_embedding')) AS similarity
    FROM ticket_embeddings
    WHERE embedding IS NOT NULL
    ORDER BY similarity DESC
    LIMIT 3
)
ORDER BY rerank_score DESC;
```

Result: `rerank_score DOUBLE` from 0 to 1, one completion call per short-list row.

`list_cosine_similarity` scans every stored vector, which is fine for small and
medium tables. For a vector index, full-text search, or larger tables, write
the same embeddings to Lance as shown in
[semantic search with Lance](lance-semantic-search.md).

Remove the example table when you are done:

```sql
DROP TABLE IF EXISTS ticket_embeddings;
```
