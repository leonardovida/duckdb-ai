---
sidebar_position: 6
---

# Generate read-only SQL over local tables

Use this cookbook when you want a model to write DuckDB `SELECT` statements from
local table context. The SQL assistant functions validate that generated SQL is
one parser-valid read-only `SELECT` before returning or executing it.

## Prerequisites

- Configure a completion provider.
- Create the [sample `support_tickets` table](support-ticket-data.md).

## Inspect the schema prompt

Start by checking what table context the model will see:

```sql
SELECT summary
FROM ai_schema_prompt(
    include_tables := ['main.support_tickets'],
    sample_rows := 3
);
```

The context includes table names, column names, types, and bounded sample rows.

## Generate SQL without running it

Use `ai_sql` when you want to inspect or store the generated query:

```sql
SELECT ai_sql(
    'Which high-priority customers have the lowest satisfaction scores?',
    include_tables := ['main.support_tickets'],
    sample_rows := 3
) AS generated_sql;
```

## Generate and run SQL

Use `ai_query_data` when you want the generated query to run immediately:

```sql
SELECT *
FROM ai_query_data(
    'Count tickets by priority and customer tier',
    include_tables := ['main.support_tickets'],
    sample_rows := 3
);
```

## Keep context scoped

Limit `include_tables` to the tables relevant to the question. For a larger
database, this keeps the prompt smaller and reduces the chance that the model
uses the wrong table.

```sql
SELECT ai_sql(
    'Show the average satisfaction score by channel for high priority tickets',
    include_tables := ['main.support_tickets'],
    sample_rows := 2
);
```

## Validate generated SQL yourself

If you store generated SQL before running it, validate it again at the boundary:

```sql
WITH generated AS (
    SELECT ai_sql(
        'Count tickets by customer tier',
        include_tables := ['main.support_tickets'],
        sample_rows := 3
    ) AS sql_text
)
SELECT ai_validate_read_only_sql(sql_text) AS validated_sql
FROM generated;
```
