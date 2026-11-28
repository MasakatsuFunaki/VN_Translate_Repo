# `vn::llm_client`

Physically insulated, Lakos-levelized C++20 LLM Strategy/Adapter library shared by `VN_TRANSLATE` game pipelines and `tools/spot_checker`.

## Physical Architecture & CMake Targets

All library sources (`src/**`) and standalone tests (`tests/**`) compile as **C++20** (`/W4 /utf-8 /bigobj`), while exported headers under `include/vn/llm/` remain C++17-compatible (`PUBLIC cxx_std_17 PRIVATE cxx_std_20`, with C++20 concepts `RawHttpTransportLike`, `BackoffSleepCallable`, and `DiagnosticLogCallable` guarded for C++20 consumers):

- **Level 1 — `vn::llm_core` (`src/core/`):** Pure C++20 domain value types (`llm_types`), protocol vtable anchors (`ILlmClient`, `IRawHttpTransport`), `concepts`-constrained `execute_with_exponential_backoff` policy template, `<ranges>` ASCII/redaction helpers, and `ZeroThoughtCircuitBreaker`. Zero Boost, OpenSSL, or OS dependencies.
- **Level 2 — `vn::llm_gemini` (`src/transport/`, `src/adapters/gemini/`):** Gemini `v1beta` wire codec, PIMPL `BeastTlsHttpTransport` (Windows `ROOT` cert store + OpenSSL + Boost.Beast TLS 1.2+), side-effect-free `.env` / registry credential lookup, and PIMPL `GeminiClientAdapter`. Links Boost, OpenSSL, `ws2_32`, `crypt32`, and `advapi32` strictly `PRIVATE`.
- **Level 3 — `vn::llm_client` (`src/llm_factory.cpp`):** Escalated parameterized factory (`create_llm_client`) linking `PUBLIC vn_llm_core vn_llm_gemini`.

## Canonical Invariants (`include/vn/llm/llm_types.h`)

- `kDefaultModel = "gemini-4-carbon"`, `kDefaultThinkingLevel = "MEDIUM"`, `kDefaultBatchSize = 40` (`thinkingBudget` emitted only when `thinking_level` is explicitly cleared; `argon` model identifiers are rejected via `is_disallowed_model` / `ensure_supported_model`).
- `maxOutputTokens` clamped to `[16384, 65536]` by default (`compute_batch_max_output_tokens`).
- Separate TCP/TLS connect deadline (`60s`) and non-streaming response read deadline (`600s`), `4` exponential-backoff retries (`2000ms` base), and `<redacted>` API-key scrubbing across transport/HTTP exceptions and retry logs.
- `ZeroThoughtCircuitBreaker`: trips immediately on Batch 0 (`slice_size >= 4`, `thought_tokens <= 0`) or after `2` consecutive unblocked top-level batches (`slice_size >= 16`), ignoring bisected sub-slices.

## Embedding via CMake `add_subdirectory`

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tools/llm_client
                 ${CMAKE_CURRENT_BINARY_DIR}/vn_llm_client
                 EXCLUDE_FROM_ALL)

target_link_libraries(my_pipeline_target PRIVATE vn::llm_client)
```

Standalone GoogleTest suites (`ut_llm_client`, `ct_llm_client`, `st_llm_model_baseline`) build only when `<repo_root>/tools/llm_client` is the top-level CMake project.

## Building & Testing Standalone

```powershell
cd <repo_root>\tools\llm_client
python build.py --clean
python build.py --test
```

- **`UT` (`tests/ut_llm_client.cpp`):** Compile-time `#ifdef` header-insulation guard, C++20 concept checks, wire codec, `.env` parser, `ZeroThoughtCircuitBreaker` BVA + postconditions, and 6-gate gauntlet evaluator unit tests.
- **`CT` (`tests/ct_llm_client.cpp`):** Multi-request stateful component tests (`429 -> 503 -> 200` backoff recovery, per-request model override, API-key redaction, non-retryable `400` fast fail) over `IRawHttpTransport`.
- **`SYSTEM` (`tests/st_llm_model_baseline.cpp`):** Live HTTPS/TLS call to `generativelanguage.googleapis.com:443` running the 8-line `CROSS_CHANNEL` Model-Degradation Baseline Gauntlet (`gemini-4-carbon`, `thinkingLevel: "MEDIUM"`, `thought_tokens >= 150` + 5 literary/wordplay gates; fails loudly locally if `GEMINI_API_KEY` is missing).
