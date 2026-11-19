// Простой инжектор: находит PlantsVsZombies.exe и загружает в него coop.dll
// Сборка: Visual Studio, Release | x86 (Win32), консольное приложение.
// Запуск: injector.exe [путь_к_dll]   (по умолчанию coop.dll рядом с injector.exe)
// Сначала запусти игру, потом инжектор. Может понадобиться запуск от администратора.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

static DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

int wmain(int argc, wchar_t** argv) {
    wchar_t dllPath[MAX_PATH];
    if (argc > 1) {
        GetFullPathNameW(argv[1], MAX_PATH, dllPath, nullptr);
    }
    else {
        GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
        std::wstring p(dllPath);
        p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"coop.dll";
        wcscpy_s(dllPath, p.c_str());
    }
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"DLL not found: %s\n", dllPath);
        return 1;
    }

    // В Steam-версии PlantsVsZombies.exe — лаунчер, настоящая игра — popcapgame1.exe
    DWORD pid = FindProcess(L"popcapgame1.exe");
    if (!pid) pid = FindProcess(L"PlantsVsZombies.exe");
    if (!pid) { wprintf(L"Game process not found\n"); return 1; }
    wprintf(L"Found game, PID %lu\n", pid);

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { wprintf(L"OpenProcess failed: %lu\n", GetLastError()); return 1; }

    size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(proc, remote, dllPath, bytes, nullptr);

    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) { wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError()); return 1; }

    WaitForSingleObject(th, INFINITE);
    DWORD result = 0;
    GetExitCodeThread(th, &result);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(proc);

    wprintf(result ? L"Injected OK\n" : L"LoadLibrary failed inside game\n");
    return result ? 0 : 1;
}
