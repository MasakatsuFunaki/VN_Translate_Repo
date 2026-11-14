// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "proxy.h"
#include "log.h"

// ── Complete winmm.dll proxy ───────────────────────────────────────────────
// Forwards ALL winmm.dll exports to the real System32 winmm.dll so that
// NVIDIA drivers, codecs, and other system DLLs that import winmm functions
// resolve correctly through our proxy.

static HMODULE g_realWinmm = nullptr;

// Function pointer table — one slot per forwarded winmm export.
// Index matches the WINMM_FUNC enum below.
#define WINMM_FUNC_COUNT 181

static FARPROC g_procs[WINMM_FUNC_COUNT] = {0};

// All exported winmm.dll function names, in alphabetical order.
static const char* g_names[WINMM_FUNC_COUNT] = {
    "CloseDriver",                    //   0
    "DefDriverProc",                  //   1
    "DriverCallback",                 //   2
    "DrvGetModuleHandle",             //   3
    "GetDriverModuleHandle",          //   4
    "OpenDriver",                     //   5
    "PlaySound",                      //   6
    "PlaySoundA",                     //   7
    "PlaySoundW",                     //   8
    "SendDriverMessage",              //   9
    "auxGetDevCapsA",                 //  10
    "auxGetDevCapsW",                 //  11
    "auxGetNumDevs",                  //  12
    "auxGetVolume",                   //  13
    "auxOutMessage",                  //  14
    "auxSetVolume",                   //  15
    "joyConfigChanged",               //  16
    "joyGetDevCapsA",                 //  17
    "joyGetDevCapsW",                 //  18
    "joyGetNumDevs",                  //  19
    "joyGetPos",                      //  20
    "joyGetPosEx",                    //  21
    "joyGetThreshold",                //  22
    "joyReleaseCapture",              //  23
    "joySetCapture",                  //  24
    "joySetThreshold",                //  25
    "mciDriverNotify",                //  26
    "mciDriverYield",                 //  27
    "mciExecute",                     //  28
    "mciFreeCommandResource",         //  29
    "mciGetCreatorTask",              //  30
    "mciGetDeviceIDA",                //  31
    "mciGetDeviceIDFromElementIDA",   //  32
    "mciGetDeviceIDFromElementIDW",   //  33
    "mciGetDeviceIDW",                //  34
    "mciGetDriverData",               //  35
    "mciGetErrorStringA",             //  36
    "mciGetErrorStringW",             //  37
    "mciGetYieldProc",                //  38
    "mciLoadCommandResource",         //  39
    "mciSendCommandA",                //  40
    "mciSendCommandW",                //  41
    "mciSendStringA",                 //  42
    "mciSendStringW",                 //  43
    "mciSetDriverData",               //  44
    "mciSetYieldProc",                //  45
    "midiConnect",                    //  46
    "midiDisconnect",                 //  47
    "midiInAddBuffer",                //  48
    "midiInClose",                    //  49
    "midiInGetDevCapsA",              //  50
    "midiInGetDevCapsW",              //  51
    "midiInGetErrorTextA",            //  52
    "midiInGetErrorTextW",            //  53
    "midiInGetID",                    //  54
    "midiInGetNumDevs",               //  55
    "midiInMessage",                  //  56
    "midiInOpen",                     //  57
    "midiInPrepareHeader",            //  58
    "midiInReset",                    //  59
    "midiInStart",                    //  60
    "midiInStop",                     //  61
    "midiInUnprepareHeader",          //  62
    "midiOutCacheDrumPatches",        //  63
    "midiOutCachePatches",            //  64
    "midiOutClose",                   //  65
    "midiOutGetDevCapsA",             //  66
    "midiOutGetDevCapsW",             //  67
    "midiOutGetErrorTextA",           //  68
    "midiOutGetErrorTextW",           //  69
    "midiOutGetID",                   //  70
    "midiOutGetNumDevs",              //  71
    "midiOutGetVolume",               //  72
    "midiOutLongMsg",                 //  73
    "midiOutMessage",                 //  74
    "midiOutOpen",                    //  75
    "midiOutPrepareHeader",           //  76
    "midiOutReset",                   //  77
    "midiOutSetVolume",               //  78
    "midiOutShortMsg",                //  79
    "midiOutUnprepareHeader",         //  80
    "midiStreamClose",                //  81
    "midiStreamOpen",                 //  82
    "midiStreamOut",                  //  83
    "midiStreamPause",                //  84
    "midiStreamPosition",             //  85
    "midiStreamProperty",             //  86
    "midiStreamRestart",              //  87
    "midiStreamStop",                 //  88
    "mixerClose",                     //  89
    "mixerGetControlDetailsA",        //  90
    "mixerGetControlDetailsW",        //  91
    "mixerGetDevCapsA",               //  92
    "mixerGetDevCapsW",               //  93
    "mixerGetID",                     //  94
    "mixerGetLineControlsA",          //  95
    "mixerGetLineControlsW",          //  96
    "mixerGetLineInfoA",              //  97
    "mixerGetLineInfoW",              //  98
    "mixerGetNumDevs",                //  99
    "mixerMessage",                   // 100
    "mixerOpen",                      // 101
    "mixerSetControlDetails",         // 102
    "mmDrvInstall",                   // 103
    "mmGetCurrentTask",               // 104
    "mmTaskBlock",                    // 105
    "mmTaskCreate",                   // 106
    "mmTaskSignal",                   // 107
    "mmTaskYield",                    // 108
    "mmioAdvance",                    // 109
    "mmioAscend",                     // 110
    "mmioClose",                      // 111
    "mmioCreateChunk",                // 112
    "mmioDescend",                    // 113
    "mmioFlush",                      // 114
    "mmioGetInfo",                    // 115
    "mmioInstallIOProcA",             // 116
    "mmioInstallIOProcW",             // 117
    "mmioOpenA",                      // 118
    "mmioOpenW",                      // 119
    "mmioRead",                       // 120
    "mmioRenameA",                    // 121
    "mmioRenameW",                    // 122
    "mmioSeek",                       // 123
    "mmioSendMessage",                // 124
    "mmioSetBuffer",                  // 125
    "mmioSetInfo",                    // 126
    "mmioStringToFOURCCA",            // 127
    "mmioStringToFOURCCW",            // 128
    "mmioWrite",                      // 129
    "mmsystemGetVersion",             // 130
    "sndPlaySoundA",                  // 131
    "sndPlaySoundW",                  // 132
    "timeBeginPeriod",                // 133
    "timeEndPeriod",                  // 134
    "timeGetDevCaps",                 // 135
    "timeGetSystemTime",              // 136
    "timeGetTime",                    // 137
    "timeKillEvent",                  // 138
    "timeSetEvent",                   // 139
    "waveInAddBuffer",                // 140
    "waveInClose",                    // 141
    "waveInGetDevCapsA",              // 142
    "waveInGetDevCapsW",              // 143
    "waveInGetErrorTextA",            // 144
    "waveInGetErrorTextW",            // 145
    "waveInGetID",                    // 146
    "waveInGetNumDevs",               // 147
    "waveInGetPosition",              // 148
    "waveInMessage",                  // 149
    "waveInOpen",                     // 150
    "waveInPrepareHeader",            // 151
    "waveInReset",                    // 152
    "waveInStart",                    // 153
    "waveInStop",                     // 154
    "waveInUnprepareHeader",          // 155
    "waveOutBreakLoop",               // 156
    "waveOutClose",                   // 157
    "waveOutGetDevCapsA",             // 158
    "waveOutGetDevCapsW",             // 159
    "waveOutGetErrorTextA",           // 160
    "waveOutGetErrorTextW",           // 161
    "waveOutGetID",                   // 162
    "waveOutGetNumDevs",              // 163
    "waveOutGetPitch",                // 164
    "waveOutGetPlaybackRate",         // 165
    "waveOutGetPosition",             // 166
    "waveOutGetVolume",               // 167
    "waveOutMessage",                 // 168
    "waveOutOpen",                    // 169
    "waveOutPause",                   // 170
    "waveOutPrepareHeader",           // 171
    "waveOutReset",                   // 172
    "waveOutRestart",                 // 173
    "waveOutSetPitch",                // 174
    "waveOutSetPlaybackRate",         // 175
    "waveOutSetVolume",               // 176
    "waveOutUnprepareHeader",         // 177
    "waveOutWrite",                   // 178
    "NotifyCallbackData",             // 179
    "WOW32DriverCallback",            // 180
};

