---
name: Bug report
about: Report a reproducible duckdb_ai bug
labels: bug
---

## Summary

## Reproduction

```sql
-- minimal SQL, without credentials
```

## Expected behavior

## Actual behavior

## Environment

- duckdb_ai version or commit:
- DuckDB version:
- OS and architecture:
- Provider or local mock:

## Validation run

```sh
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```
