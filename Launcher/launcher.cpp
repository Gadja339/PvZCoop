// PvZ Co-op — лаунчер
// Окно с настройками сети, кнопка «Играть»: сохраняет coop.ini, запускает игру через Steam,
// дожидается окна игры и загружает в неё Coop.dll (лежит рядом с лаунчером).
// Сборка: Visual Studio, Release | x86 (Win32), приложение Windows (/SUBSYSTEM:WINDOWS), /MT.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cwchar>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t* kVersion   = L"1.0";
static const wchar_t* kGameExe   = L"popcapgame1.exe";        // настоящая игра (Steam GOTY)
static const wchar_t* kSteamRun  = L"steam://rungameid/3590"; // Plants vs. Zombies GOTY
static const wchar_t* kDllName   = L"Coop.dll";

enum {
    ID_HOST = 101, ID_CLIENT, ID_ADDR, ID_PORT, ID_DELAY, ID_HINT, ID_PLAY, ID_LOG, ID_AUTOCLOSE,
    ID_ADDR_LABEL, ID_DELAY_LABEL
};
enum { WM_APP_LOG = WM_APP + 1, WM_APP_DONE };

static HWND  g_wnd = nullptr;
static HFONT g_font = nullptr, g_fontBig = nullptr;
static std::wstring g_dir, g_ini, g_dll;
static volatile bool g_busy = false;

// ---------------------------------------------------------------------------
// Вспомогательное
// ---------------------------------------------------------------------------

static void Log(const std::wstring& s) {
    PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(s));
}

static std::wstring GetText(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    // убрать пробелы по краям
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

static bool IsHost() { return SendDlgItemMessageW(g_wnd, ID_HOST, BM_GETCHECK, 0, 0) == BST_CHECKED; }

static std::wstring LocalIPv4List() {
    std::wstring out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size) != NO_ERROR) return out;
    }
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            wchar_t ip[64] = {};
            DWORD n = 64;
            if (WSAAddressToStringW(u->Address.lpSockaddr, u->Address.iSockaddrLength, nullptr, ip, &n) == 0) {
                if (wcsncmp(ip, L"169.254.", 8) == 0) continue;   // адрес без сети
                if (!out.empty()) out += L", ";
                out += ip;
            }
        }
    }
    return out;
}

static void UpdateHint() {
    bool host = IsHost();
    EnableWindow(GetDlgItem(g_wnd, ID_ADDR), !host);
    EnableWindow(GetDlgItem(g_wnd, ID_ADDR_LABEL), !host);
    EnableWindow(GetDlgItem(g_wnd, ID_DELAY), host);
    EnableWindow(GetDlgItem(g_wnd, ID_DELAY_LABEL), host);
    std::wstring hint;
    if (host) {
        std::wstring ips = LocalIPv4List();
        hint = L"Ты создаёшь игру. Друг подключается к тебе.\r\n";
        hint += L"Твой адрес в локальной сети: " + (ips.empty() ? std::wstring(L"не найден") : ips) + L"\r\n";
        hint += L"Через Porthole: раздай TCP-порт " + GetText(ID_PORT) + L".";
    } else {
        hint = L"Ты подключаешься к другу.\r\n";
        hint += L"Локальная сеть: адрес хоста (192.168.x.x), порт как у хоста.\r\n";
        hint += L"Через Porthole: 127.0.0.1 и порт, который показывает Porthole.";
    }
    SetDlgItemTextW(g_wnd, ID_HINT, hint.c_str());
}

// ---------------------------------------------------------------------------
// coop.ini
// ---------------------------------------------------------------------------

static void LoadSettings() {
    wchar_t mode[32] = {}, host[128] = {};
    GetPrivateProfileStringW(L"net", L"mode", L"host", mode, 32, g_ini.c_str());
    GetPrivateProfileStringW(L"net", L"host", L"127.0.0.1", host, 128, g_ini.c_str());
    int port  = GetPrivateProfileIntW(L"net", L"port", 34567, g_ini.c_str());
    int delay = GetPrivateProfileIntW(L"net", L"delay", 10, g_ini.c_str());

    bool client = _wcsicmp(mode, L"client") == 0;
    CheckRadioButton(g_wnd, ID_HOST, ID_CLIENT, client ? ID_CLIENT : ID_HOST);
    SetDlgItemTextW(g_wnd, ID_ADDR, host);
    SetDlgItemInt(g_wnd, ID_PORT, port, FALSE);
    int sel = delay <= 10 ? 0 : delay <= 15 ? 1 : delay <= 20 ? 2 : 3;
    SendDlgItemMessageW(g_wnd, ID_DELAY, CB_SETCURSEL, sel, 0);
}

