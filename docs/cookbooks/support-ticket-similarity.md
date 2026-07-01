---
sidebar_position: 4
---

# Compare support tickets with embeddings

Use this cookbook to find semantically similar support tickets from text columns.
`ai_similarity` embeds both inputs with the same model and returns cosine
similarity.

## Prerequisites

- Configure an embedding-capable provider.
- Create the [sample `support_tickets` table](support-ticket-data.md).

For OpenAI, use an embedding model such as `text-embedding-3-small`.

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

For repeated large workloads, store embeddings with `ai_embed` and compare those
vectors in a separate workflow instead of recomputing every pair.
