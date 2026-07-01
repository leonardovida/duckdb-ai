# Security policy

## Supported versions

`duckdb_ai` is currently a pre-1.0 preview. Security fixes target the current
main branch and the latest tagged preview release when one exists.

## Reporting a vulnerability

Use GitHub private vulnerability reporting when it is available for this
repository. If private reporting is not available, contact the maintainers
privately before opening a public issue.

Do not include API keys, provider credentials, private data, session cookies, or
full sensitive prompts in public reports. Include the affected version or commit,
the provider path involved, a minimal reproduction, and whether the issue
requires a configured log endpoint or provider credentials.

## Security expectations

- No provider telemetry is sent unless the user configures a provider call.
- Usage logging is disabled unless a log endpoint is configured.
- Prompt, input, and response text are omitted from usage logs by default.
- Provider API keys should come from environment variables or `TYPE duckdb_ai`
  secrets, not direct SQL arguments.
- `duckdb_ai_allowed_hosts` can restrict provider and usage-log destinations.
