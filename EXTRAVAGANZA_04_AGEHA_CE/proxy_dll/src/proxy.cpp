// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Complete winmm.dll proxy forwarding all 181 exports to System32\winmm.dll.
#include "proxy.h"

#include "log.h"

constexpr int kWinmmFuncCount = 181;

static HMODULE g_realWinmm = nullptr;
static FARPROC g_procs[kWinmmFuncCount] = {0};

static const char* const g_names[kWinmmFuncCount] = {
    "CloseDriver",
    "DefDriverProc",
    "DriverCallback",
    "DrvGetModuleHandle",
    "GetDriverModuleHandle",
    "OpenDriver",
    "PlaySound",
    "PlaySoundA",
    "PlaySoundW",
    "SendDriverMessage",
    "auxGetDevCapsA",
    "auxGetDevCapsW",
    "auxGetNumDevs",
    "auxGetVolume",
    "auxOutMessage",
    "auxSetVolume",
    "joyConfigChanged",
    "joyGetDevCapsA",
    "joyGetDevCapsW",
    "joyGetNumDevs",
    "joyGetPos",
    "joyGetPosEx",
    "joyGetThreshold",
    "joyReleaseCapture",
    "joySetCapture",
    "joySetThreshold",
    "mciDriverNotify",
    "mciDriverYield",
    "mciExecute",
    "mciFreeCommandResource",
    "mciGetCreatorTask",
    "mciGetDeviceIDA",
    "mciGetDeviceIDFromElementIDA",
    "mciGetDeviceIDFromElementIDW",
    "mciGetDeviceIDW",
    "mciGetDriverData",
    "mciGetErrorStringA",
    "mciGetErrorStringW",
    "mciGetYieldProc",
    "mciLoadCommandResource",
    "mciSendCommandA",
    "mciSendCommandW",
    "mciSendStringA",
    "mciSendStringW",
    "mciSetDriverData",
    "mciSetYieldProc",
    "midiConnect",
    "midiDisconnect",
    "midiInAddBuffer",
    "midiInClose",
    "midiInGetDevCapsA",
    "midiInGetDevCapsW",
    "midiInGetErrorTextA",
    "midiInGetErrorTextW",
    "midiInGetID",
    "midiInGetNumDevs",
    "midiInMessage",
    "midiInOpen",
    "midiInPrepareHeader",
    "midiInReset",
    "midiInStart",
    "midiInStop",
    "midiInUnprepareHeader",
    "midiOutCacheDrumPatches",
    "midiOutCachePatches",
    "midiOutClose",
    "midiOutGetDevCapsA",
    "midiOutGetDevCapsW",
    "midiOutGetErrorTextA",
    "midiOutGetErrorTextW",
    "midiOutGetID",
    "midiOutGetNumDevs",
    "midiOutGetVolume",
    "midiOutLongMsg",
    "midiOutMessage",
    "midiOutOpen",
    "midiOutPrepareHeader",
    "midiOutReset",
    "midiOutSetVolume",
    "midiOutShortMsg",
    "midiOutUnprepareHeader",
    "midiStreamClose",
    "midiStreamOpen",
    "midiStreamOut",
    "midiStreamPause",
    "midiStreamPosition",
    "midiStreamProperty",
    "midiStreamRestart",
    "midiStreamStop",
    "mixerClose",
    "mixerGetControlDetailsA",
    "mixerGetControlDetailsW",
    "mixerGetDevCapsA",
    "mixerGetDevCapsW",
    "mixerGetID",
    "mixerGetLineControlsA",
    "mixerGetLineControlsW",
    "mixerGetLineInfoA",
    "mixerGetLineInfoW",
    "mixerGetNumDevs",
    "mixerMessage",
    "mixerOpen",
    "mixerSetControlDetails",
    "mmDrvInstall",
    "mmGetCurrentTask",
    "mmTaskBlock",
    "mmTaskCreate",
    "mmTaskSignal",
    "mmTaskYield",
    "mmioAdvance",
    "mmioAscend",
    "mmioClose",
    "mmioCreateChunk",
    "mmioDescend",
    "mmioFlush",
    "mmioGetInfo",
    "mmioInstallIOProcA",
    "mmioInstallIOProcW",
    "mmioOpenA",
    "mmioOpenW",
    "mmioRead",
    "mmioRenameA",
    "mmioRenameW",
    "mmioSeek",
    "mmioSendMessage",
    "mmioSetBuffer",
    "mmioSetInfo",
    "mmioStringToFOURCCA",
    "mmioStringToFOURCCW",
    "mmioWrite",
    "mmsystemGetVersion",
    "sndPlaySoundA",
    "sndPlaySoundW",
    "timeBeginPeriod",
    "timeEndPeriod",
    "timeGetDevCaps",
    "timeGetSystemTime",
    "timeGetTime",
    "timeKillEvent",
    "timeSetEvent",
    "waveInAddBuffer",
    "waveInClose",
    "waveInGetDevCapsA",
    "waveInGetDevCapsW",
    "waveInGetErrorTextA",
    "waveInGetErrorTextW",
    "waveInGetID",
    "waveInGetNumDevs",
    "waveInGetPosition",
    "waveInMessage",
    "waveInOpen",
    "waveInPrepareHeader",
    "waveInReset",
    "waveInStart",
    "waveInStop",
    "waveInUnprepareHeader",
    "waveOutBreakLoop",
    "waveOutClose",
    "waveOutGetDevCapsA",
    "waveOutGetDevCapsW",
    "waveOutGetErrorTextA",
    "waveOutGetErrorTextW",
    "waveOutGetID",
    "waveOutGetNumDevs",
    "waveOutGetPitch",
    "waveOutGetPlaybackRate",
    "waveOutGetPosition",
    "waveOutGetVolume",
    "waveOutMessage",
    "waveOutOpen",
    "waveOutPause",
    "waveOutPrepareHeader",
    "waveOutReset",
    "waveOutRestart",
    "waveOutSetPitch",
    "waveOutSetPlaybackRate",
    "waveOutSetVolume",
    "waveOutUnprepareHeader",
    "waveOutWrite",
    "NotifyCallbackData",
    "WOW32DriverCallback",
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
    for (int i = 0; i < kWinmmFuncCount; ++i) {
        g_procs[i] = GetProcAddress(g_realWinmm, g_names[i]);
        if (g_procs[i]) ++resolved;
    }
    Log("Proxy: loaded real winmm.dll, resolved %d/%d exports", resolved, kWinmmFuncCount);
}

void ProxyShutdown() {
    if (g_realWinmm) {
        FreeLibrary(g_realWinmm);
        g_realWinmm = nullptr;
    }
}

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
