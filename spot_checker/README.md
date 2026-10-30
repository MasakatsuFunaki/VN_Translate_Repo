# `spot_checker` — Statistical Hotspot & Surgical Visual Novel Translation Spot-Checker (`gemini-4-argon`)

`spot_checker` is a standalone, reusable C++17 subproject that scans an entire visual novel script (`translated_text.json`, 38,000–55,000+ lines) in milliseconds using a mathematical hotspot detection algorithm and dispatches surgical sliding-window calls to `gemini-4-argon` (`thinkingBudget: 2048`, `responseMimeType: "application/json"`).

## Key Features

1. **Literature-Backed Full-Novel Statistical & Discourse Hotspot Detection (`HotspotDetector`, PIMPL)**:
   - **Gale & Church (1993) + Pellegrino et al. (2011) Script-Aware Information-Load Residual ($Z_{\text{info}}$)**: Weights Japanese source characters by script information density (Kanji = `2.4` bits, Katakana = `1.4` bits, Hiragana/ASCII = `1.0` bit) with variance scaling $\sqrt{\max(10, B_S) / \bar{B}_S}$ and a generous literary leeway deadband ($|\delta_{\text{GC}}| > 3.0$) so natural English phrasing is never penalized.
   - **Artifact, Romaji Gloss & Quote Mismatch Score ($s_{\text{artifact}}(i)$)**: Detects leftover CJK/kana, fullwidth punctuation (`「`, `」`, `……`), inline romaji notes (`(hippu)`), and spoken-quote mismatches.
   - **Six Document-Level JP->EN Discourse Trap Signals ($s_{\text{coupling}}(i)$ / $s_{\text{style}}(i)$)**:
     1. *Directional Benefactive (`てくれる/てもらう/くださる`) & Overt 1st-Person (`俺は/僕は/私は/あたしは`) Contradiction* (Nakaiwa & Ikehara 1992; Voita et al. ACL 2019; Shimazu et al. LREC 2020).
     2. *Question–Answer Adjacency Pair & Elliptical Verb-Echo Mismatch* (`脳割れてるんじゃない？` -> `割れてる。`, Bawden et al. NAACL 2018).
     3. *Cross-Turn Lexical Cohesion (`BlonDe` Hook-and-Eye)*: 5-turn lookback ($W_{\text{lex}}=5$) for repeated $\ge 2$-kanji compounds with zero shared English content stems (Jiang et al. NAACL 2022).
     4. *Kinsui Role-Language (`Yakuwarigo`: `にゃ`, `かにゃ`, `ですわ`, `でござる`) & Per-Speaker Register EWMA Drift* (Kinsui 2003).
     5. *Translationese Calques, Yule's $K$ Content-Word Repetition Degeneracy & Shannon Entropy Divergence* (Volansky et al. 2015; Koppel & Ordan 2011; Toral 2019).
   - **Page's CUSUM (1954) Change-Point Detection + Gaussian KDE & Non-Maximum Suppression (NMS)**: Accumulates sequential regime-shift evidence $S_i = \max(0, S_{i-1} + s_i - (\mu_s + k_{\text{drift}}))$ alongside Gaussian KDE ($\sigma = W/4$) and selects disjoint windows ranked by standardized energy + CUSUM peak.
   - **Hybrid Adaptive Contagion**: Automatically enqueues adjacent windows $[a-W, a)$ and $[a+W, a+2W)$ when empirical LLM defect density $\hat{p}(w) \ge 0.12$ or fixes occur within 5 lines of a window boundary.

2. **Two-Stage Hybrid Architecture (`SpotChecker`, PIMPL)**:
   - **Stage 1 — Packed LLM Radar Pass (`--radar-calls <N>`)**: Packs up to `6` candidate passages per Radar API call for rapid GEMBA-MQM triage (`critical=25`, `major=5`, `minor=1`, Kocmi & Federmann WMT 2023), skipping clean passages (`mqm_score == 0`) and prioritizing high-MQM passages with pinpointed `suspect_ids`.
   - **Stage 2 — Surgical Span-Grounded Repair & Three Anti-Regression "Do-No-Harm" Mechanisms**:
     - **Trick 1 — Pinpointed Suspect-Only Locking (`editable_ids`)**: Unlocks only lines with $s_i > 0$ or Radar `suspect_ids` (plus $\pm 1$ adjacent banter partners) inside each window while locking clean lines as `[READ-ONLY CONTEXT]`.
     - **Trick 2 — Strict Defect Taxonomy, GEMBA-MQM Severity & Confidence Gate (`confidence >= 0.85`)**: Enforces the 6 defect categories (`mistranslation`, `broken_callback`, `artifact_or_note`, `character_voice`, `censored_or_westernized`, `stiff_translationese`), `severity` (`critical|major|minor`), optional `error_span`, and rejects fixes below `--min-confidence`.
     - **Trick 3 — Deterministic C++ Post-Edit Validator (`validate_proposed_fix`)**: Verifies GEMBA-MQM `error_span` removal, quote envelope invariance, zero leftover CJK/fullwidth punctuation/romaji glosses, engine placeholder preservation (`%s`, `%d`, `\n`), `TONE_OF_NOVEL.md` glossary term preservation, and strict mathematical non-regression ($s(\text{jp}, \text{new\_en}) \le s(\text{jp}, \text{old\_en})$).

3. **The Literary Sweet Spot Editorial Mandate**:
   - Built-in editorial rules combined with `<game_dir>/TONE_OF_NOVEL.md` enforce natural literary prose, author intent, non-Westernization, and zero censorship while avoiding both wooden literalism and over-localized tone drift.

## Building & Running Tests

```powershell
python spot_checker/build.py --test --install
```

## CLI Usage (`01_spot_check.exe`)

```powershell
# Statistical hotspot mode (default): scan entire novel and check top K hotspot windows
.\spot_checker\build\install\bin\01_spot_check.exe --game CROSS_CHANNEL --max-windows 1

# Linear mode: check sequential windows starting at a specific entry offset
.\spot_checker\build\install\bin\01_spot_check.exe --game CROSS_CHANNEL --mode linear --offset 120 --window 40 --max-windows 1

# Apply saved improvements in argon_spot_check.json to translated_text.json and caches (0 API calls)
.\spot_checker\build\install\bin\01_spot_check.exe --game CROSS_CHANNEL --apply
```
