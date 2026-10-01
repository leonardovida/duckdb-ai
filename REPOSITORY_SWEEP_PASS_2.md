# Repository sweep, second pass — 2026-10-01

This follow-up to the v0.6.1 sweep fixes additional classifier, cache, token-budget,
and release-packaging defects. It adds independent-label evaluation gates and diagnostics
and removes the generated SQL cache's linear recency scan. All executed checks
pass. Validation reduces regression risk; it does not establish that the repository
is bug-free or that a live model meets an application's accuracy requirements.

## Scope and baseline

Started from clean `main` at `d49d1b162781b201e8547c9601a44c1336e2660f`
(v0.6.1) and saved its release binary for reproductions and measurements. Reviewed
project-owned C++ and provider paths, Python examples/tests, cache and pacing
behavior, signing/upload scripts, CI/build configuration, and documentation and
website dependencies, emphasizing gaps identified by the first sweep.
DuckDB and extension CI submodules remain unchanged. HTTP tests use local mock
providers; upload tests use stand-ins for AWS and Brotli. No live provider calls,
actual uploads, release publication, or upstream community changes were made.

## Corrections and improvements

| Area | Previous behavior | Result |
| --- | --- | --- |
| Centroid accumulation | Summing several finite coordinates near DOUBLE's maximum overflowed, although their mean was finite. | Incremental means avoid overflow for both equal and opposite signs; extreme finite artifacts can train successfully. |
| Classifier label formatting | Commas split a label into separate prompt choices; quotes/backslashes were not decoded consistently, and control characters could produce invalid label JSON. | One JSON escaping path preserves commas, quotes, backslashes, Unicode, and control characters through scalar classification, training, and fallback. |
| Canonical fallback labels | Already validated labels were decoded a second time, stripping literal quotes, outer whitespace, or Markdown fences. | The final artifact lookup compares the canonical label exactly. |
| Invalid label sets | Whitespace-only labels and labels duplicated under trimmed case folding could survive classifier/artifact validation. | These cases fail early under the same uniqueness rules as classification prompts. |
| SQL cache isolation | Credential rotation or replacing a model profile could reuse earlier SQL from the same question/schema pair. | Keys include the resolved provider configuration, credential fingerprint, profile name, and profile options. Tests verify distinct account results and newly tightened input limits. Credentials are not stored directly in cache keys. |
| SQL cache bounds | A 1,024-entry cap still permitted large retained key/schema strings with no byte limit. | Retained key and SQL text is capped at 64 MiB, counting both owned key copies. Oversized entries bypass caching, and replacements remove old entries. Container overhead is additional. |
| SQL cache recency | Every cache hit searched and erased a deque entry under the cache mutex. | Stored list iterators and splice update recency in constant time; map lookup and key hashing still depend on key length. |
| SQL cache allocation failures | An allocation failure during insertion could leave retained-byte accounting or the recency list inconsistent. | Failed insertions roll back the list entry, and byte accounting changes only after insertion succeeds. |
| Estimated completion budgets | Reservations omitted separate system prompts and response schemas; input-limit checks omitted schemas. | Reservations include all three input components and the requested/default output estimate. Schema copies in instructions and structured-output fields both count. |
| Unsigned packaging | Truncating an existing 256-byte signature file left its previous signature intact. | Every unsigned run starts with a fresh zero-filled signature. The v0.6.1 behavior was reproduced with a sentinel signature. |
| Signing workspace and cleanup | Signing used caller-owned `private.pem`; the hash helper's `x*` files could overwrite/delete caller files, and signing failures left the key behind. | Private staging isolates all hash/signing files, restricts permissions, and cleans up on failure and exit. Existing caller sentinels remain untouched. |
| Signature size | Unsupported signature lengths were silently padded or truncated. | Only the required 256-byte RSA signature is accepted; other sizes fail before packaging sidecars are replaced. |
| WebAssembly packaging | `wasm*` selected WebAssembly headers, while only `wasm_*` selected Brotli. | Every `wasm*` target uses the same WebAssembly packaging/compression/header path. Optional upload flags and credentials may be omitted safely. |
| Accuracy diagnostics | Aggregate accuracy could conceal minority-class mistakes or missing predictions. | The labeled Jev evaluator reports support, precision, recall, F1, macro F1, answer coverage, and a confusion matrix with failures separate from predicted labels. Undefined metrics are null; macro F1 lists the observed ground-truth classes used. |

## Regression evidence

The saved v0.6.1 binary failed targeted reproductions for finite centroid overflow,
comma-bearing fallback labels, credential rotation, SQL-cache byte retention,
and long-system-prompt pacing. The old packaging script retained a sentinel
signature on an unsigned rerun. Final review additionally reproduced the double
normalization of a literal quoted label before its fix.
An allocation-fault harness using the actual cache insertion function reproduced
invalid cache accounting before the fix. Nine injected allocation failures now
preserve cache invariants, including replacement and subsequent eviction.

New runtime checks cover opposite-sign means, eleven label shapes through three
classification paths, account/profile changes, byte eviction with recency refresh,
and system/schema reservations. Pacing tests interrupt the waiting process and
verify only the first request reached the provider; they do not test an entire
60-second refill window. Signing tests verify a real RSA signature over multiple
1 MiB hash chunks, unsupported key sizes, injected signing failure, cleanup,
unsigned reruns, WebAssembly framing/headers, and native upload destinations.
The AWS and Brotli stand-ins check invocation and framing, not real S3 credentials
or a platform's installed Brotli encoder.

