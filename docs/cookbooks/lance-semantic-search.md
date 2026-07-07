---
sidebar_position: 5
---

# Store embeddings in Lance for semantic search

Use this cookbook when you want to reuse embeddings across searches, build a
vector index, or combine semantic search with full-text search. Keep
`ai_similarity` for small ad hoc comparisons; use the Lance extension when the
same embeddings should be stored, indexed, and queried repeatedly.

## Prerequisites

- Install and load both extensions.
- Configure an embedding-capable provider.
- Create the [sample `support_tickets` table](support-ticket-data.md).

```sql
INSTALL ai FROM community;
LOAD ai;

INSTALL lance;
LOAD lance;
```

The examples below use OpenAI `text-embedding-3-small` and cast embeddings to
`FLOAT[1536]`. Replace `1536` with the width returned by your embedding model.
Lance can search `FLOAT[]` and `DOUBLE[]` query vectors, but vector indexes need
a fixed-size vector column such as `FLOAT[1536]` or `DOUBLE[1536]`.

## Build an embedding staging table

Create embeddings once and keep the source text beside the vector. Use
`on_error := 'null'` when you want failed provider calls to be filtered out
instead of failing the whole batch.

```sql
CREATE OR REPLACE TABLE support_ticket_embedding_stage AS
SELECT
    ticket_id,
    customer_id,
    priority,
    status,
    subject,
    body,
    subject || chr(10) || body AS search_text,
    ai_embed(
        subject || chr(10) || body,
        provider := 'openai',
        model := 'text-embedding-3-small',
        on_error := 'null'
    ) AS embedding
FROM support_tickets;
```

Check for rejected rows before writing the Lance dataset:

```sql
SELECT count(*) AS rejected_rows
FROM support_ticket_embedding_stage
WHERE embedding IS NULL;
```

## Write a Lance dataset

Cast the embedding to a fixed-size vector before writing the dataset. This keeps
the Lance table ready for vector index creation.

```sql
COPY (
    SELECT
        ticket_id,
        customer_id,
        priority,
        status,
        subject,
        body,
        search_text,
        embedding::FLOAT[1536] AS embedding
    FROM support_ticket_embedding_stage
    WHERE embedding IS NOT NULL
) TO 'support_ticket_embeddings.lance' (
    FORMAT lance,
    mode 'overwrite'
);
```

You can also attach a Lance namespace when you want stable table names:

```sql
ATTACH './lance_data' AS lance_ns (TYPE LANCE);

CREATE OR REPLACE TABLE lance_ns.main.support_ticket_embeddings AS
SELECT
    ticket_id,
    customer_id,
    priority,
    status,
    subject,
    body,
    search_text,
    embedding::FLOAT[1536] AS embedding
FROM support_ticket_embedding_stage
WHERE embedding IS NOT NULL;
```

## Create search indexes

Create a vector index for semantic search. Start with a small
`num_partitions` value for small datasets, then tune it for larger tables.

```sql
CREATE INDEX support_ticket_embedding_idx
ON 'support_ticket_embeddings.lance' (embedding)
USING IVF_FLAT WITH (num_partitions = 1, metric_type = 'l2');
```

Add a full-text index when keyword matching should also influence retrieval:

```sql
CREATE INDEX support_ticket_text_idx
ON 'support_ticket_embeddings.lance' (search_text)
USING INVERTED;
```

Inspect the indexes:

```sql
SHOW INDEXES ON 'support_ticket_embeddings.lance';
```

## Search with a query embedding

Embed the search request into a DuckDB variable, then pass that vector to
`lance_vector_search`. Lance search parameters must be literals, prepared
parameters, or variable lookups rather than lateral columns from a CTE. Lance
returns `_distance`; smaller values are closer matches.

```sql
SET VARIABLE semantic_query = 'production incident blocking a business workflow';
SET VARIABLE semantic_query_embedding = (
    SELECT ai_embed(
        getvariable('semantic_query'),
        provider := 'openai',
        model := 'text-embedding-3-small'
    )::FLOAT[1536] AS embedding
);

SELECT
    result.ticket_id,
    result.subject,
    result.priority,
    result._distance
FROM lance_vector_search(
    'support_ticket_embeddings.lance',
    'embedding',
    getvariable('semantic_query_embedding'),
    k = 10,
    use_index = true,
    refine_factor = 2
) AS result
ORDER BY result._distance ASC;
```

## Rerank the shortlist

Use `ai_rerank` only on the small candidate set returned by Lance. This keeps
the expensive model-based scoring step bounded.

```sql
SET VARIABLE semantic_query = 'production incident blocking a business workflow';
SET VARIABLE semantic_query_embedding = (
    SELECT ai_embed(
        getvariable('semantic_query'),
        provider := 'openai',
        model := 'text-embedding-3-small'
    )::FLOAT[1536] AS embedding
);

WITH candidates AS (
    SELECT
        result.ticket_id,
        result.subject,
        result.body,
        result._distance
    FROM lance_vector_search(
        'support_ticket_embeddings.lance',
        'embedding',
        getvariable('semantic_query_embedding'),
        k = 20,
        use_index = true
    ) AS result
)
SELECT
    candidates.ticket_id,
    candidates.subject,
    candidates._distance,
    ai_rerank(
        getvariable('semantic_query'),
        candidates.subject || chr(10) || candidates.body,
        provider := 'openai',
        model := 'gpt-4o-mini'
    ) AS rerank_score
FROM candidates
ORDER BY rerank_score DESC, candidates._distance ASC
LIMIT 10;
```

## Combine vector and full-text search

Use Lance hybrid search when you want semantic similarity and keyword relevance
in the same retrieval step.

```sql
SET VARIABLE hybrid_query = 'billing outage after plan upgrade';
SET VARIABLE hybrid_query_embedding = (
    SELECT ai_embed(
        getvariable('hybrid_query'),
        provider := 'openai',
        model := 'text-embedding-3-small'
    )::FLOAT[1536] AS embedding
);

SELECT
    result.ticket_id,
    result.subject,
    result._hybrid_score,
    result._distance,
    result._score
FROM lance_hybrid_search(
    'support_ticket_embeddings.lance',
    'embedding',
    getvariable('hybrid_query_embedding'),
    'search_text',
    getvariable('hybrid_query'),
    k = 10,
    alpha = 0.7,
    oversample_factor = 4
) AS result
ORDER BY result._hybrid_score DESC;
```

Use a lower `alpha` when keyword matches should matter more, and a higher
`alpha` when vector similarity should dominate.

## Learn more

- [DuckDB Lance extension](https://duckdb.org/docs/current/core_extensions/lance)
- [Lance DuckDB SQL reference](https://github.com/lance-format/lance-duckdb/blob/main/docs/sql.md)
- [LanceDB DuckDB integration](https://docs.lancedb.com/integrations/data/duckdb)
