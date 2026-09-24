-- Create jev_batch_input(source_id, body VARCHAR), then run this file in the
-- same connection. Set TYPESAFE_API_KEY in the environment before starting.
LOAD ai;

-- Materialize once so downstream projections and exports do not call Jev again.
CREATE TEMP TABLE jev_batch_results AS
SELECT source_id, ai_jev(body, {
    department: MAP {
        'billing': 'Payments, duplicate charges, invoices, refunds',
        'technical': 'Bugs, outages, data imports, integrations',
        'other': 'None of the above'
    },
    urgency: ['Routine question', 'Degraded work with a workaround', 'Production is blocked'],
    refund_requested: MAP {
        'true': 'Explicitly asks for money to be returned',
        'false': 'Does not explicitly ask for money to be returned'
    }
}, model := 'jev-1.13.0', batch_size := 32) AS decision
FROM jev_batch_input;

SELECT source_id, decision.* FROM jev_batch_results ORDER BY source_id;
