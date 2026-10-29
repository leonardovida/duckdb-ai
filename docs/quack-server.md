---
sidebar_position: 7
title: "Run on a Quack server"
sidebar_label: "Run on a Quack server"
description: "Serve DuckDB AI functions to remote DuckDB clients over Quack: load the extension on the server, keep provider credentials there, and lock down what clients can reach."
keywords: ["DuckDB Quack", "DuckDB remote server", "DuckDB AI server", "DuckDB 2.0", "CONNECT", "LLM credentials"]
---

# Run on a Quack server

DuckDB 2.0 ships [Quack](https://duckdb.org/docs/current/quack/overview), a
client-server protocol that lets one DuckDB instance serve others over HTTP. When a
client routes its session to the server with `CONNECT`, every query runs on the
server. The `ai` extension, its provider credentials and its outbound requests then
all live on the server, and clients need neither the extension nor any API key.

## Start the server

Community extensions are not autoloaded, so the server has to load `ai` itself.
Put the setup in an init file:

```sql
-- ai-server.sql
INSTALL ai FROM community;
LOAD ai;
SET GLOBAL duckdb_ai_provider = 'openai';
```

Then start the server with the provider key in its environment:

```sh
OPENAI_API_KEY=... duckdb analytics.duckdb -init ai-server.sql -serve
```

`-serve` starts Quack on `localhost:9494` and stores its token in a persistent
`quack` secret, so `duckdb -connect` on the same machine works without further
setup. To serve other machines, call `quack_serve` with `allow_other_hostname`
and put a TLS-terminating reverse proxy in front, as described in the
[Quack security guide](https://duckdb.org/docs/current/quack/security).

Instead of an environment variable, the key can live in a persistent secret on the
server, which survives restarts:

```sql
CREATE PERSISTENT SECRET openai_server (
    TYPE duckdb_ai, AI_PROVIDER 'openai', API_KEY '...'
);
```

## Call AI functions from a client

Attach the server and route the session to it:

```sql
CREATE SECRET (TYPE quack, TOKEN '⟨server token⟩', SCOPE 'quack:ai-server.example:443');
ATTACH 'quack:ai-server.example:443' AS srv;
CONNECT srv;

SELECT ticket_id, ai_classify(body, ['billing', 'outage', 'other']) AS category
FROM support_tickets;

DISCONNECT;
```

While connected, every statement runs on the server, including `SET duckdb_ai_*`
and `CREATE SECRET`. A client can therefore pick models or bring its own
`TYPE duckdb_ai` secret for its session.

Where the call runs depends on how the client reaches the server:

| Client SQL | Where `ai_*` runs | Who needs the extension and a key |
| --- | --- | --- |
| `CONNECT srv; SELECT ai_classify(body, ...) FROM t` | Server | Server |
| `FROM srv.query('SELECT ai_classify(body, ...) FROM t')` | Server | Server |
| `SELECT ai_classify(body, ...) FROM srv.t` without `CONNECT` | Client | Client |

AI calls stop when the server interrupts the query, for example when the server's
`max_execution_time` passes or a client cancels the query with `quack_cancel`.

## What clients share

Each `CONNECT`ed client gets its own server session, but the extension's state is
per database instance:

- `ai_usage()` lists the usage events of every client, and `ai_clear_usage()` or
  `ai_clear_cache()` from one client clears them for all.
- The response cache, rate limits and token budgets are shared. All clients
  spend from the same provider account when they use the server's key.

Use Quack's authentication and authorization hooks to decide who may connect and
which statements they may run.

## Keep credentials and egress on the server

Remote clients can run any SQL the server allows, so the extension does not let a
session decide where the server's keys or requests go:

- **Keys stay with their origin.** An API key from a `TYPE duckdb_ai` secret is
  only sent to the scheme, host and port of the `BASE_URL` stored in that
  secret. Without one, the key, like a key from an environment variable, is only
  sent to the provider's endpoint: its built-in URL or the base URL environment
  variable (for example `OPENAI_BASE_URL`). A client that passes `base_url := ...`, sets
  `duckdb_ai_base_url`, or creates an external model whose `location` points
  elsewhere gets an error, and no request is sent.
- **The environment allowlist is a ceiling.** When the server process has
  `DUCKDB_AI_ALLOWED_HOSTS` set, every provider and usage-log request must match
  it. A session can narrow it with `duckdb_ai_allowed_hosts` but not widen it.
  Hosts in this variable may also receive the server's keys.
- **`enable_external_access = false` applies.** With external access disabled, the
  extension only sends requests to endpoints listed in `allowed_directories` or
  `allowed_paths`.

A locked-down server starts Quack in its init file before it disables external
access, because Quack and persistent secrets are read from the home directory:

```sql
-- ai-server-locked.sql
INSTALL ai FROM community;
LOAD ai;
SET GLOBAL duckdb_ai_provider = 'openai';
CALL quack_serve();
SET allowed_directories = ['https://api.openai.com/'];
SET enable_external_access = false;
```

```sh
DUCKDB_AI_ALLOWED_HOSTS=api.openai.com OPENAI_API_KEY=... duckdb analytics.duckdb -init ai-server-locked.sql
```

`enable_external_access` cannot be turned back on for the running instance.

To send a key through a gateway, store the gateway URL in the secret, set the
provider's base URL environment variable, or list the gateway host in
`DUCKDB_AI_ALLOWED_HOSTS`:

```sql
CREATE PERSISTENT SECRET gateway (
    TYPE duckdb_ai, AI_PROVIDER 'openai', API_KEY '...', BASE_URL 'https://ai-gateway.internal/v1'
);
```

See [security and data flow](security-data-flow.md) for the full list of egress
controls.
