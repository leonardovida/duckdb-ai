---
sidebar_position: 2
---

# Create the sample support tickets table

Use this small table to try the cookbook examples. It includes customer
metadata, timestamps, free-text ticket fields, numeric account context, and an
internal note column with realistic content for redaction tests.

```sql
LOAD ai;

CREATE OR REPLACE TABLE support_tickets AS
SELECT *
FROM (
    VALUES
        (
            1001,
            'acme_corp',
            'enterprise',
            TIMESTAMP '2026-06-28 09:15:00',
            'email',
            'high',
            120000,
            2,
            'en',
            'Slack export delayed',
            'The Slack export has been stuck for two hours and blocks the finance close.',
            'Escalated by CSM. Mentions workspace ws_prod_eu and user pat@acme.test.'
        ),
        (
            1002,
            'acme_corp',
            'enterprise',
            TIMESTAMP '2026-06-28 10:40:00',
            'chat',
            'medium',
            120000,
            3,
            'en',
            'Invoice owner changed',
            'Please update the billing owner before the next renewal invoice is issued.',
            'Billing contact moved teams; keep renewal risk on the account watchlist.'
        ),
        (
            1003,
            'northwind',
            'startup',
            TIMESTAMP '2026-06-29 13:05:00',
            'web',
            'low',
            18000,
            5,
            'nl',
            'Documentation request',
            'Customer asks for an example that joins order rows with shipment status.',
            'Good candidate for a docs snippet; no production impact.'
        ),
        (
            1004,
            'globex',
            'business',
            TIMESTAMP '2026-06-30 08:20:00',
            'email',
            'high',
            64000,
            1,
            'en',
            'Query regression after upgrade',
            'Dashboard query that used to finish in 8 seconds now times out after the upgrade.',
            'Likely performance regression. Ask engineering to inspect query_id q_8842.'
        )
) AS t(
    ticket_id,
    customer_id,
    customer_tier,
    created_at,
    channel,
    priority,
    arr_usd,
    satisfaction_score,
    language,
    subject,
    body,
    internal_note
);
```

Check that the table is ready:

```sql
SELECT
    count(*) AS ticket_count,
    min(created_at) AS first_ticket,
    max(created_at) AS last_ticket
FROM support_tickets;
```

Expected shape:

| ticket_count | first_ticket | last_ticket |
| --- | --- | --- |
| `4` | `2026-06-28 09:15:00` | `2026-06-30 08:20:00` |

Next: [enrich support tickets with AI text functions](support-ticket-enrichment.md).
