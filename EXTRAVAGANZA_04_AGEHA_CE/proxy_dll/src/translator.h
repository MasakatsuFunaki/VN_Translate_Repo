// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Runtime translator: loads JP->EN translation_table.tsv and hooks the
// TextSplitter function at RVA 0x0FF140 to replace CP932 Japanese strings
// with English at runtime without modifying SPT files on disk.
#pragma once

#include <windows.h>

void TranslatorInit();
void TranslatorShutdown();
