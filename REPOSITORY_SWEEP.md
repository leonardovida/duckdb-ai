# Repository sweep — 2026-10-01

The sweep found and fixed correctness defects, removed two substantial local
performance bottlenecks, and repaired a static-analysis gate that was silently
skipping the extension. All executed checks pass. This is evidence of improved
reliability, not a guarantee that the repository contains no remaining bugs.

## Scope

Reviewed the project-owned C++ sources and headers, SQL and mock-provider tests,
Python examples and benchmarks, build configuration, CI workflows, shell scripts,
documentation, and Docusaurus frontend/dependencies. DuckDB and extension CI
submodules were used for validation and left unchanged; their complete upstream
implementations were not audited. No live AI providers were called.

## Fixed defects

| Area | Previous behavior | Result |
| --- | --- | --- |
| Schema union types | A value matching none of an array of allowed types could pass. | Unmatched values fail validation. |
| Nested boolean schemas | Boolean children could be ignored by supported schema keywords. | `true`/`false` children participate in validation. |
| Unicode length | Schema lengths counted UTF-8 bytes. | Lengths count Unicode code points, including combining marks. |
| Integer accuracy | JSON integers passed through doubles, corrupting large record values and some schema comparisons. | Signed/unsigned 64-bit integer digits remain exact in projection, bounds, enum, uniqueness, and integer divisibility checks. Out-of-range BIGINT projections fail safely. |
| Typed responses | A provider response could enter the cache and count as successful before typed validation failed. | Typed validators run before success accounting; rejected responses are evicted and usage records the error. Validation does not introduce automatic retries. |
| Scalar record failures | Projection exceptions could escape `fail_on_error := false`. | Failed records return a null struct under that policy. |
| Embedding precision | Synthetic cached embedding JSON used 15 significant digits. | Cache serialization preserves round-trip double precision. |
| Similarity | Squaring extreme finite coordinates could overflow or underflow. | Scaled norms produce finite cosine values in `[-1, 1]`; zero vectors remain errors. |
| Classifier holdouts | A global row-index split could hold out every example of a sparse class; an unevaluated artifact could be usable at threshold zero. | Holdouts are selected within classes and retain training examples. Usability requires evaluated holdout rows. |
| Batch-size arithmetic | An oversized finite result could be cast outside the signed integer range. | Results outside BIGINT raise an error. |
| Static analysis | The shared `src/.*/` file filter matched neither extension C++ file directly under `src/`. | `make tidy-check-ai` analyzes both sources, and CI invokes it. Findings were resolved; the ABI-specific `google-runtime-int` style check is disabled for this target. |
| Website dependencies | npm audit reported three high-severity affected packages. | Patched `brace-expansion`, `fast-uri`, and `joi`; the final audit reports zero known vulnerabilities. |

## Performance evidence

Compared a saved, unchanged release binary with the final release binary on the
same Linux host. Runtime cases used the repository's local mock embedding server
and alternated binary order across five repetitions. Chunk cases used five
repetitions per binary, except the original 1 MB case was stopped after its first
five-second timeout. Timings include shell startup and query execution.

| Workload | Before median | After median | Observation |
| --- | ---: | ---: | --- |
| 1,000 unique embeddings | 36.6 ms | 31.6 ms | 1.16× faster |
| 10,000 unique embeddings | 402.4 ms | 109.8 ms | 3.66× faster |
| 1,000 repeated similarities | 25.8 ms | 25.9 ms | Essentially unchanged |
| 10,000 repeated similarities | 41.3 ms | 41.5 ms | Essentially unchanged |
| 100 KB newline-rich document, fixed chunks | 644.9 ms | 21.5 ms | 30.0× faster |
| 1 MB newline-rich document, fixed chunks | >5,000 ms | 37.7 ms | Original timed out; no precise speed ratio |

Usage eviction now pops from a deque instead of moving up to 1,024 events under
a mutex. Pricing lookup copies only the selected price instead of the complete
catalog, preserving date-sensitive Gemini pricing. Chunk headings and page
numbers advance through the input instead of scanning each prefix; heading
delimiter search also avoids repeatedly searching the suffix for a missing page
break. Existing batching and similarity deduplication request counts are retained.