static int DelayFromCombo() {
    static const int values[] = { 10, 15, 20, 30 };
    LRESULT i = SendDlgItemMessageW(g_wnd, ID_DELAY, CB_GETCURSEL, 0, 0);
    return (i >= 0 && i < 4) ? values[i] : 10;
}

static bool SaveSettings() {
    std::wstring addr = GetText(ID_ADDR);
    BOOL ok = FALSE;
    UINT port = GetDlgItemInt(g_wnd, ID_PORT, &ok, FALSE);
    if (!ok || port == 0 || port > 65535) {
        MessageBoxW(g_wnd, L"Порт должен быть числом от 1 до 65535 (обычно 34567).", L"PvZ Co-op", MB_ICONWARNING);
        return false;
    }
    if (!IsHost() && addr.empty()) {
        MessageBoxW(g_wnd, L"Укажи адрес хоста.", L"PvZ Co-op", MB_ICONWARNING);
        return false;
    }
    const wchar_t* ini = g_ini.c_str();
    WritePrivateProfileStringW(L"net", L"; mode", L"off | host | client", ini);
    WritePrivateProfileStringW(L"net", L"mode", IsHost() ? L"host" : L"client", ini);
    if (!addr.empty()) WritePrivateProfileStringW(L"net", L"host", addr.c_str(), ini);
    WritePrivateProfileStringW(L"net", L"port", std::to_wstring(port).c_str(), ini);
    WritePrivateProfileStringW(L"net", L"delay", std::to_wstring(DelayFromCombo()).c_str(), ini);
    return true;
}

// ---------------------------------------------------------------------------
// Игра: поиск процесса, окна, внедрение
// ---------------------------------------------------------------------------

static DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static bool HasModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof(me) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        if (_wcsicmp(me.szModule, name) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

struct FindWnd { DWORD pid; bool found; };
static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* f = (FindWnd*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) { f->found = true; return FALSE; }
    return TRUE;
}
static bool HasVisibleWindow(DWORD pid) {
    FindWnd f{ pid, false };
    EnumWindows(EnumProc, (LPARAM)&f);
    return f.found;
}

// 0 = успех, иначе текст ошибки
static const wchar_t* Inject(DWORD pid, const std::wstring& dll) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return L"нет доступа к процессу игры. Запусти лаунчер от имени администратора.";
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(proc); return L"не удалось выделить память в игре."; }
    WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr);
    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return L"не удалось запустить загрузку в игре (антивирус?).";
    }
    WaitForSingleObject(th, 15000);
    DWORD result = 0;
    GetExitCodeThread(th, &result);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(proc);
    return result ? nullptr : L"игра не смогла загрузить Coop.dll.";
}

static DWORD WINAPI PlayThread(LPVOID) {
    DWORD pid = FindProcess(kGameExe);
    if (pid && HasModule(pid, kDllName)) {
        Log(L"Мод уже загружен в запущенную игру.");
        Log(L"Чтобы применить новые настройки, закрой игру и нажми «Играть» ещё раз.");
        PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
        return 0;
    }
    if (!pid) {
        Log(L"Запускаю игру через Steam...");
        HINSTANCE r = ShellExecuteW(nullptr, L"open", kSteamRun, nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) Log(L"Steam не запустился сам — запусти игру вручную, я подожду.");
        Log(L"Жду игру (до 2 минут)...");
        for (int i = 0; i < 240 && !pid; ++i) {
            Sleep(500);
            pid = FindProcess(kGameExe);
        }
        if (!pid) {
            Log(L"Игра так и не запустилась. Запусти её и нажми «Играть» ещё раз.");
            PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
            return 0;
        }
    } else {
        Log(L"Игра уже запущена.");
    }
    // Ждём окно игры: к этому моменту игра успевает подготовиться
    for (int i = 0; i < 120 && !HasVisibleWindow(pid); ++i) Sleep(500);
    Sleep(1500);
    if (!FindProcess(kGameExe)) {
        Log(L"Игра закрылась до загрузки мода.");
        PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
        return 0;
    }
    Log(L"Загружаю мод в игру...");
    if (const wchar_t* err = Inject(pid, g_dll)) {
        Log(std::wstring(L"Ошибка: ") + err);
        PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
        return 0;
    }
    Log(L"Готово! Мод загружен — рядом с игрой откроется окно мода.");
    Log(IsHost() ? L"Жди, пока друг подключится, потом оба открывайте один и тот же уровень."
                 : L"Когда в окне мода появится «session seed», открывай тот же уровень, что и хост.");
    PostMessageW(g_wnd, WM_APP_DONE, 1, 0);
    return 0;
}

