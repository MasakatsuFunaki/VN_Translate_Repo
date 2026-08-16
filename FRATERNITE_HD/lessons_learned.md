# VN Translation — Lessons Learned

Checklist for wiring up a runtime translator for a new JP visual novel.

## Rendering

- **Don't replace text inside the engine's DC if the game uses DirectDraw or Direct3D.** Ship a transparent top-most overlay window and draw EN there; in-DC replacement gets clipped or overwritten by the GPU compositor.
- **If TextOutA always has `x=y=0` and the DC type is a memory DC, the engine is staging to an off-screen bitmap.** Treat the TextOutA hook as read-only JP capture and stop hunting for viewport/world transforms.
- **Engines render each glyph multiple times per frame (shadow/outline/fill).** Dedup by "same glyph as last hit" but detect a pass-identifying signal (e.g. differing return-address on the stack) so legit consecutive duplicate characters aren't eaten.

## Translation prompt

- **Never tell the LLM to "preserve the meaning" of silence or symbol-only lines** — it will invent explanatory prose. Write the rule as *symbol-only stays symbol-only* with one concrete before/after example.
- **Keep CP932 / fullwidth punctuation in the English output.** Converting `…`, `「`, `」` to ASCII breaks byte-for-byte overlay matching and signals to the LLM that symbols are disposable.
