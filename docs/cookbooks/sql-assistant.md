---
sidebar_position: 17
title: "Generate read-only SQL over local tables"
sidebar_label: "Text-to-SQL over local tables"
description: "Generate and run read-only DuckDB SELECT statements from natural-language questions with ai_sql and ai_query_data."
keywords: ["DuckDB text-to-SQL", "natural language to SQL", "ai_sql", "ai_query_data"]
---

# Generate read-only SQL over local tables

Use this cookbook when you want a model to write DuckDB `SELECT` statements from
local table context. The SQL assistant functions check that the generated SQL is
one parser-valid, read-only `SELECT` before returning or running it.

| Function | Result |
| --- | --- |
| `ai_schema_prompt(...)` | One row with `summary VARCHAR`: the table context sent to the model. No provider call. |
| `ai_sql(question, ...)` | `VARCHAR` with the generated `SELECT`. It does not run the query. |
| `ai_query_data(question, ...)` | The columns of the generated query, after running it. |

## Prerequisites

- Configure a completion provider. Hosted providers read their key from the
  environment, for example `OPENAI_API_KEY`. See the
  [provider guides](../provider-guides.md).
- Create the [sample `support_tickets` table](support-ticket-data.md).

## Decide what the model may see

The model receives table names, column names and types. With
`sample_rows := N`, it also receives up to N real rows from each table. In the
sample data, `internal_note` contains an email address, so leave that column
out before you send sample rows.

`ai_schema_prompt` reads tables, not views. Copy the columns you want to share
into a table:

```sql
CREATE OR REPLACE TABLE tickets_for_sql AS
SELECT * EXCLUDE (internal_note)
FROM support_tickets;
```

For sensitive tables, omit `sample_rows`. The model then sees only the schema.

## Inspect the schema prompt

Check what table context the model will see. This makes no provider call:

```sql
SELECT summary
FROM ai_schema_prompt(
    include_tables := ['main.tickets_for_sql'],
    sample_rows := 3
);
```

The context includes table names, column names, types, and up to three sample
rows.

## Generate SQL without running it

Use `ai_sql` when you want to inspect or store the generated query:

```sql
SELECT ai_sql(
    'Which high-priority customers have the lowest satisfaction scores?',
    include_tables := ['main.tickets_for_sql'],
    sample_rows := 3
) AS generated_sql;
```

Result: `generated_sql VARCHAR`, one read-only `SELECT`.

## Generate and run SQL

Use `ai_query_data` when you want the generated query to run immediately:

```sql
SELECT *
FROM ai_query_data(
    'Count tickets by priority and customer tier',
    include_tables := ['main.tickets_for_sql'],
    sample_rows := 3
);
```

Result: whatever columns the generated query returns, so the shape can change
when the question changes.

`ai_query_data` calls the model while DuckDB binds the query, before any rows
are read. It caches the generated SQL in the current DuckDB instance, keyed by
the question, the schema context, the model and the options. Running the same
statement again reuses the cached SQL without another provider call. Check the
cache with `SELECT * FROM ai_query_cache_stats();`.

## Retry when generated SQL does not bind

A model can invent a column or a function name. With `fix_attempts := N` (0 to
5, default 0), the function binds the generated SQL against your catalog and,
when binding fails, sends the error back to the model for up to N more calls:

```sql
SELECT *
FROM ai_query_data(
    'Average satisfaction score by channel for high priority tickets',
    include_tables := ['main.tickets_for_sql'],
    fix_attempts := 2
);
```

Each correction is a normal model call and appears in `ai_usage()`. `ai_sql`
accepts the same option.

## Keep context scoped

Limit `include_tables` to the tables relevant to the question. For a larger
database, this keeps the prompt smaller and reduces the chance that the model
uses the wrong table.

```sql
SELECT ai_sql(
    'Show the average satisfaction score by channel for high priority tickets',
    include_tables := ['main.tickets_for_sql'],
    sample_rows := 2
);
```

## Validate generated SQL yourself

If you store generated SQL before running it, validate it again at the
boundary. With `true` as the second argument, the SQL must also bind against
the current catalog, so a missing table or column raises an error:

```sql
WITH generated AS (
    SELECT ai_sql(
        'Count tickets by customer tier',
        include_tables := ['main.tickets_for_sql'],
        sample_rows := 3
    ) AS sql_text
)
SELECT ai_validate_read_only_sql(sql_text, true) AS validated_sql
FROM generated;
```

Result: `validated_sql VARCHAR`, the input SQL unchanged. Use
`ai_is_read_only_sql(sql_text, true)` instead to get a `BOOLEAN` without
raising an error.

Remove the example table when you are done:

```sql
DROP TABLE IF EXISTS tickets_for_sql;
```

See the [SQL assistant reference](../functions.md#sql-assistant-functions) for
every option.