Raw timings, request counts, retained/dropped usage events, and sampled process
metrics are in [repository-sweep-benchmarks.json](test/benchmarks/repository-sweep-benchmarks.json).
These results measure local overhead, not remote-provider latency or production
throughput. No material similarity speedup was measured.

## Validation

- Release build succeeded with GCC 14 and Ninja on Linux.
- SQLLogicTests passed: 524 assertions, up from the original 500.
- Main mock-provider suite passed, including provider API coverage, Jev provider
  and typed/batched Jev coverage, and 58 new repository regression checks.
- Resumable enrichment and labeled Jev batch evaluation smoke suites passed.
- The corrected clang-tidy target passed with LLVM 22.1.8 and analyzed both C++
  sources. DuckDB's full extension format gate and `git diff --check` passed.
- Docusaurus TypeScript check and production build passed; npm audit reported
  zero known vulnerabilities. Both project shell scripts passed `bash -n`.

## Prioritized further improvements

1. **Gate accuracy on independent, representative labeled data.** Extend
   `examples/jev_batch_evaluation.py` into a maintained evaluation job with
   per-class precision/recall, confusion matrices, abstention/fallback coverage,
   latency, and cost. Include minority classes, multilingual text, ambiguity,
   out-of-domain input, and malformed outputs. `BuildClassifierArtifact` currently
   measures agreement with provider-generated labels, which cannot establish
   correctness against human ground truth. Set acceptance thresholds from the
   application's error costs, and rerun after model or prompt changes.

2. **Improve classifier sampling and calibration.** `ClassifierAppendSample`
   keeps the first bounded samples rather than a representative reservoir.
   Ordered data can omit later classes; duplicate or correlated examples can
   inflate holdout agreement. Add deterministic reservoir sampling and grouped
   holdouts, with explicit coverage diagnostics and independently calibrated
   confidence/fallback thresholds. This changes artifact quality and should be
   evaluated before changing defaults.

3. **Make request token budgets more faithful.** `EstimateTokenCount` uses
   approximately one token per four bytes. `EstimatedCompletionTokens` reserves
   the user prompt and expected output but omits the separate system prompt and
   response schema. Account for those components, then add provider/model-aware
   tokenization or configurable conservative margins. Validate pacing against
   reported usage, especially for long instructions, CJK text, and code. Current
   token limits are estimates, not exact provider quota enforcement.

4. **Expand required platform and fault coverage.** Keep the repaired tidy gate
   required. Add focused ASan/UBSan builds, parser/schema fuzzing, and cancellation
   and concurrent-connection stress tests. Make a small macOS/Windows matrix
   required for C++ changes; full distribution builds currently require manual
   opt-in. This sweep verified Linux behavior and did not run sanitizers or live
   provider contracts.

5. **Bound every cache by bytes and expose durable job totals.** The response
   cache already has a byte cap, but `StorePromptQueryCachedSql` caps only entry
   count. Add a generated-SQL byte budget and exercise it with large schemas.
   `ai_usage_summary()` summarizes retained events, so it ceases to represent
   complete job totals after eviction; use persistent aggregate counters or
   durable exports for billing/reconciliation. Existing drop counters remain
   useful and are verified by the new regressions.

6. **Harden the release script before relying on automated publishing.**
   `scripts/extension-upload.sh` uses a fixed `private.pem` in the working
   directory and deletes it only after signing succeeds. Use a restrictive
   temporary file with an EXIT trap, quote paths, validate positional inputs,
   and test signing/compression/upload paths with mocked commands. This sweep
   checked shell syntax; it did not execute signing or publishing.

7. **Split large C++ modules around testable responsibilities.** Separate schema
   validation, provider transport/cache/accounting, classifier logic, and
   chunking from registration/binding. Preserve the SQL API and add focused
   internal tests during extraction. This reduces review and regression risk;
   profile mixed provider/model workloads before changing batching or grouping.

Remaining numerical limits should stay explicit: the JSON Schema implementation
is a documented subset, exact integer guarantees cover 64-bit integers, and
non-integer schema arithmetic remains floating point. A successful schema check
does not establish factual accuracy of an AI-generated answer.