void ProxyInit() {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wcscat_s(sysDir, L"\\winmm.dll");

    g_realWinmm = LoadLibraryW(sysDir);
    if (!g_realWinmm) {
        Log("FATAL: Could not load real winmm.dll from %ls", sysDir);
        return;
    }

    int resolved = 0;
    for (int i = 0; i < WINMM_FUNC_COUNT; i++) {
        g_procs[i] = GetProcAddress(g_realWinmm, g_names[i]);
        if (g_procs[i]) resolved++;
    }
    Log("Proxy: loaded real winmm.dll, resolved %d/%d exports", resolved, WINMM_FUNC_COUNT);
}

void ProxyShutdown() {
    if (g_realWinmm) {
        FreeLibrary(g_realWinmm);
        g_realWinmm = nullptr;
    }
}

// ── Naked forwarding stubs ─────────────────────────────────────────────────
// Each stub just jumps through the corresponding function pointer.
// __declspec(naked) avoids any prologue/epilogue so the call frame is preserved.

#define PROXY_STUB(name, idx) \
    extern "C" __declspec(naked) void __stdcall proxy_##name() { \
        __asm { jmp dword ptr [g_procs + idx * 4] } \
    }

PROXY_STUB(CloseDriver, 0)
PROXY_STUB(DefDriverProc, 1)
PROXY_STUB(DriverCallback, 2)
PROXY_STUB(DrvGetModuleHandle, 3)
PROXY_STUB(GetDriverModuleHandle, 4)
PROXY_STUB(OpenDriver, 5)
PROXY_STUB(PlaySound, 6)
PROXY_STUB(PlaySoundA, 7)
PROXY_STUB(PlaySoundW, 8)
PROXY_STUB(SendDriverMessage, 9)
PROXY_STUB(auxGetDevCapsA, 10)
PROXY_STUB(auxGetDevCapsW, 11)
PROXY_STUB(auxGetNumDevs, 12)
PROXY_STUB(auxGetVolume, 13)
PROXY_STUB(auxOutMessage, 14)
PROXY_STUB(auxSetVolume, 15)
PROXY_STUB(joyConfigChanged, 16)
PROXY_STUB(joyGetDevCapsA, 17)
PROXY_STUB(joyGetDevCapsW, 18)
PROXY_STUB(joyGetNumDevs, 19)
PROXY_STUB(joyGetPos, 20)
PROXY_STUB(joyGetPosEx, 21)
PROXY_STUB(joyGetThreshold, 22)
PROXY_STUB(joyReleaseCapture, 23)
PROXY_STUB(joySetCapture, 24)
PROXY_STUB(joySetThreshold, 25)
PROXY_STUB(mciDriverNotify, 26)
PROXY_STUB(mciDriverYield, 27)
PROXY_STUB(mciExecute, 28)
PROXY_STUB(mciFreeCommandResource, 29)
PROXY_STUB(mciGetCreatorTask, 30)
PROXY_STUB(mciGetDeviceIDA, 31)
PROXY_STUB(mciGetDeviceIDFromElementIDA, 32)
PROXY_STUB(mciGetDeviceIDFromElementIDW, 33)
PROXY_STUB(mciGetDeviceIDW, 34)
PROXY_STUB(mciGetDriverData, 35)
PROXY_STUB(mciGetErrorStringA, 36)
PROXY_STUB(mciGetErrorStringW, 37)
PROXY_STUB(mciGetYieldProc, 38)
PROXY_STUB(mciLoadCommandResource, 39)
PROXY_STUB(mciSendCommandA, 40)
PROXY_STUB(mciSendCommandW, 41)
PROXY_STUB(mciSendStringA, 42)
PROXY_STUB(mciSendStringW, 43)
PROXY_STUB(mciSetDriverData, 44)
PROXY_STUB(mciSetYieldProc, 45)
PROXY_STUB(midiConnect, 46)
PROXY_STUB(midiDisconnect, 47)
PROXY_STUB(midiInAddBuffer, 48)
PROXY_STUB(midiInClose, 49)
PROXY_STUB(midiInGetDevCapsA, 50)
PROXY_STUB(midiInGetDevCapsW, 51)
PROXY_STUB(midiInGetErrorTextA, 52)
PROXY_STUB(midiInGetErrorTextW, 53)
PROXY_STUB(midiInGetID, 54)
PROXY_STUB(midiInGetNumDevs, 55)
PROXY_STUB(midiInMessage, 56)
PROXY_STUB(midiInOpen, 57)
PROXY_STUB(midiInPrepareHeader, 58)
PROXY_STUB(midiInReset, 59)
PROXY_STUB(midiInStart, 60)
PROXY_STUB(midiInStop, 61)
PROXY_STUB(midiInUnprepareHeader, 62)
PROXY_STUB(midiOutCacheDrumPatches, 63)
PROXY_STUB(midiOutCachePatches, 64)
PROXY_STUB(midiOutClose, 65)
PROXY_STUB(midiOutGetDevCapsA, 66)
PROXY_STUB(midiOutGetDevCapsW, 67)
PROXY_STUB(midiOutGetErrorTextA, 68)
PROXY_STUB(midiOutGetErrorTextW, 69)
PROXY_STUB(midiOutGetID, 70)
PROXY_STUB(midiOutGetNumDevs, 71)
PROXY_STUB(midiOutGetVolume, 72)
PROXY_STUB(midiOutLongMsg, 73)
PROXY_STUB(midiOutMessage, 74)
PROXY_STUB(midiOutOpen, 75)
PROXY_STUB(midiOutPrepareHeader, 76)
PROXY_STUB(midiOutReset, 77)
PROXY_STUB(midiOutSetVolume, 78)
PROXY_STUB(midiOutShortMsg, 79)
PROXY_STUB(midiOutUnprepareHeader, 80)
PROXY_STUB(midiStreamClose, 81)
PROXY_STUB(midiStreamOpen, 82)
PROXY_STUB(midiStreamOut, 83)
PROXY_STUB(midiStreamPause, 84)
PROXY_STUB(midiStreamPosition, 85)
PROXY_STUB(midiStreamProperty, 86)
PROXY_STUB(midiStreamRestart, 87)
PROXY_STUB(midiStreamStop, 88)
PROXY_STUB(mixerClose, 89)
PROXY_STUB(mixerGetControlDetailsA, 90)
PROXY_STUB(mixerGetControlDetailsW, 91)
PROXY_STUB(mixerGetDevCapsA, 92)
PROXY_STUB(mixerGetDevCapsW, 93)
PROXY_STUB(mixerGetID, 94)
PROXY_STUB(mixerGetLineControlsA, 95)
PROXY_STUB(mixerGetLineControlsW, 96)
PROXY_STUB(mixerGetLineInfoA, 97)
PROXY_STUB(mixerGetLineInfoW, 98)
PROXY_STUB(mixerGetNumDevs, 99)
PROXY_STUB(mixerMessage, 100)
PROXY_STUB(mixerOpen, 101)
PROXY_STUB(mixerSetControlDetails, 102)
PROXY_STUB(mmDrvInstall, 103)
PROXY_STUB(mmGetCurrentTask, 104)
PROXY_STUB(mmTaskBlock, 105)
PROXY_STUB(mmTaskCreate, 106)
PROXY_STUB(mmTaskSignal, 107)
PROXY_STUB(mmTaskYield, 108)
PROXY_STUB(mmioAdvance, 109)
PROXY_STUB(mmioAscend, 110)
PROXY_STUB(mmioClose, 111)
PROXY_STUB(mmioCreateChunk, 112)
PROXY_STUB(mmioDescend, 113)
PROXY_STUB(mmioFlush, 114)
PROXY_STUB(mmioGetInfo, 115)
PROXY_STUB(mmioInstallIOProcA, 116)
PROXY_STUB(mmioInstallIOProcW, 117)
PROXY_STUB(mmioOpenA, 118)
PROXY_STUB(mmioOpenW, 119)
PROXY_STUB(mmioRead, 120)
PROXY_STUB(mmioRenameA, 121)
PROXY_STUB(mmioRenameW, 122)
PROXY_STUB(mmioSeek, 123)
PROXY_STUB(mmioSendMessage, 124)
PROXY_STUB(mmioSetBuffer, 125)
PROXY_STUB(mmioSetInfo, 126)
PROXY_STUB(mmioStringToFOURCCA, 127)
PROXY_STUB(mmioStringToFOURCCW, 128)
PROXY_STUB(mmioWrite, 129)
PROXY_STUB(mmsystemGetVersion, 130)
PROXY_STUB(sndPlaySoundA, 131)
PROXY_STUB(sndPlaySoundW, 132)
PROXY_STUB(timeBeginPeriod, 133)
PROXY_STUB(timeEndPeriod, 134)
PROXY_STUB(timeGetDevCaps, 135)
PROXY_STUB(timeGetSystemTime, 136)
PROXY_STUB(timeGetTime, 137)
PROXY_STUB(timeKillEvent, 138)
PROXY_STUB(timeSetEvent, 139)
PROXY_STUB(waveInAddBuffer, 140)
PROXY_STUB(waveInClose, 141)
PROXY_STUB(waveInGetDevCapsA, 142)
PROXY_STUB(waveInGetDevCapsW, 143)
PROXY_STUB(waveInGetErrorTextA, 144)
PROXY_STUB(waveInGetErrorTextW, 145)
PROXY_STUB(waveInGetID, 146)
PROXY_STUB(waveInGetNumDevs, 147)
PROXY_STUB(waveInGetPosition, 148)
PROXY_STUB(waveInMessage, 149)
PROXY_STUB(waveInOpen, 150)
PROXY_STUB(waveInPrepareHeader, 151)
PROXY_STUB(waveInReset, 152)
PROXY_STUB(waveInStart, 153)
PROXY_STUB(waveInStop, 154)
PROXY_STUB(waveInUnprepareHeader, 155)
PROXY_STUB(waveOutBreakLoop, 156)
PROXY_STUB(waveOutClose, 157)
PROXY_STUB(waveOutGetDevCapsA, 158)
PROXY_STUB(waveOutGetDevCapsW, 159)
PROXY_STUB(waveOutGetErrorTextA, 160)
PROXY_STUB(waveOutGetErrorTextW, 161)
PROXY_STUB(waveOutGetID, 162)
PROXY_STUB(waveOutGetNumDevs, 163)
PROXY_STUB(waveOutGetPitch, 164)
PROXY_STUB(waveOutGetPlaybackRate, 165)
PROXY_STUB(waveOutGetPosition, 166)
PROXY_STUB(waveOutGetVolume, 167)
PROXY_STUB(waveOutMessage, 168)
PROXY_STUB(waveOutOpen, 169)
PROXY_STUB(waveOutPause, 170)
PROXY_STUB(waveOutPrepareHeader, 171)
PROXY_STUB(waveOutReset, 172)
PROXY_STUB(waveOutRestart, 173)
PROXY_STUB(waveOutSetPitch, 174)
PROXY_STUB(waveOutSetPlaybackRate, 175)
PROXY_STUB(waveOutSetVolume, 176)
PROXY_STUB(waveOutUnprepareHeader, 177)
PROXY_STUB(waveOutWrite, 178)
PROXY_STUB(NotifyCallbackData, 179)
PROXY_STUB(WOW32DriverCallback, 180)
