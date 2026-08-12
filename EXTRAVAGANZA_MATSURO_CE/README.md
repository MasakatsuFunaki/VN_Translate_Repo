# EXTRAVAGANZA ~Matsuro~ CE (末路 / Downfall)

| | |
|---|---|
| **Engine** | nnn/BCmkri (BLACKCyc) — same as EXTRAVAGANZA CE |
| **Format** | SPT (XOR 0xFF encrypted, CP932) |
| **JP Strings** | ~6,174 |
| **SPT Files** | mushi.spt (main), mushi0.spt, mushi1.spt, fmb_backscript.spt, sys.spt |
| **Characters** | Miyaka (美弥香), Ayaka (綾佳), Rengoku (煉獄), Shirou (史郎), Collector, Announcer |
| **Delivery** | `winmm.dll` — runtime text hook via `translation_table.tsv`. SPTs are NOT modified (repack caused save/load crashes). |

> **AGENT** — Ghidra is the ONLY allowed disassembly method.
> Project: `D:\ghidra_projects\EXTRAVAGANZA_MATSURO_CE`. Write `.java` GhidraScripts, run via `analyzeHeadless.bat`.
> NEVER write scripts that scan EXE bytes for x86 opcodes.

## Pipeline

```
01_extract          → extracted_text.json
02_translate        → speaker gate → translated_text.json + translation_table.tsv (CP932, tab-separated)
00_run_all          → all in one shot
03_translate_xtx    → name-plate tables (one-shot, once per install, not part of 00_run_all)
```

`python build.py --deploy` copies `winmm.dll` into the game folder.

## Build

```bat
python build.py --test --install   # both halves + tests + stage
python build.py --deploy
python build.py --clean

set ANTHROPIC_API_KEY=sk-ant-...
build\install\bin\00_run_all.exe
```

Options: `--batch 50`, `--test` (1 batch), `--max-batches 4`, `--file mushi.spt`.

## Proxy DLL

10 patches (fontSize 24→14, spacing, line count via JE→JMP) + runtime text hook at the
text splitter function. Word wrapping at `MAX_LINE_CHARS=60`. Check `proxy_log.txt` for
diagnostics.