Evaluation tests use supplied ground-truth labels and include misclassification,
missing predictions, classes without predictions, unobserved classes, all-failed
input, and empty input. Existing request-count, usage, opt-in, and batch failure
checks remain in place. Evaluation output additions preserve the earlier metrics.

## Measured cache performance

Five repetitions per binary and workload, alternating binary order on the same
Linux host. Each process warms 1,024 distinct cache entries, then times 2,000
repeated binds of the newest entry. Questions use equal-width numeric suffixes;
the long workload also shares a 4,096-character prefix. Timers exclude process
startup and warmup. Both binaries make exactly 1,024 mock-provider requests per
run and no provider calls during the timed cache-hit phase.

| Workload | v0.6.1 median | Final median | Speedup |
| --- | ---: | ---: | ---: |
| Short questions, full cache | 321.5 ms | 277.9 ms | 1.16× |
| Shared 4,096-character question prefix, full cache | 500.8 ms | 333.9 ms | 1.50× |

These are local cached-binding measurements, not remote-provider throughput or
a promised production speedup. Host noise affects small differences. Raw rounds,
request counts, platform, and binary hashes are in
[test/benchmarks/repository-pass2-benchmarks.json](test/benchmarks/repository-pass2-benchmarks.json).
The reproduction runner is
[test/benchmarks/prompt_sql_cache_benchmark.py](test/benchmarks/prompt_sql_cache_benchmark.py).

## Validation

- Release build succeeded on Linux with GCC 14 and Ninja.
- SQLLogicTests: **530 assertions**, including new label and diagnostic cases.
- Main mock suite passed, including provider API, Jev provider and typed batching,
  the earlier 135 regression checks, and **42 second-pass runtime checks**.
- Packaging smoke passed: **8 checks**, wired into CI and the release checklist.
- Resumable enrichment and labeled Jev evaluation suites passed.
- Corrected clang-tidy gate, full extension format check, and `git diff --check`
  passed after final source changes.
- Docusaurus typecheck and production build passed after final documentation
  changes. npm audit still reports zero known vulnerabilities.
- Both project shell scripts passed `bash -n`; the generated community docs
  snapshot remains current. Vendored submodules are clean.

## Follow-through implementation

The four priorities from the initial second-pass report were implemented before
opening the source PR:

1. **Sampling and independent accuracy gates.** Classifier aggregation now uses
   a bounded bottom-k reservoir of distinct texts, with stable hashed priorities
   and collision-safe text tie breaking. Merge results are independent of input
   order and worker partitioning. Exact duplicates are retained once, preventing
   their appearance in both training and holdout. Tests cover ordered input,
   duplicate-heavy input, and parallel aggregate merging. The evaluator accepts
   accuracy, macro-F1, coverage, class-recall and required-label gates. CI uses
   independent hand-authored synthetic multilingual/minority/out-of-domain cases
   to exercise acceptance and rejection; it does not certify live model quality.
2. **Configurable token margins and context windows.** Profiles accept finite
   `token_estimate_multiplier` values from 1 through 16, shared by completion
   pacing, input limits, and embedding batch planning. `context_size` reserves
   estimated input plus requested output (512 estimated tokens when unspecified);
   `max_input_tokens` remains independently enforced as input only. Malformed
   numeric limit options fail explicitly instead of silently disabling limits.
3. **Fault/platform checks.** CMake supports instrumenting the two owned C++
   sources with ASan/UBSan while linking the normal DuckDB core. Local sanitizer
   checks passed the earlier 135 regressions, 42 second-pass cases, and generated
   reliability cases. The latter include 100 schema and 40 Unicode chunk
   cases, sampling, budget and lifetime-counter checks, and concurrent requests.
   Normal-library stress additionally exercises four connections sharing one
   database and verifies one coalesced provider request. macOS ARM64 and Windows
   AMD64 builds/tests now run on each change through DuckDB's distribution tooling,
   and the aggregate CI gate fails on any failed/skipped validation job.
4. **Observable and persisted totals.** `ai_query_cache_stats()` exposes cache
   entries, retained string bytes, bounds, hits, misses, evictions and oversized
   bypasses. `ai_usage_totals()` retains provider row events, transport attempts,
   failures, cache hits, partial known tokens and unknown-token event counts
   independently of event-buffer eviction. The resumable enrichment example
   persists each batch snapshot in its result transaction, surviving reopening
   and rolling back uncommitted snapshots with results.

The final normal reliability suite passes **158 checks**, including the
shared-connection case. The independent accuracy-gate and evaluator suites pass;
CI runs them alongside the standard regression suites. The generated community
function snapshot includes both additive diagnostic functions. Existing SQL
function names, options and result schemas remain compatible; artifact version
remains 1 with additive sampling metadata.

## Deployment limits

Use representative independently labeled production data to set the new gates.
Exact provider tokenization remains unavailable; margins are configurable estimates,
not exact quotas. Exact duplicate isolation does not group near-duplicate records
or establish confidence calibration. Core/vendor code remains uninstrumented in
the owned-code sanitizer target. Lifetime counters are in memory; only explicitly
persisted batch snapshots survive reopening. No live AI-provider contracts, real
S3 uploads, or actual Brotli encoder were tested. Platform CI results are recorded
on the source PR before merge rather than claimed from the Linux workspace.
