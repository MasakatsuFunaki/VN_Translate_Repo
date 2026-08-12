// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Cross-Channel runtime translator.
//
// Strategy: cc.exe is heavily packed (entry point in the last section,
// blanked section names, X/W/R everywhere) so static RE on the unpacked
// image yields nothing. We sidestep that by IAT-hooking GDI32 text
// APIs in cc.exe's import table — the IAT is in the (unpacked) PE
// header, so the OS loader fills it before any unpacker code runs.
// Whenever the engine calls ExtTextOutA / TextOutA with a CP932 byte
// buffer we look it up in translations.tsv and substitute the English
// version before forwarding to the real GDI function.

#include <cstdint>
#include <string>

void TranslatorInit();
void TranslatorShutdown();

// Reverse lookup used by the choice entry hook (BuildChoiceOurBuf).
// PatchScriptBuffer (the LZSS decompress hook) replaces JP with
// FitToSlot-truncated, space-padded EN before any choice opcode runs,
// so the bytes the entry hook sees are e.g. `"Go up to "`, not the
// original JP. This map is built at TSV-load time using the same
// FitToSlot() the patcher uses, so every form the patcher could have
// written maps back to the full EN.
bool Translator_PrefixLookup(const std::string& maybe_truncated_en,
                              std::string& full_en_out);
