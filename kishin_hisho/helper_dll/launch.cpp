// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 32-bit launcher — same architecture as the target, so LoadLibraryA address
// is identical in both processes. Spawns KishinHisho.exe suspended, injects
// our DLL, resumes, waits, prints log path.
#include <windows.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const char* exe = (argc > 1) ? argv[1]
        : "C:\\Program Files (x86)\\NitroPlus\\KishinHisho\\KishinHisho.exe";

    // Helper DLL must sit next to launch.exe (we pass an absolute path).
    char dllPath[MAX_PATH];
    GetModuleFileNameA(NULL, dllPath, MAX_PATH);
    char* slash = strrchr(dllPath, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - dllPath), "alpharom_helper.dll");

    // The DLL writes alpharom_helper.log into the GAME's folder (next to
    // KishinHisho.exe), not next to itself. Compute that path so we can
    // wipe a stale log and tell the user where to look.
    char logPath[MAX_PATH];
    strcpy_s(logPath, MAX_PATH, exe);
    char* exeSlash = strrchr(logPath, '\\');
    if (exeSlash) strcpy_s(exeSlash + 1, MAX_PATH - (exeSlash + 1 - logPath), "alpharom_helper.log");
    DeleteFileA(logPath);

    printf("[*] EXE: %s\n", exe);
    printf("[*] DLL: %s\n", dllPath);
    printf("[*] LOG: %s\n", logPath);
    printf("    (if you can't find the log, UAC may have redirected it to\n"
           "     %%LOCALAPPDATA%%\\VirtualStore\\... -- check there)\n");

    char workDir[MAX_PATH];
    strcpy_s(workDir, MAX_PATH, exe);
    char* s2 = strrchr(workDir, '\\');
    if (s2) *s2 = '\0';

    char cmdLine[MAX_PATH * 2];
    sprintf_s(cmdLine, MAX_PATH * 2, "\"%s\"", exe);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(exe, cmdLine, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, workDir, &si, &pi)) {
        printf("CreateProcessA failed: %lu\n", GetLastError());
        return 1;
    }
    printf("[*] PID=%lu (suspended)\n", pi.dwProcessId);

    // Allocate + write DLL path in target
    SIZE_T pathLen = strlen(dllPath) + 1;
    LPVOID remote = VirtualAllocEx(pi.hProcess, NULL, pathLen,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("VirtualAllocEx failed\n"); TerminateProcess(pi.hProcess,1); return 2; }
    SIZE_T written = 0;
    WriteProcessMemory(pi.hProcess, remote, dllPath, pathLen, &written);

    // Same architecture -> same LoadLibraryA address
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    printf("[*] LoadLibraryA = %p, remote DLL path = %p\n", loadLib, remote);

    HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0,
                                   (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
    if (!th) { printf("CreateRemoteThread failed\n"); TerminateProcess(pi.hProcess,1); return 3; }
    WaitForSingleObject(th, INFINITE);
    DWORD hmod = 0;
    GetExitCodeThread(th, &hmod);
    CloseHandle(th);
    printf("[*] LoadLibraryA returned 0x%08lX (HMODULE)\n", hmod);
    if (hmod == 0) {
        printf("[!] DLL failed to load.\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return 4;
    }

    printf("[*] Resuming target main thread...\n");
    ResumeThread(pi.hThread);

    // Wait until the game process exits naturally (no timeout — let the user
    // play; we're a launcher, not a watchdog).
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    printf("[*] Process exited with code 0x%08lX\n", ec);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Print log
    printf("\n========== alpharom_helper.log ==========\n");
    FILE* f = NULL;
    fopen_s(&f, logPath, "r");
    if (!f) { printf("(no log file)\n"); return 0; }
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf)-1, f)) { buf[n]=0; fputs(buf, stdout); }
    fclose(f);
    return 0;
}