static void OnPlay() {
    if (g_busy) return;
    if (GetFileAttributesW(g_dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_wnd, (L"Не найден " + std::wstring(kDllName) + L" рядом с лаунчером:\n" + g_dll).c_str(),
                    L"PvZ Co-op", MB_ICONERROR);
        return;
    }
    if (!SaveSettings()) return;
    g_busy = true;
    EnableWindow(GetDlgItem(g_wnd, ID_PLAY), FALSE);
    SetDlgItemTextW(g_wnd, ID_LOG, L"");
    Log(IsHost() ? L"Режим: хост, порт " + GetText(ID_PORT) + L", задержка " + std::to_wstring(DelayFromCombo())
                 : L"Режим: подключение к " + GetText(ID_ADDR) + L":" + GetText(ID_PORT));
    HANDLE t = CreateThread(nullptr, 0, PlayThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// ---------------------------------------------------------------------------
// Окно
// ---------------------------------------------------------------------------

static HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
                DWORD exStyle = 0, HFONT font = nullptr) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    return c;
}

static void CreateControls() {
    std::wstring title = L"PvZ Co-op  v" + std::wstring(kVersion);
    Add(L"STATIC", title.c_str(), 0, 16, 12, 400, 28, 0, 0, g_fontBig);

    Add(L"BUTTON", L"Я хост (создаю игру)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 16, 50, 200, 22, ID_HOST);
    Add(L"BUTTON", L"Подключаюсь к другу", BS_AUTORADIOBUTTON, 230, 50, 200, 22, ID_CLIENT);

    Add(L"STATIC", L"Адрес хоста:", 0, 16, 88, 120, 20, ID_ADDR_LABEL);
    Add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, 140, 85, 180, 24, ID_ADDR, WS_EX_CLIENTEDGE);
    Add(L"STATIC", L"Порт:", 0, 335, 88, 40, 20, 0);
    Add(L"EDIT", L"", ES_NUMBER | WS_TABSTOP, 375, 85, 70, 24, ID_PORT, WS_EX_CLIENTEDGE);

    Add(L"STATIC", L"Задержка:", 0, 16, 122, 120, 20, ID_DELAY_LABEL);
    HWND cb = Add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 140, 119, 305, 200, ID_DELAY);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"10 ходов — локальная сеть");
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"15 ходов");
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"20 ходов — интернет / Porthole");
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"30 ходов — плохая связь");

    Add(L"STATIC", L"", 0, 16, 156, 430, 60, ID_HINT);

    Add(L"BUTTON", L"Играть", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 224, 160, 40, ID_PLAY, 0, g_fontBig);
    HWND ac = Add(L"BUTTON", L"Закрыть лаунчер после запуска", BS_AUTOCHECKBOX | WS_TABSTOP, 196, 233, 250, 22, ID_AUTOCLOSE);
    SendMessageW(ac, BM_SETCHECK, BST_CHECKED, 0);

    Add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 16, 276, 430, 130, ID_LOG,
        WS_EX_CLIENTEDGE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = h;
        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW big = ncm.lfMessageFont;
        big.lfHeight = big.lfHeight * 3 / 2;
        big.lfWeight = FW_BOLD;
        g_fontBig = CreateFontIndirectW(&big);
        CreateControls();
        LoadSettings();
        UpdateHint();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_HOST: case ID_CLIENT: UpdateHint(); break;
        case ID_PORT: if (HIWORD(wp) == EN_CHANGE && IsHost()) UpdateHint(); break;
        case ID_PLAY: case IDOK: OnPlay(); break;
        }
        return 0;
    case WM_APP_LOG: {
        auto* s = (std::wstring*)lp;
        HWND log = GetDlgItem(h, ID_LOG);
        int len = GetWindowTextLengthW(log);
        SendMessageW(log, EM_SETSEL, len, len);
        std::wstring line = (len ? L"\r\n" : L"") + *s;
        SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
        delete s;
        return 0;
    }
    case WM_APP_DONE:
        g_busy = false;
        EnableWindow(GetDlgItem(h, ID_PLAY), TRUE);
        if (wp == 1 && SendDlgItemMessageW(h, ID_AUTOCLOSE, BM_GETCHECK, 0, 0) == BST_CHECKED)
            SetTimer(h, 1, 3000, nullptr);   // дать прочитать «Готово!»
        return 0;
    case WM_TIMER:
        KillTimer(h, 1);
        DestroyWindow(h);
        return 0;
    case WM_CTLCOLORSTATIC: {
        // фон надписей как у окна (окно журнала — как обычное поле только для чтения)
        if (GetDlgCtrlID((HWND)lp) == ID_LOG) break;
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_CLOSE:
        if (g_busy && MessageBoxW(h, L"Лаунчер ещё ждёт игру. Закрыть?", L"PvZ Co-op",
                                  MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = exe;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    g_ini = g_dir + L"coop.ini";
    g_dll = g_dir + kDllName;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"PvZCoopLauncher";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT r{ 0, 0, 462, 422 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"PvZ Co-op", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    WSACleanup();
    return 0;
}
