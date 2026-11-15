# `spot_checker` — Statistical Hotspot & Surgical VN Translation Spot-Checker

`spot_checker` is a levelized, game-agnostic C++17 tool that scans visual novel script corpora (`<game_dir>/script_output/translated_text.json`) for statistical translation weaknesses and dispatches surgical sliding-window passes to `gemini-4-argon` (`thinkingBudget: 2048`, `responseMimeType: "application/json"`).

## Architecture & Component Layout

- **`spot_checker_foundation` (`L1..L3`)**: `common/` (`status`, `utf8_text`, `win32_cli_env`), `domain/` (`spot_check_types`, `vn_line_syntax`, `tone_glossary`), pure boundary protocols in `ports/` (`ILlmTransport`, `IFileStore`, `ICryptSync`, `IClock`, `ILogSink`), and `storage/` (`json_format_style`, `script_repository` with ordered `translation_cache_*.json` sync and pre-image atomic rollback).
- **`spot_checker_argon_client` (`L3..L4`)**: `client/argon_wire_codec` and PIMPL-insulated `client/argon_client` (TLS via cached Windows `ROOT` X.509 store to `generativelanguage.googleapis.com:443`).
- **`spot_checker_engine` (`L3..L5`)**:
  - `detector/` (`weakness_signals`, PIMPL `hotspot_detector`): Pellegrino script-weighted Gale-Church OLS length Z-scores, six JP->EN discourse trap signals, sustained multi-line H-scene span barriers, per-script Page CUSUM + Gaussian KDE + $O(N)$ NMS, and hybrid adaptive contagion.
  - `checker/` (`window_planner`, `prompt_codec`, `fix_validator`, `spot_check_checkpoint`, PIMPL `spot_checker`): Stage 1 packed GEMBA-MQM Radar (`6` passages/call), Stage 2 suspect-locked repair, progressive refusal recovery (`BLOCK_NONE`, QA framing, context-stripped retry, bisection, `blocked_spans` with `--model` re-queue), and post-edit validation.
- **`01_spot_check` (`L6`, `apps/spot_check_main.cpp`)**: Thin CLI composition root.

## Build & Test Pyramid

Tests live under `tests/UT/<component>/`, `tests/COMPONENT/<component>/`, and `tests/SYSTEM/<kind>/`, each wired via `add_subdirectory(...)` from the root `CMakeLists.txt`:

```powershell
python spot_checker/build.py --test --install   # Build + stage 01_spot_check.exe + offline UT/CT/ST (0 API calls)
python spot_checker/build.py --test --live-api  # Include budget-capped live gemini-4-argon ST gate
```

## CLI Usage (`01_spot_check.exe`)

```powershell
# Default production run (hotspot mode, 20 Radar calls, up to 400 windows, auto-applies + syncs .enc)
<repo_root>\spot_checker\build\install\bin\01_spot_check.exe --game <game_name>

# Offline statistical scan (0 API calls, 0 writes in <game_dir>) / linear dry-run / offline checkpoint apply
<repo_root>\spot_checker\build\install\bin\01_spot_check.exe --game-dir <game_dir> --scan-only --out <report.json>
<repo_root>\spot_checker\build\install\bin\01_spot_check.exe --game <game_name> --mode linear --offset 120 --window 40 --max-windows 1 --dry-run
<repo_root>\spot_checker\build\install\bin\01_spot_check.exe --game <game_name> --apply
```

| Option | Description |
|---|---|
| `--help, -h` | Show help message and exit (`0`). |
| `--game <name>` / `--game-dir <path>` | Target game folder under `<repo_root>` or explicit `<game_dir>` (mutually exclusive). |
| `--mode <hotspot\|linear>` / `--offset <N>` | Window selection mode (`hotspot` default; `--offset >= 0` implies `linear`). |
| `--scan-only` / `--dry-run` / `--apply` | Offline scan (`0` API calls), checkpoint-only LLM pass, or offline artifact apply (mutually exclusive). |
| `--out <path>` / `--model <id>` | Override output JSON path; set Gemini model ID (default `gemini-4-argon`; changing `--model` re-queues `blocked_spans`). |
| `--radar-calls <N>` / `--window <N>` / `--max-windows <N>` | Stage 1 Radar calls (`6` passages/call, default `20`), lines/window (`40`), max Stage 2 windows (`400`, `0` = all). |
| `--z-threshold <Z>` / `--min-confidence <C>` | Minimum hotspot Z-score (`0.0`) and LLM confidence gate in `(0.0, 1.0]` (`0.85`). |
| `--lock-non-suspects` / `--include-h-scenes` / `--fix-punctuation` | Lock non-flagged lines, include sustained H-scenes, or enable punctuation/romaji-note fixes. |

## Adapting for Custom Games & LLM Agents

- **Custom game JSONs (`0` C++ changes)**: Point `--game-dir <game_dir>` at `<game_dir>/script_output/translated_text.json` formatted as `{ "<file>": [{"jp":"...","en":"..."}] }`, `{ "<file>": {"strings": [...]} }`, `{ "<file>": {"lines": [...]} }`, or wrapped in `{"scripts": {...}}` (entry keys: `"text"`/`"content"`/`"jp"`, `"translated"`/`"translation"`/`"en"`, optional `"speaker"`/`"speaker_en"`/`"type"`). Add an optional `<game_dir>/TONE_OF_NOVEL.md` glossary/voice guide, set `GEMINI_API_KEY` in the environment or `%USERPROFILE%\.gemini\.env`, and rebuild/deploy your game's runtime table (e.g., `02_build_tsv.exe` + `python build.py --deploy --game-dir <install>`) after `01_spot_check.exe` updates `script_output/*.json`.
- **Swapping LLM providers (Claude, OpenAI, local Ollama/vLLM)**: Implement `ILlmTransport::generate(const LlmRequest&)` (`src/ports/llm_transport.h`) and pass it via `SpotCheckerDependencies::transport` into `SpotChecker::create(...)` in `src/apps/spot_check_main.cpp`; `detector/`, `checker/`, and `storage/` require zero modifications.
- **External git clone testing**: `python spot_checker/build.py --test` runs 100% offline (`0` API calls) without needing `translations_key.txt` or `GEMINI_API_KEY`.
