# Provider coverage rollout

Requested focus: DeepSeek, Qwen, GLM (Z.ai), Kimi, MiniMax,
Tencent HY4/HY3, and Xiaomi MiMo. Preserve existing providers and SQL contracts.
This is a feature rollout requested explicitly, separate from maintenance automation.

## Definition of support

Track each public API capability separately: documented, request-tested,
mock-response-tested, and live-verified. Arbitrary model identifiers must remain
usable without waiting for a catalog release. Never describe an entire provider
as fully supported merely because its chat endpoint works.

## Starting inventory

| Provider | Current implementation | Main investigation |
| --- | --- | --- |
| DeepSeek | Chat, built-in default/pricing | Thinking, tool state, cache usage, FIM |
| Qwen/DashScope | Chat and embeddings | Current regional APIs, reasoning, structured output, reranking |
| GLM/Z.ai | Chat and embeddings | Native options, coding versus general endpoints, current models |
| Kimi/Moonshot | Chat | Thinking/tool state, token limits, native request options |
| MiniMax | Chat | Messages compatibility, reasoning and structured output |
| Tencent/Hunyuan | TokenHub chat, HY3 default | HY4 preview and HY3 across Chat/Responses/Messages, regional hosts |
| MiMo/Xiaomi | No native provider | Authentication, chat and Messages endpoints, model catalog |

This inventory describes code, not live service validation. Remaining provider
contracts require individual primary-source research before implementation.

## Sequence and acceptance gates

1. Examples: use Ollama `qwen3.8:27b` explicitly. Keep the existing runtime
   fallback unchanged until a separate default-policy decision. Validate docs.
2. Build a dated provider-by-capability matrix from official references, using
   Exa for discovery and Context7 for implementation documentation. Record model
   identifiers, lifecycle, regions, auth, limits, pricing, and protocol variants.
3. Close basic native-provider gaps: MiMo first, then Tencent HY4/HY3 endpoint
   and model selection. Preserve aliases, explicit URLs and model overrides.
   Require deterministic request tests and mock HTTP auth/response smokes.
4. Add controlled provider-native options to existing text operations where
   needed. Prevent overrides of extension-owned fields and credentials. Test
   reasoning-only, empty, truncated, refused, malformed and tool-call responses.
5. Implement protocol coverage in separate reviewed slices: Chat first,
   Messages second, Responses third. Define SQL result representation before
   exposing tool calls or multi-turn state. Never auto-execute returned tools.
6. Validate embeddings/reranking only where each provider publicly offers them.
   Test ordering, dimensions, partial failures, batching, retries and accounting.
7. Image/audio/video generation, uploads and asynchronous jobs are excluded by
   the user's scope decision. Complete text/reasoning/tools/embeddings/reranking first.
8. For each slice: format/build/SQLLogic/mock smokes, docs typecheck/build,
   supported DuckDB compatibility, and opt-in credentialed live checks. Label
   untested regions/models honestly. Publication requires explicit authorization.

## Initial verified public sources

- [Ollama Qwen3.8 27B](https://ollama.com/library/qwen3.8:27b)
- [DeepSeek thinking/tool state](https://api-docs.deepseek.com/guides/thinking_mode)
- [Tencent protocol/model matrix](https://www.tencentcloud.com/document/product/1300/80632)
- [MiMo chat](https://mimo.mi.com/docs/en-US/api/chat/openai-api)
- [MiMo Messages](https://mimo.mi.com/docs/en-US/api/chat/anthropic-api)

## Status

Implemented locally on 2026-09-14:

- Qwen3.8 27B examples; existing Ollama fallback preserved.
- Native MiMo provider with Xiaomi aliases, API-key header and token-limit spelling.
- `request_options` on existing completion functions, with reserved-field and
  duplicate-key checks and cache-key/equality integration.
- `ai_provider_call` for native Chat, Messages, Responses, embeddings, reranking
  and FIM bodies. Full non-streaming response objects and buffered SSE event
  objects preserve reasoning, signatures, tool calls, usage and finish states.
  Model IDs remain unrestricted; tools are never automatically executed.
- Non-chat calls require an exact endpoint, supporting region/workspace paths
  without guessing. Native embeddings/reranking preserve provider indices and
  shapes; `ai_embed` retains automatic batching. `ai_rerank` remains the existing
  completion scorer. These are deliberate interface boundaries, not protocol
  translations or a new model catalog service.
- Dated public-source matrix, request examples, endpoint/auth guidance and
  implementation limits in docs/provider-guides.md and docs/functions.md.
- New native HTTP smoke integrated into the standard mock suite covers seven
  providers, HY3/HY4 selections, native operations, auth, tool-only responses,
  state preservation, large integers, SSE terminators, HTTP/JSON/stream errors
  and rejected credentials/options.

Validation: final release build, 413 SQLLogic assertions, full TLS/mock HTTP suite,
format check, clang-tidy and docs typecheck/build passed. PR and merge are
authorized; hosted checks must pass before merge. No release is planned.

Live verification gate: the managed `DuckDB AI Provider Tests` environment was
inspected through 1Password (names only). It contains no direct credentials for
DeepSeek, Qwen, Z.ai, Kimi, MiniMax, Tencent or MiMo. Those account/model checks are
not run. Provider/model availability, region-specific behavior, pricing and limits
remain provider-controlled. This transport coverage must not be advertised as
live verification of every model or every provider API.
