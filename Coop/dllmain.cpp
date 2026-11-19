// PvZ GOTY (Steam 1.2.0.1095, popcapgame1.exe) — Co-op mod
// Этап 3: сетевой слой (lockstep) + одинаковый генератор случайных чисел у обоих игроков
// Оформление: правая половина панели (слоты 4–6, растения игрока 2) — сиреневая
// Этап 4: игра не зависает при ожидании напарника; уход напарника с уровня и обрыв связи
// Сложность: бюджет волн зомби (wave_budget), задаёт хост
//
// Как играть по сети: оба подключаются (в главном меню), затем оба запускают ОДИН И ТОТ ЖЕ уровень
// и выбирают одинаковые растения. Синхронная игра стартует сама на первом игровом тике.
// Сборка: Visual Studio, Release | x86 (Win32), DLL, /MT.
//
// Клавиши:
//   F1  — +100 солнца (отладка; ВНИМАНИЕ: в сетевой игре вызовет рассинхрон)
//   Стрелки — выбрать клетку своего игрока
//   F3 / F4 — посадить растение из слота 1 / 2 в выбранную клетку
//   F5  — что в руке (курсор)          F6 — таблица пакетов семян
//   F7 / F8 — экран выбора растений: найти и показать / что изменилось
//   F2  — поиск счётчиков тиков        F9 — поиск генератора случайных чисел
//   F10 — сохранить образ игры в game_dump.bin
//   F11 — статус сети
//   F12 — выгрузить мод (НЕ закрывай консоль крестиком — закроется игра)
//
// Настройки сети — в coop.ini рядом с Coop.dll (создаётся автоматически при первом запуске).

#define NOMINMAX        // чтобы windows.h не ломал std::min/std::max
#include <winsock2.h>   // ДО windows.h
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <intrin.h>
#include <vector>
#include <map>
#include <deque>
#include <string>
#include <mutex>
#include <atomic>
#include <algorithm>
#include "MinHook.h"

#pragma comment(lib, "version.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ---------------------------------------------------------------------------
// Весь вывод мода идёт и в консоль, и в файл coop_log.txt рядом с Coop.dll
// (файл перезаписывается при каждом запуске мода)
// ---------------------------------------------------------------------------
#include <cstdarg>
#include <share.h>
static FILE* g_logFile = nullptr;

static int CoopLog(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = vprintf(fmt, a);
    va_end(a);
    if (g_logFile) {
        va_start(a, fmt);
        vfprintf(g_logFile, fmt, a);
        va_end(a);
        fflush(g_logFile);
    }
    return n;
}
#define printf CoopLog

namespace addr {
    // Найдено через Cheat Engine для этой сборки игры
    constexpr uintptr_t LawnAppPtr = 0x731C50;  // "popcapgame1.exe"+331C50
    constexpr uintptr_t App_Board = 0x868;     // LawnApp -> Board*
    constexpr uintptr_t Board_Sun = 0x5578;    // Board -> int sun
    constexpr uintptr_t Board_SeedBank = 0x15C; // Board -> SeedBank*

    // SeedBank (панель семян)
    constexpr uintptr_t SeedBank_NumPackets = 0x24;  // int: сколько пакетов на этом уровне
    constexpr uintptr_t SeedBank_Packets = 0x28;  // массив SeedPacket
    constexpr uintptr_t SeedPacket_Size = 0x50;

    // SeedPacket (смещения от начала пакета)
    constexpr uintptr_t Packet_RefreshCounter = 0x24; // int: прогресс перезарядки (тики)
    constexpr uintptr_t Packet_RefreshTime = 0x28; // int: полное время перезарядки (тики)
    constexpr uintptr_t Packet_Index = 0x2C; // int: номер слота
    constexpr uintptr_t Packet_Type = 0x34; // int: SeedType
    constexpr uintptr_t Packet_Imitater = 0x38; // int: тип для имитатора (-1 = нет)
    constexpr uintptr_t Packet_Active = 0x48; // byte: 1 = готов к использованию
    constexpr uintptr_t Packet_Refreshing = 0x49; // byte: 1 = идёт перезарядка
    constexpr uintptr_t Packet_TimesUsed = 0x4C; // int

    // Функции игры
    // Board::TakeSunMoney: EDI = Board*, EBX = стоимость, возврат AL (1 = списано)
    constexpr uintptr_t Fn_TakeSun = 0x41F620;

    // Board::MouseDownWithPlant: стек [Board*, x, y], ECX = clickCount (<0 = правый клик)
    constexpr uintptr_t Fn_MouseDownWithPlant = 0x413250;

    // Тик Board (Update/UpdateGame): ECX = Board*, внутри mMainCounter += 1
    constexpr uintptr_t Fn_BoardTick = 0x418D70;
    constexpr uintptr_t Board_MainCounter = 0x5580;

    // Курсор (что игрок держит в руке): Board+0x150 -> CursorObject*
    constexpr uintptr_t Board_Cursor = 0x150;
    constexpr uintptr_t Cursor_SeedBankIndex = 0x24; // номер слота в панели
    constexpr uintptr_t Cursor_SeedType = 0x28; // тип растения (0 = горохострел, 1 = подсолнух...)
    constexpr uintptr_t Cursor_Imitater = 0x2C; // тип для имитатора (-1 = нет)
    constexpr uintptr_t Cursor_Type = 0x30; // 0 = пустая рука, 1 = растение из панели

    // Найдено, пока не используется:
    // 0x411520 — Board::CanPlantAt? (вызывается с push edi/ebx/ebp, 0 в eax = можно сажать)
    // 0x4216D0 — стоимость растения: EAX = SeedType, EDX = Imitater, EDI = Board
    // 0x410200 — Board::ClearCursor, EAX = Board
}

// Геометрия лужайки (дневной уровень): клетка 80x100 пикселей, поле начинается с (40, 80)
namespace lawn {
    constexpr int Cols = 9, Rows = 6;
    inline int CellCenterX(int col) { return 40 + col * 80 + 40; }
    inline int CellCenterY(int row) { return 80 + row * 100 + 50; }
}

static HMODULE g_self = nullptr;
static volatile bool g_running = true;

template <typename T>
static bool SafeRead(uintptr_t address, T& out) {
    __try { out = *reinterpret_cast<T*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template <typename T>
static bool SafeWrite(uintptr_t address, const T& value) {
    __try { *reinterpret_cast<T*>(address) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static uintptr_t GetApp() {
    uintptr_t app = 0;
    SafeRead(addr::LawnAppPtr, app);
    return app;
}

// Адрес Board или 0, если сейчас не в бою
static uintptr_t GetBoard() {
    uintptr_t app = GetApp(), board = 0;
    if (!app) return 0;
    if (!SafeRead(app + addr::App_Board, board)) return 0;
    return board;
}

static void OpenLogFile() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    if (char* sl = strrchr(path, '\\')) strcpy_s(sl + 1, MAX_PATH - (sl + 1 - path), "coop_log.txt");
    g_logFile = _fsopen(path, "w", _SH_DENYNO); // другие программы могут читать лог, пока мод работает
}

static void SetupConsole(FILE*& f) {
    OpenLogFile();
    AllocConsole();
    freopen_s(&f, "CONOUT$", "w", stdout);
    SetConsoleTitleA("PvZ Co-op mod");

    // Отключаем QuickEdit: клик мышью в консоли больше не замораживает мод
    HANDLE hIn = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD mode = 0;
    if (GetConsoleMode(hIn, &mode)) {
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        mode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, mode);
    }
}

static void PrintGameInfo() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    printf("[coop] exe:  %s\n", path);
    printf("[coop] base: %08X\n", (unsigned)(uintptr_t)GetModuleHandleA(nullptr));

    DWORD dummy = 0, size = GetFileVersionInfoSizeA(path, &dummy);
    if (size) {
        std::vector<char> buf(size);
        VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
        if (GetFileVersionInfoA(path, 0, size, buf.data()) &&
            VerQueryValueA(buf.data(), "\\", (void**)&fi, &len) && fi) {
            printf("[coop] version: %u.%u.%u.%u\n",
                HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        }
    }
}

// ===========================================================================
// Настройки (coop.ini)
// ===========================================================================

enum class NetMode { Off, Host, Client };

struct Config {
    NetMode mode = NetMode::Off;
    char    host[128] = "127.0.0.1";
    int     port = 34567;
    int     delay = 10;   // задержка ввода в тиках (10 тиков = 100 мс)
    int     waveBudget = 100; // [game] wave_budget: «бюджет» волн зомби в процентах (у хоста — для обоих)

    // Проверка детерминизма на одном компьютере (только при mode=off)
    enum class Test { Off, Record, Replay } test = Test::Off;
    uint32_t testSeed = 12345;
    char     dir[MAX_PATH] = {}; // папка, где лежит Coop.dll
};
static Config g_cfg;

static void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "coop.ini");

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        // Первый запуск — создаём файл с настройками по умолчанию
        WritePrivateProfileStringA("net", "; mode", "off | host | client", path);
        WritePrivateProfileStringA("net", "mode", "off", path);
        WritePrivateProfileStringA("net", "host", "127.0.0.1", path);
        WritePrivateProfileStringA("net", "port", "34567", path);
        WritePrivateProfileStringA("net", "delay", "10", path);
        printf("[cfg] created %s\n", path);
    }
    if (GetPrivateProfileIntA("game", "wave_budget", -1, path) == -1) {
        WritePrivateProfileStringA("game", "; wave_budget", "100 = normal, 150 = 1.5x more zombie points per wave, 200 = 2x...", path);
        WritePrivateProfileStringA("game", "wave_budget", "100", path);
    }
    if (GetPrivateProfileIntA("test", "seed", -1, path) == -1) {
        WritePrivateProfileStringA("test", "; mode", "off | record | replay  (works only with [net] mode=off)", path);
        WritePrivateProfileStringA("test", "mode", "off", path);
        WritePrivateProfileStringA("test", "seed", "12345", path);
    }
    strcpy_s(g_cfg.dir, path);
    if (char* sl = strrchr(g_cfg.dir, '\\')) sl[1] = 0;

    char mode[32] = {};
    GetPrivateProfileStringA("net", "mode", "off", mode, sizeof(mode), path);
    GetPrivateProfileStringA("net", "host", "127.0.0.1", g_cfg.host, sizeof(g_cfg.host), path);
    g_cfg.port = GetPrivateProfileIntA("net", "port", 34567, path);
    g_cfg.delay = GetPrivateProfileIntA("net", "delay", 10, path);
    if (g_cfg.delay < 2) g_cfg.delay = 2;
    if (g_cfg.delay > 60) g_cfg.delay = 60;
    g_cfg.waveBudget = GetPrivateProfileIntA("game", "wave_budget", 100, path);
    if (g_cfg.waveBudget < 50) g_cfg.waveBudget = 50;
    if (g_cfg.waveBudget > 500) g_cfg.waveBudget = 500;
    printf("[cfg] wave budget %d%%\n", g_cfg.waveBudget);

    if (_stricmp(mode, "host") == 0)        g_cfg.mode = NetMode::Host;
    else if (_stricmp(mode, "client") == 0) g_cfg.mode = NetMode::Client;
    else                                    g_cfg.mode = NetMode::Off;

    printf("[cfg] %s  mode=%s host=%s port=%d delay=%d\n", path,
        g_cfg.mode == NetMode::Host ? "host" : g_cfg.mode == NetMode::Client ? "client" : "off",
        g_cfg.host, g_cfg.port, g_cfg.delay);

    char test[32] = {};
    GetPrivateProfileStringA("test", "mode", "off", test, sizeof(test), path);
    g_cfg.testSeed = (uint32_t)GetPrivateProfileIntA("test", "seed", 12345, path);
    if (g_cfg.mode == NetMode::Off) {
        if (_stricmp(test, "record") == 0)      g_cfg.test = Config::Test::Record;
        else if (_stricmp(test, "replay") == 0) g_cfg.test = Config::Test::Replay;
    }
    if (g_cfg.test != Config::Test::Off)
        printf("[cfg] TEST MODE: %s, seed=%u, file=%scoop_replay.txt\n",
            g_cfg.test == Config::Test::Record ? "RECORD" : "REPLAY", g_cfg.testSeed, g_cfg.dir);
}

// Номер своего игрока: хост (и одиночная игра) = 1, клиент = 2
static int LocalPlayer() { return g_cfg.mode == NetMode::Client ? 2 : 1; }

// ===========================================================================
// Хуки: TakeSunMoney, MouseDownWithPlant
// ===========================================================================

// --- Board::TakeSunMoney (usercall: EDI = board, EBX = amount, ret AL) ---

static void* g_origTakeSun = nullptr;

static void __stdcall OnTakeSun(uintptr_t board, int amount, int result) {
    printf("[hook] TakeSunMoney board=%08X amount=%d -> %s\n",
        (unsigned)board, amount, result ? "OK" : "not enough sun");
}

__declspec(naked) static void Hook_TakeSun() {
    __asm {
        call[g_origTakeSun]   // оригинал с теми же EDI/EBX -> результат в AL
        pushad
        movzx eax, al
        push eax               // result
        push ebx               // amount
        push edi               // board
        call OnTakeSun
        popad
        ret
    }
}

// --- Board::MouseDownWithPlant (стек: board, x, y; ECX = clickCount) ---

static void* g_origMouseDownWithPlant = nullptr;
static bool  g_inDoPlant = false;        // true, пока посадку делает сам мод
static std::atomic<bool> g_lockstepActive{ false };

// Сообщение на экране игры (под панелью семян). Пишет поток игры, читает отрисовка (тот же поток).
namespace status {
    static char  g_text[96] = "";
    static DWORD g_until = 0;        // 0 = пока не снимут вручную
    static bool  g_on = false;
    static void Show(const char* text, DWORD ms = 0) {
        strncpy_s(g_text, text, _TRUNCATE);
        g_until = ms ? GetTickCount() + ms : 0;
        g_on = true;
    }
    static void Hide() { g_on = false; }
    static const char* Current() {
        if (!g_on) return nullptr;
        if (g_until && (int)(GetTickCount() - g_until) > 0) { g_on = false; return nullptr; }
        return g_text;
    }
}

static void __stdcall OnMouseDownWithPlant(uintptr_t board, int x, int y, int clicks) {
    if (g_inDoPlant) return; // наша же посадка — не шумим
    printf("[hook] MouseDownWithPlant board=%08X x=%d y=%d clicks=%d\n",
        (unsigned)board, x, y, clicks);
}

__declspec(naked) static void Hook_MouseDownWithPlant() {
    __asm {
        pushad
        mov  eax, [esp + 36]    // board
        mov  edx, [esp + 40]    // x
        mov  esi, [esp + 44]    // y
        push ecx                // clicks
        push esi
        push edx
        push eax
        call OnMouseDownWithPlant
        popad
        jmp[g_origMouseDownWithPlant]
    }
}

// ===========================================================================
// Панель семян и посадка из кода
// ===========================================================================

struct PacketInfo {
    int  type = -1, imitater = -1;
    int  refreshCounter = 0, refreshTime = 0, timesUsed = 0;
    bool active = false, refreshing = false;
};

static uintptr_t GetSeedBankOf(uintptr_t board) {
    uintptr_t bank = 0;
    if (board) SafeRead(board + addr::Board_SeedBank, bank);
    return bank;
}

static int GetNumPackets(uintptr_t bank) {
    int n = 0;
    if (bank) SafeRead(bank + addr::SeedBank_NumPackets, n);
    return (n >= 0 && n <= 10) ? n : 0;
}

static bool ReadPacket(uintptr_t bank, int slot, PacketInfo& p) {
    if (!bank || slot < 0 || slot >= GetNumPackets(bank)) return false;
    uintptr_t pk = bank + addr::SeedBank_Packets + slot * addr::SeedPacket_Size;
    unsigned char active = 0, refreshing = 0;
    bool ok = SafeRead(pk + addr::Packet_Type, p.type)
        && SafeRead(pk + addr::Packet_Imitater, p.imitater)
        && SafeRead(pk + addr::Packet_RefreshCounter, p.refreshCounter)
        && SafeRead(pk + addr::Packet_RefreshTime, p.refreshTime)
        && SafeRead(pk + addr::Packet_TimesUsed, p.timesUsed)
        && SafeRead(pk + addr::Packet_Active, active)
        && SafeRead(pk + addr::Packet_Refreshing, refreshing);
    p.active = active != 0;
    p.refreshing = refreshing != 0;
    return ok;
}

struct PlantRequest {
    int player;     // 1 или 2 (для лога)
    int slot;       // из какого слота панели
    int col, row;   // клетка
};

// Вызов Board::MouseDownWithPlant: стек [board, x, y], ECX = clicks.
// Кто чистит стек, мы не знаем, поэтому сохраняем ESP в ESI и восстанавливаем после вызова.
static void CallMouseDownWithPlant(uintptr_t board, int x, int y, int clicks) {
    uintptr_t fn = addr::Fn_MouseDownWithPlant;
    __asm {
        mov  esi, esp
        push y
        push x
        push board
        mov  ecx, clicks
        mov  eax, fn
        call eax
        mov  esp, esi
    }
}

// Выполняется ТОЛЬКО В ПОТОКЕ ИГРЫ (из OnBoardTick)
static void DoPlant(uintptr_t board, const PlantRequest& r) {
    PacketInfo pk;
    if (!ReadPacket(GetSeedBankOf(board), r.slot, pk)) {
        printf("[p%d] slot %d does not exist on this level\n", r.player, r.slot + 1);
        return;
    }
    if (!pk.active) {
        printf("[p%d] slot %d not ready (recharge %d/%d)\n",
            r.player, r.slot + 1, pk.refreshCounter, pk.refreshTime);
        return;
    }

    uintptr_t cursor = 0;
    if (!SafeRead(board + addr::Board_Cursor, cursor) || !cursor) {
        printf("[p%d] no cursor object\n", r.player);
        return;
    }

    // Запоминаем, что было в руке
    int savedIndex = 0, savedSeed = 0, savedImit = 0, savedType = 0;
    SafeRead(cursor + addr::Cursor_SeedBankIndex, savedIndex);
    SafeRead(cursor + addr::Cursor_SeedType, savedSeed);
    SafeRead(cursor + addr::Cursor_Imitater, savedImit);
    SafeRead(cursor + addr::Cursor_Type, savedType);

    // «Берём» пакет, как при клике по панели (mActive = 0)
    uintptr_t packetAddr = GetSeedBankOf(board) + addr::SeedBank_Packets + r.slot * addr::SeedPacket_Size;
    SafeWrite(packetAddr + addr::Packet_Active, (unsigned char)0);

    SafeWrite(cursor + addr::Cursor_SeedBankIndex, r.slot);
    SafeWrite(cursor + addr::Cursor_SeedType, pk.type);
    SafeWrite(cursor + addr::Cursor_Imitater, pk.imitater);
    SafeWrite(cursor + addr::Cursor_Type, 1);

    int sunBefore = 0, sunAfter = 0;
    SafeRead(board + addr::Board_Sun, sunBefore);

    g_inDoPlant = true;
    CallMouseDownWithPlant(board, lawn::CellCenterX(r.col), lawn::CellCenterY(r.row), 1);
    g_inDoPlant = false;

    SafeRead(board + addr::Board_Sun, sunAfter);

    int timesUsedAfter = pk.timesUsed;
    SafeRead(packetAddr + addr::Packet_TimesUsed, timesUsedAfter);
    bool planted = timesUsedAfter > pk.timesUsed;
    if (!planted)
        SafeWrite(packetAddr + addr::Packet_Active, (unsigned char)1); // как ClearCursor

    printf("[p%d] slot %d (seed %d) at col=%d row=%d: sun %d -> %d %s\n",
        r.player, r.slot + 1, pk.type, r.col, r.row, sunBefore, sunAfter,
        planted ? "PLANTED" : "(not planted)");

    // Возвращаем руку как было
    SafeWrite(cursor + addr::Cursor_SeedBankIndex, savedIndex);
    SafeWrite(cursor + addr::Cursor_SeedType, savedSeed);
    SafeWrite(cursor + addr::Cursor_Imitater, savedImit);
    SafeWrite(cursor + addr::Cursor_Type, savedType);
}

// F6: таблица пакетов
static void PrintPackets() {
    uintptr_t board = GetBoard(), bank = GetSeedBankOf(board);
    int n = GetNumPackets(bank);
    printf("[bank] seedbank=%08X packets=%d\n", (unsigned)bank, n);
    for (int i = 0; i < n; ++i) {
        PacketInfo p;
        if (!ReadPacket(bank, i, p)) continue;
        printf("[bank]   slot %d: seed=%2d imit=%2d  %s  recharge %4d/%4d  used=%d\n",
            i + 1, p.type, p.imitater,
            p.active ? "READY   " : (p.refreshing ? "RECHARGE" : "INACTIVE"),
            p.refreshCounter, p.refreshTime, p.timesUsed);
    }
}

// F5: что сейчас в руке
static void DumpCursor() {
    uintptr_t board = GetBoard(), cursor = 0;
    if (!board || !SafeRead(board + addr::Board_Cursor, cursor) || !cursor) {
        printf("[cur] no board/cursor\n");
        return;
    }
    int idx = 0, seed = 0, imit = 0, type = 0;
    SafeRead(cursor + addr::Cursor_SeedBankIndex, idx);
    SafeRead(cursor + addr::Cursor_SeedType, seed);
    SafeRead(cursor + addr::Cursor_Imitater, imit);
    SafeRead(cursor + addr::Cursor_Type, type);
    printf("[cur] cursor=%08X  slot=%d seed=%d imit=%d type=%d\n",
        (unsigned)cursor, idx, seed, imit, type);
}

// ===========================================================================
// Клики по полю (Board::MouseDown, 0x415430, thiscall: ECX = Board, стек x, y, clickCount)
//
// В синхронной игре клик не выполняется сразу, а становится командой. На нужном тике оба мода
// вызывают настоящий Board::MouseDown. Для клика напарника в «руку» (курсор) временно
// подставляется ЕГО состояние, а после клика запоминается обратно — каждый мод хранит руку
// напарника, поэтому на обоих компьютерах игра видит одно и то же.
// ===========================================================================

constexpr uintptr_t Fn_BoardMouseDown = 0x415430;

// Экран выбора растений (SeedChooserScreen)
namespace sc {
    constexpr uintptr_t App_Chooser = 0x874;    // LawnApp -> SeedChooserScreen*
    constexpr uintptr_t Fn_MouseDown = 0x494DC0; // thiscall: ECX = chooser, стек x, y, clickCount
    constexpr uintptr_t Seeds = 0xBC;     // ChosenSeed[49]
    constexpr uintptr_t SeedSize = 0x3C;
    constexpr int       NumSeeds = 49;
    // поля ChosenSeed
    constexpr uintptr_t S_X = 0x00, S_Y = 0x04, S_Type = 0x20, S_State = 0x24, S_BankIndex = 0x28, S_Imitater = 0x34;
    constexpr int StateInBank = 1, StateInChooser = 3;
    constexpr uintptr_t SeedsInBank = 0xD3C;    // int: сколько растений уже в панели
}
using BoardMouseDown_t = void(__thiscall*)(void* board, int x, int y, int clicks);
static BoardMouseDown_t g_origBoardMouseDown = nullptr;
static bool g_execClick = false; // true, пока клик выполняет сам мод

struct CursorState { int slot = 0, seed = -1, imit = -1, type = 0; };
static CursorState g_remoteCursor;   // рука напарника (только поток игры)

static bool ReadCursor(uintptr_t board, CursorState& c) {
    uintptr_t cur = 0;
    if (!SafeRead(board + addr::Board_Cursor, cur) || !cur) return false;
    SafeRead(cur + addr::Cursor_SeedBankIndex, c.slot);
    SafeRead(cur + addr::Cursor_SeedType, c.seed);
    SafeRead(cur + addr::Cursor_Imitater, c.imit);
    SafeRead(cur + addr::Cursor_Type, c.type);
    return true;
}

static void WriteCursor(uintptr_t board, const CursorState& c) {
    uintptr_t cur = 0;
    if (!SafeRead(board + addr::Board_Cursor, cur) || !cur) return;
    SafeWrite(cur + addr::Cursor_SeedBankIndex, c.slot);
    SafeWrite(cur + addr::Cursor_SeedType, c.seed);
    SafeWrite(cur + addr::Cursor_Imitater, c.imit);
    SafeWrite(cur + addr::Cursor_Type, c.type);
}

// В начале синхронной игры у обоих игроков пустые руки — одинаково на обоих компьютерах
static void ResetCursorsForSync(uintptr_t board) {
    CursorState empty;
    CursorState local;
    if (ReadCursor(board, local) && local.type != 0)
        printf("[click] note: your hand was not empty at sync start, cleared\n");
    WriteCursor(board, empty);
    g_remoteCursor = empty;
}

// Уровень, где панель поделена 3 + 3 (уровень начат через синхронный выбор растений).
// Слоты 0..2 — игрок 1, слоты 3..5 — игрок 2.
static uintptr_t g_splitBoard = 0;
static int SlotOwner(int slot) { return slot < 3 ? 1 : 2; }

// Если клик игрока взял в руку пакет из чужого слота — отменяем это (одинаково у обоих игроков,
// потому что проверка идёт внутри синхронного клика)
static void UndoForeignPick(uintptr_t board, int player, const CursorState& before, CursorState& after) {
    if (board != g_splitBoard) return;
    if (after.type != 1) return;                                     // в руке не пакет из панели
    if (before.type == 1 && before.slot == after.slot) return;       // ничего нового не взял
    if (SlotOwner(after.slot) == player) return;                     // свой слот
    // Возвращаем пакет (при взятии игра делает его неактивным) и прежнюю руку
    uintptr_t bank = 0;
    SafeRead(board + addr::Board_SeedBank, bank);
    if (bank) SafeWrite(bank + addr::SeedBank_Packets + after.slot * addr::SeedPacket_Size + addr::Packet_Active,
        (unsigned char)1);
    printf("[p%d] slot %d belongs to player %d - not allowed\n", player, after.slot + 1, SlotOwner(after.slot));
    after = before;
}

// Выполнить клик игрока player (только поток игры)
static void ExecClick(uintptr_t board, int player, int x, int y, int clicks) {
    if (!g_origBoardMouseDown) return;
    printf("[p%d] click x=%d y=%d %s\n", player, x, y, clicks < 0 ? "(right)" : "");
    g_execClick = true;
    if (player == LocalPlayer()) {
        CursorState before, after;
        ReadCursor(board, before);
        g_origBoardMouseDown((void*)board, x, y, clicks);
        ReadCursor(board, after);
        CursorState fixed = after;
        UndoForeignPick(board, player, before, fixed);
        if (fixed.type != after.type || fixed.slot != after.slot) WriteCursor(board, fixed);
    }
    else {
        CursorState mine;
        ReadCursor(board, mine);
        WriteCursor(board, g_remoteCursor);
        CursorState before = g_remoteCursor;
        g_origBoardMouseDown((void*)board, x, y, clicks);
        ReadCursor(board, g_remoteCursor);
        UndoForeignPick(board, player, before, g_remoteCursor);
        WriteCursor(board, mine);
    }
    g_execClick = false;
}

// ===========================================================================
// Модель выбора растений по сети (3 + 3). Хост — «судья».
//   Действия: PICK (взять растение), REMOVE (вернуть своё), START (Let's Rock).
//   Хост проверяет и применяет действие к модели, затем рассылает; оба мода ставят его в очередь
//   и выполняют настоящим кликом по экрану выбора в потоке игры.
// ===========================================================================

namespace scm {
    enum : uint8_t { A_PICK = 1, A_REMOVE = 2, A_START = 3 };
    constexpr int kPerPlayer = 3;
    constexpr int kMaxSeeds = 49;

    struct Ev { uint8_t player, action, seed; };

    static std::mutex g_mx;
    static std::deque<Ev> g_queue;           // к исполнению в потоке игры
    static int  g_owner[kMaxSeeds] = {};     // 0 — свободно, 1/2 — чьё
    static std::vector<int> g_order[3];      // порядок выбора у каждого игрока
    static bool g_started = false;

    static const char* ActionName(uint8_t a) {
        return a == A_PICK ? "pick" : a == A_REMOVE ? "remove" : a == A_START ? "start" : "?";
    }

    // Вызывать под g_mx
    static void ResetLocked() {
        g_queue.clear();
        for (int& o : g_owner) o = 0;
        for (auto& v : g_order) v.clear();
        g_started = false;
    }

    static void Reset() {
        std::lock_guard<std::mutex> lk(g_mx);
        ResetLocked();
    }

    // Проверка (только хост). Возвращает nullptr, если можно, иначе причину. Под g_mx.
    static const char* ValidateLocked(int p, uint8_t a, int s) {
        if (g_started) return "level already started";
        if (a == A_START) {
            if ((int)g_order[1].size() != kPerPlayer || (int)g_order[2].size() != kPerPlayer)
                return "both players need 3 plants";
            return nullptr;
        }
        if (s < 0 || s >= kMaxSeeds) return "bad plant";
        if (a == A_PICK) {
            if (g_owner[s] != 0) return "plant already taken";
            if ((int)g_order[p].size() >= kPerPlayer) return "you already have 3 plants";
            return nullptr;
        }
        if (a == A_REMOVE) {
            if (g_owner[s] != p) return "not your plant";
            return nullptr;
        }
        return "unknown action";
    }

    // Применить к модели и поставить в очередь исполнения. Под g_mx.
    static void ApplyLocked(int p, uint8_t a, int s) {
        if (a == A_PICK) { g_owner[s] = p; g_order[p].push_back(s); }
        else if (a == A_REMOVE) {
            g_owner[s] = 0;
            auto& v = g_order[p];
            v.erase(std::remove(v.begin(), v.end(), s), v.end());
        }
        else if (a == A_START) g_started = true;
        g_queue.push_back({ (uint8_t)p, a, (uint8_t)s });
        printf("[sc] player %d %s %d  (P1: %u plants, P2: %u plants)\n", p, ActionName(a), s,
            (unsigned)g_order[1].size(), (unsigned)g_order[2].size());
    }
}

// ===========================================================================
// Генератор случайных чисел игры (Sexy::MTRand, Mersenne Twister)
//
//   gMTRand = 0x7E2E00: unsigned long mt[624]; int mti (смещение 0x9C0)
//   MTRand::NextNoAssert = 0x5FD690 (thiscall, ECX = MTRand*) — через неё идут все случайные числа
//
// Чтобы обе копии игры вели себя одинаково, мод в ключевые моменты заполняет состояние
// генератора у обоих игроков из общего seed (свой init_genrand, без вызова функций игры).
// ===========================================================================

namespace rng {
    constexpr uintptr_t GlobalMTRand = 0x7E2E00;
    constexpr uintptr_t Fn_NextNoAssert = 0x5FD690;
    constexpr int       N = 624;
    constexpr uintptr_t Mti = 0x9C0;

    static void SeedGlobal(uint32_t seed) {
        uint32_t mt[N];
        mt[0] = seed;
        for (int i = 1; i < N; ++i)
            mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + (uint32_t)i;
        for (int i = 0; i < N; ++i) SafeWrite(GlobalMTRand + i * 4, mt[i]);
        SafeWrite(GlobalMTRand + Mti, N); // следующий вызов пересчитает массив
    }

    // Короткий «отпечаток» состояния генератора
    static uint32_t StateHash() {
        int mti = 0;
        SafeRead(GlobalMTRand + Mti, mti);
        uint32_t h = 2166136261u ^ (uint32_t)mti;
        for (int i = 0; i < N; i += 13) {
            uint32_t v = 0;
            SafeRead(GlobalMTRand + i * 4, v);
            h = (h ^ v) * 16777619u;
        }
        return h;
    }
}

// Общие для сети и генератора данные сессии
namespace session {
    static std::atomic<bool>     g_haveSeed{ false };  // получен ли seed сессии
    static std::atomic<uint32_t> g_seed{ 0 };
    static std::atomic<int>      g_delay{ 10 };
    static std::atomic<int>      g_wavePct{ 100 };  // бюджет волн, заданный хостом для этой сессии
    static std::atomic<uint32_t> g_nextLevel{ 0 };    // сколько уровней создано с начала сессии

    // Только поток игры:
    static uintptr_t g_seededBoard = 0;   // уровень, генератор которого мы заполнили
    static uint32_t  g_seededEpoch = 0;   // его номер в сессии
    static uintptr_t g_warnedBoard = 0;
    static uintptr_t g_goodBoard = 0;     // уровень, созданный ПОСЛЕ подключения (с seed сессии)
    static std::atomic<bool> g_noBoardSeen{ false }; // был момент без уровня (меню и т.п.) — следующий Board новый
    static uintptr_t g_badStartWarned = 0;
    static uint32_t  g_seedGen = 0;       // растёт при каждом заполнении генератора (новый уровень)
    static int       g_lastCounter = -1;  // mMainCounter при последнем вызове генератора
    static int       g_prevCounterForLog = -1;

    static uint32_t LevelSeed(uint32_t epoch) { return g_seed + 0x9E3779B9u * (epoch + 1); }
}

// --- Хук MTRand::NextNoAssert: первый вызов генератора после создания нового уровня ---

static void* g_origNextNoAssert = nullptr;
static std::atomic<bool> g_netConnected{ false }; // копия net::g_connected для хука (объявлен раньше net)

namespace net { static void SendScReset(); } // определена ниже, в сетевом коде

// ---------------------------------------------------------------------------
// Трассировка генератора для поиска рассинхрона.
// Для каждого тика синхронной игры запоминаем, КТО брал случайные числа (адрес в коде игры).
// При первом рассинхроне оба игрока сохраняют coop_rng_trace.txt — сравнив два файла,
// видно первый тик и функцию, где игры разошлись.
// Отдельно: случайные числа, взятые ВНЕ обновления поля (например, при отрисовке),
// зависят от частоты кадров и почти наверняка ломают синхронизацию — о них пишем сразу.
// ---------------------------------------------------------------------------
static bool g_inBoardUpdate = false;   // поток игры: сейчас идёт Board::Update
static int  g_inFoley = 0;             // поток игры: сейчас внутри проигрывания звука (TodFoley)
static uint32_t g_foleyProtected = 0;  // сколько звуков за уровень «отвязано» от генератора

namespace rtrace {
    constexpr int kSlots = 512, kPer = 48;
    struct Slot { uint32_t epoch, tick; uint16_t n, lost; uint32_t c[kPer]; };
    static Slot     g_ring[kSlots];
    static bool     g_on = false;
    static uint32_t g_epoch = 0, g_tick = 0;
    static uint32_t g_outside[32]; static int g_outsideN = 0;
    static uint32_t g_outsideTotal = 0;
    static bool     g_dumped = false;

    static void Begin(uint32_t epoch) {
        memset(g_ring, 0xFF, sizeof(g_ring));
        g_epoch = epoch; g_tick = 0; g_on = true; g_dumped = false;
        g_outsideN = 0; g_outsideTotal = 0;
    }
    static void Stop() { g_on = false; }

    // Код генератора и обёрток Sexy::Rand — их пропускаем, ищем вызывающего в коде игры
    static bool IsRngCode(uint32_t a) {
        return (a >= 0x5FD000 && a < 0x5FE000) || (a >= 0x6052E0 && a < 0x605340);
    }
    // ebp на входе в NextNoAssert = кадр обёртки (все обёртки с push ebp / mov ebp, esp)
    static uint32_t FindCaller(uint32_t ebp) {
        for (int i = 0; i < 4 && ebp; ++i) {
            uint32_t ret = 0, next = 0;
            if (!SafeRead(ebp + 4, ret) || !SafeRead(ebp, next)) return 0;
            if (!IsRngCode(ret)) return ret;
            if (next <= ebp) return ret;
            ebp = next;
        }
        return 0;
    }

    static void Record(uint32_t caller, bool outside) {
        if (!g_on) return;
        if (outside) {
            ++g_outsideTotal;
            bool seen = false;
            for (int i = 0; i < g_outsideN; ++i) if (g_outside[i] == caller) { seen = true; break; }
            if (!seen && g_outsideN < 32) {
                g_outside[g_outsideN++] = caller;
                printf("[rng] WARNING: random number taken OUTSIDE the game update by %08X "
                    "(tick %u) - this can break sync\n", caller, g_tick);
            }
        }
        Slot& s = g_ring[g_tick % kSlots];
        if (s.tick != g_tick || s.epoch != g_epoch) { s.tick = g_tick; s.epoch = g_epoch; s.n = 0; s.lost = 0; }
        if (s.n < kPer) s.c[s.n++] = caller | (outside ? 0x80000000u : 0);
        else ++s.lost;
    }

    static void Dump(uint32_t from, uint32_t to, const char* why) {
        if (g_dumped) return;
        g_dumped = true;
        char path[MAX_PATH];
        strcpy_s(path, g_cfg.dir);
        strcat_s(path, "coop_rng_trace.txt");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "w") != 0 || !f) { printf("[rng] cannot write %s\n", path); return; }
        fprintf(f, "# PvZ coop RNG trace, player %d, level %u, %s\n", LocalPlayer(), g_epoch, why);
        fprintf(f, "# tick: callers (! = outside game update)\n");
        for (uint32_t t = from; t <= to; ++t) {
            const Slot& s = g_ring[t % kSlots];
            if (s.tick != t || s.epoch != g_epoch) { fprintf(f, "%u: (no data)\n", t); continue; }
            fprintf(f, "%u:", t);
            for (int i = 0; i < s.n; ++i)
                fprintf(f, " %s%06X", (s.c[i] & 0x80000000u) ? "!" : "", s.c[i] & 0x7FFFFFFFu);
            if (s.lost) fprintf(f, " (+%u more)", s.lost);
            fprintf(f, "\n");
        }
        fclose(f);
        printf("[rng] trace saved: %s (ticks %u..%u)\n", path, from, to);
    }
}

static void __stdcall OnRandCall(uintptr_t self, uint32_t ebp) {
    if (self != rng::GlobalMTRand) return;
    if (rtrace::g_on && !g_inFoley) rtrace::Record(rtrace::FindCaller(ebp), !g_inBoardUpdate);
    uintptr_t board = GetBoard();
    if (!board) { session::g_lastCounter = -1; return; }

    // Новый уровень = новый адрес Board ИЛИ счётчик тиков уровня сбросился назад
    // (при «перезапустить уровень» новый Board может оказаться по тому же адресу)
    int counter = 0;
    SafeRead(board + addr::Board_MainCounter, counter);
    // Новый Board может оказаться по тому же адресу — поэтому учитываем и то, что между уровнями
    // был момент без Board (главное меню, экран награды)
    const bool freshBoard = session::g_noBoardSeen.exchange(false);
    bool newLevel = board != session::g_seededBoard || freshBoard ||
        (session::g_lastCounter >= 0 && counter < session::g_lastCounter);
    session::g_prevCounterForLog = session::g_lastCounter;
    session::g_lastCounter = counter;
    if (!newLevel) return;

    // Новый объект Board (а не просто сброс счётчика на старте уровня после экрана выбора)
    const bool newBoardObject = board != session::g_seededBoard || freshBoard;

    // Новый уровень, и это первое случайное число для него
    if (g_cfg.test != Config::Test::Off && !g_netConnected) {
        session::g_seededBoard = board;
        session::g_seededEpoch = 0;
        ++session::g_seedGen;
        rng::SeedGlobal(g_cfg.testSeed);
        printf("[test] new level (#%u): board=%08X counter=%d (prev %d) -> generator seeded with %u\n",
            session::g_seedGen, (unsigned)board, counter, session::g_prevCounterForLog, g_cfg.testSeed);
    }
    else if (g_netConnected && session::g_haveSeed) {
        session::g_seededBoard = board;
        session::g_seededEpoch = session::g_nextLevel++;
        ++session::g_seedGen;
        // Счётчик уровня тикает и на экране выбора, а при «Let's Rock» сбрасывается на 0 —
        // это тоже попадает сюда. Выбор растений и разделение слотов сбрасываем только
        // для действительно нового уровня.
        if (newBoardObject) {
            session::g_goodBoard = board;
            g_splitBoard = 0;
            if (g_cfg.mode == NetMode::Host) {
                scm::Reset();          // новый выбор растений — у клиента сбросит сообщение
                net::SendScReset();
            }
        }
        rng::SeedGlobal(session::LevelSeed(session::g_seededEpoch));
        printf("[rng] new level %u: generator seeded (%08X)\n",
            session::g_seededEpoch, session::LevelSeed(session::g_seededEpoch));
    }
    else if (board != session::g_warnedBoard) {
        session::g_warnedBoard = board;
        session::g_seededBoard = board; // не заполнен, но отмечен
        if (g_cfg.mode != NetMode::Off)
            printf("[rng] level created WITHOUT session seed (not connected yet). "
                "Connect first, then start the level.\n");
    }
}

__declspec(naked) static void Hook_NextNoAssert() {
    __asm {
        pushad
        push ebp                // кадр обёртки Rand — по нему найдём вызывающего
        push ecx                // MTRand* this
        call OnRandCall
        popad
        jmp[g_origNextNoAssert]
    }
}

// ===========================================================================
// Сеть
//
// Протокол (TCP, little-endian). Каждое сообщение:
//   uint16 len   — длина (тип + данные)
//   uint8  type
//   ...данные
//
//   HELLO (1): uint32 magic 'PVZC', uint16 protoVersion, uint8 player
//   TICK  (3): uint32 epoch, uint32 tick, uint8 count, count * Cmd — команды игрока на тик уровня epoch
//   HASH  (4): uint32 epoch, uint32 tick, uint32 hash              — отпечаток состояния
//   SEED  (5): uint32 seed, uint32 delay, uint16 wavePct           — хост -> клиент сразу после HELLO
//   LEVEL (10): uint32 epoch, uint8 event (1 = начал синхронную игру, 2 = ушёл с уровня), uint32 tick
//
//   epoch = номер уровня с начала сессии (0, 1, 2...), одинаковый у обоих.
//   Cmd (10 байт): uint8 kind (1 = посадка F3/F4, 2 = клик), uint8 player, uint8 slot, uint8 col,
//                  uint8 row, int16 x, int16 y, int8 clicks
// ===========================================================================

namespace net {
    enum : uint8_t {
        MSG_HELLO = 1, MSG_TICK = 3, MSG_HASH = 4, MSG_SEED = 5,
        MSG_SC_RESET = 6, MSG_SC_REQ = 7, MSG_SC_EXEC = 8, MSG_SC_REJECT = 9,
        MSG_LEVEL = 10
    };
    enum : uint8_t { LV_START = 1, LV_LEFT = 2 };
    constexpr uint32_t kMagic = 0x435A5650; // 'PVZC'
    constexpr uint16_t kProto = 7;

    enum : uint8_t { CMD_PLANT = 1, CMD_CLICK = 2 };
    // 10 байт: kind, player, slot, col, row, int16 x, int16 y, int8 clicks
    struct Cmd {
        uint8_t kind = 0, player = 0, slot = 0, col = 0, row = 0;
        int16_t x = 0, y = 0;
        int8_t  clicks = 0;
    };

    static inline uint64_t Key(uint32_t epoch, uint32_t tick) { return ((uint64_t)epoch << 32) | tick; }

    static SOCKET g_sock = INVALID_SOCKET;
    static SOCKET g_listen = INVALID_SOCKET;
    static std::atomic<bool> g_connected{ false };
    static std::mutex g_sendMx;
    static HANDLE g_thread = nullptr;

    // Принятые данные (пишет сетевой поток, читает поток игры), ключ = Key(epoch, tick)
    static std::mutex g_rxMx;
    static std::map<uint64_t, std::vector<Cmd>> g_remoteTicks;
    // Отпечаток: [0] общий, [1] солнце, [2] счётчик тиков, [3] пакеты, [4] генератор
    struct HashRec { uint32_t v[5] = {}; };
    static std::map<uint64_t, HashRec> g_remoteHashes;

    // Что напарник сообщил о своём уровне (под g_rxMx)
    struct PeerLevel {
        bool     started = false; uint32_t startEpoch = 0;
        bool     left = false;    uint32_t leftEpoch = 0, leftTick = 0;
    };
    static PeerLevel g_peerLevel;

    static void SetConnected(bool v) { g_connected = v; g_netConnected = v; }

    // --- отправка ---

    static bool SendRaw(const void* data, int len) {
        const char* p = static_cast<const char*>(data);
        while (len > 0) {
            int n = send(g_sock, p, len, 0);
            if (n <= 0) return false;
            p += n; len -= n;
        }
        return true;
    }

    static bool SendMsg(uint8_t type, const std::vector<uint8_t>& payload) {
        if (!g_connected) return false;
        std::vector<uint8_t> buf(3 + payload.size());
        uint16_t len = (uint16_t)(1 + payload.size());
        memcpy(buf.data(), &len, 2);
        buf[2] = type;
        if (!payload.empty()) memcpy(buf.data() + 3, payload.data(), payload.size());
        std::lock_guard<std::mutex> lk(g_sendMx);
        if (!SendRaw(buf.data(), (int)buf.size())) {
            SetConnected(false);
            return false;
        }
        return true;
    }

    template <typename T> static void Put(std::vector<uint8_t>& v, T x) {
        size_t o = v.size(); v.resize(o + sizeof(T)); memcpy(v.data() + o, &x, sizeof(T));
    }
    template <typename T> static bool Get(const std::vector<uint8_t>& v, size_t& o, T& x) {
        if (o + sizeof(T) > v.size()) return false;
        memcpy(&x, v.data() + o, sizeof(T)); o += sizeof(T); return true;
    }

    static void SendHello() {
        std::vector<uint8_t> p;
        Put<uint32_t>(p, kMagic); Put<uint16_t>(p, kProto); Put<uint8_t>(p, (uint8_t)LocalPlayer());
        SendMsg(MSG_HELLO, p);
    }

    static void SendLevel(uint32_t epoch, uint8_t ev, uint32_t tick) {
        std::vector<uint8_t> p;
        Put<uint32_t>(p, epoch); Put<uint8_t>(p, ev); Put<uint32_t>(p, tick);
        SendMsg(MSG_LEVEL, p);
    }

    static void SendSeed(uint32_t seed, int delay, int wavePct) {
        std::vector<uint8_t> p;
        Put<uint32_t>(p, seed); Put<uint32_t>(p, (uint32_t)delay); Put<uint16_t>(p, (uint16_t)wavePct);
        SendMsg(MSG_SEED, p);
    }

    static void SendTick(uint32_t epoch, uint32_t tick, const std::vector<Cmd>& cmds) {
        std::vector<uint8_t> p;
        Put<uint32_t>(p, epoch);
        Put<uint32_t>(p, tick);
        Put<uint8_t>(p, (uint8_t)std::min<size_t>(cmds.size(), 255));
        for (size_t i = 0; i < cmds.size() && i < 255; ++i) {
            Put(p, cmds[i].kind); Put(p, cmds[i].player); Put(p, cmds[i].slot);
            Put(p, cmds[i].col);  Put(p, cmds[i].row);
            Put(p, cmds[i].x);    Put(p, cmds[i].y);   Put(p, cmds[i].clicks);
        }
        SendMsg(MSG_TICK, p);
    }

    // --- выбор растений ---
    static void SendScReset() { SendMsg(MSG_SC_RESET, {}); }
    static void SendScReq(uint8_t action, uint8_t seed) {
        std::vector<uint8_t> p; Put(p, action); Put(p, seed); SendMsg(MSG_SC_REQ, p);
    }
    static void SendScExec(uint8_t player, uint8_t action, uint8_t seed) {
        std::vector<uint8_t> p; Put(p, player); Put(p, action); Put(p, seed); SendMsg(MSG_SC_EXEC, p);
    }
    static void SendScReject(uint8_t action, uint8_t seed, const char* why) {
        std::vector<uint8_t> p; Put(p, action); Put(p, seed);
        for (const char* c = why; *c && p.size() < 120; ++c) p.push_back((uint8_t)*c);
        SendMsg(MSG_SC_REJECT, p);
    }

    // Хост принимает решение по действию игрока p (вызывается из потока игры для своих кликов
    // и из сетевого потока для запросов клиента)
    static void HostDecide(int p, uint8_t a, int s) {
        std::lock_guard<std::mutex> lk(scm::g_mx);
        const char* why = scm::ValidateLocked(p, a, s);
        if (why) {
            printf("[sc] player %d %s %d rejected: %s\n", p, scm::ActionName(a), s, why);
            if (p != LocalPlayer()) SendScReject(a, (uint8_t)s, why);
            return;
        }
        scm::ApplyLocked(p, a, s);
        SendScExec((uint8_t)p, a, (uint8_t)s);
    }

    static void SendHash(uint32_t epoch, uint32_t tick, const HashRec& h) {
        std::vector<uint8_t> p;
        Put<uint32_t>(p, epoch); Put<uint32_t>(p, tick);
        for (uint32_t v : h.v) Put<uint32_t>(p, v);
        SendMsg(MSG_HASH, p);
    }

    // --- приём ---

    static bool RecvAll(void* data, int len) {
        char* p = static_cast<char*>(data);
        while (len > 0) {
            int n = recv(g_sock, p, len, 0);
            if (n == 0) return false;
            if (n < 0) {
                if (WSAGetLastError() == WSAETIMEDOUT && g_running) continue;
                return false;
            }
            p += n; len -= n;
        }
        return true;
    }

    static void HandleMessage(uint8_t type, const std::vector<uint8_t>& d) {
        size_t o = 0;
        switch (type) {
        case MSG_HELLO: {
            uint32_t magic = 0; uint16_t proto = 0; uint8_t player = 0;
            Get(d, o, magic); Get(d, o, proto); Get(d, o, player);
            if (magic != kMagic || proto != kProto) {
                printf("[net] wrong HELLO (magic=%08X proto=%u, mine=%u) - different mod version?\n",
                    magic, proto, kProto);
                SetConnected(false);
                return;
            }
            printf("[net] peer says hello: player %u\n", player);
            break;
        }
        case MSG_SEED: {
            uint32_t seed = 0, delay = 10; uint16_t wavePct = 100;
            Get(d, o, seed); Get(d, o, delay); Get(d, o, wavePct);
            session::g_seed = seed;
            session::g_delay = (int)delay;
            session::g_wavePct = wavePct;
            printf("[net] host settings: zombie wave budget %u%%\n", wavePct);
            session::g_nextLevel = 0;
            session::g_haveSeed = true;
            printf("[net] session seed %08X, delay %u ticks. Now start the SAME level as the host.\n",
                seed, delay);
            break;
        }
        case MSG_TICK: {
            uint32_t epoch = 0, tick = 0; uint8_t n = 0;
            if (!Get(d, o, epoch) || !Get(d, o, tick) || !Get(d, o, n)) return;
            std::vector<Cmd> cmds(n);
            for (auto& c : cmds) {
                Get(d, o, c.kind); Get(d, o, c.player); Get(d, o, c.slot);
                Get(d, o, c.col);  Get(d, o, c.row);
                Get(d, o, c.x);    Get(d, o, c.y);   Get(d, o, c.clicks);
            }
            std::lock_guard<std::mutex> lk(g_rxMx);
            g_remoteTicks[Key(epoch, tick)] = std::move(cmds);
            break;
        }
        case MSG_HASH: {
            uint32_t epoch = 0, tick = 0; HashRec h;
            Get(d, o, epoch); Get(d, o, tick);
            for (auto& v : h.v) Get(d, o, v);
            std::lock_guard<std::mutex> lk(g_rxMx);
            g_remoteHashes[Key(epoch, tick)] = h;
            break;
        }
        case MSG_SC_RESET: {
            scm::Reset();
            g_splitBoard = 0;
            printf("[sc] host opened seed selection - choose your 3 plants\n");
            break;
        }
        case MSG_SC_REQ: {  // только хост
            uint8_t a = 0, s = 0;
            Get(d, o, a); Get(d, o, s);
            if (g_cfg.mode == NetMode::Host) HostDecide(2, a, s);
            break;
        }
        case MSG_SC_EXEC: { // только клиент
            uint8_t p = 0, a = 0, s = 0;
            Get(d, o, p); Get(d, o, a); Get(d, o, s);
            std::lock_guard<std::mutex> lk(scm::g_mx);
            scm::ApplyLocked(p, a, s);
            break;
        }
        case MSG_SC_REJECT: {
            uint8_t a = 0, s = 0;
            Get(d, o, a); Get(d, o, s);
            std::string why(d.begin() + std::min(o, d.size()), d.end());
            printf("[sc] host rejected your %s %d: %s\n", scm::ActionName(a), s, why.c_str());
            break;
        }
        case MSG_LEVEL: {
            uint32_t epoch = 0, tick = 0; uint8_t ev = 0;
            Get(d, o, epoch); Get(d, o, ev); Get(d, o, tick);
            std::lock_guard<std::mutex> lk(g_rxMx);
            if (ev == LV_START) {
                g_peerLevel.started = true; g_peerLevel.startEpoch = epoch;
                printf("[net] partner started synced play on level %u\n", epoch);
            }
            else if (ev == LV_LEFT) {
                g_peerLevel.left = true; g_peerLevel.leftEpoch = epoch; g_peerLevel.leftTick = tick;
                printf("[net] partner left level %u at tick %u\n", epoch, tick);
            }
            break;
        }
        default:
            printf("[net] unknown message type %u\n", type);
        }
    }

    static void SetupSocket(SOCKET s) {
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        DWORD timeout = 200;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    }

    static SOCKET AcceptPeer() {
        if (g_listen == INVALID_SOCKET) {
            g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            BOOL reuse = TRUE;
            setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons((u_short)g_cfg.port);
            if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0 || listen(g_listen, 1) != 0) {
                printf("[net] cannot listen on port %d (error %d)\n", g_cfg.port, WSAGetLastError());
                closesocket(g_listen); g_listen = INVALID_SOCKET;
                Sleep(2000);
                return INVALID_SOCKET;
            }
            printf("[net] host: waiting for player 2 on port %d...\n", g_cfg.port);
        }
        while (g_running) {
            fd_set fs; FD_ZERO(&fs); FD_SET(g_listen, &fs);
            timeval tv = { 0, 200000 };
            if (select(0, &fs, nullptr, nullptr, &tv) > 0) {
                sockaddr_in from = {}; int fl = sizeof(from);
                SOCKET s = accept(g_listen, (sockaddr*)&from, &fl);
                if (s != INVALID_SOCKET) {
                    char ip[64] = {};
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    printf("[net] player 2 connected from %s\n", ip);
                    return s;
                }
            }
        }
        return INVALID_SOCKET;
    }

    static SOCKET ConnectToHost() {
        char portStr[16];
        sprintf_s(portStr, "%d", g_cfg.port);
        bool announced = false;
        while (g_running) {
            addrinfo hints = {}, * res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(g_cfg.host, portStr, &hints, &res) == 0 && res) {
                SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                if (s != INVALID_SOCKET && connect(s, res->ai_addr, (int)res->ai_addrlen) == 0) {
                    freeaddrinfo(res);
                    printf("[net] connected to host %s:%d\n", g_cfg.host, g_cfg.port);
                    return s;
                }
                if (s != INVALID_SOCKET) closesocket(s);
                freeaddrinfo(res);
            }
            if (!announced) {
                printf("[net] client: trying to connect to %s:%d (retry every 2 s)...\n",
                    g_cfg.host, g_cfg.port);
                announced = true;
            }
            for (int i = 0; i < 10 && g_running; ++i) Sleep(200);
        }
        return INVALID_SOCKET;
    }

    static DWORD WINAPI NetThread(LPVOID) {
        while (g_running) {
            SOCKET s = (g_cfg.mode == NetMode::Host) ? AcceptPeer() : ConnectToHost();
            if (s == INVALID_SOCKET) continue;

            SetupSocket(s);
            {
                std::lock_guard<std::mutex> lk(g_rxMx);
                g_remoteTicks.clear();
                g_remoteHashes.clear();
                g_peerLevel = PeerLevel();
            }
            g_sock = s;
            SetConnected(true);
            SendHello();

            if (g_cfg.mode == NetMode::Host) {
                // Новый seed сессии на каждое подключение
                LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
                uint32_t seed = (uint32_t)qpc.LowPart ^ (uint32_t)(qpc.HighPart * 2654435761u) ^ GetTickCount();
                session::g_seed = seed;
                session::g_delay = g_cfg.delay;
                session::g_nextLevel = 0;
                session::g_haveSeed = true;
                session::g_wavePct = g_cfg.waveBudget;
                SendSeed(seed, g_cfg.delay, g_cfg.waveBudget);
                printf("[net] session seed %08X sent. Both players: start the SAME level now.\n", seed);
            }

            while (g_running && g_connected) {
                uint16_t len = 0;
                if (!RecvAll(&len, 2) || len == 0) break;
                std::vector<uint8_t> body(len);
                if (!RecvAll(body.data(), len)) break;
                std::vector<uint8_t> payload(body.begin() + 1, body.end());
                HandleMessage(body[0], payload);
            }

            SetConnected(false);
            session::g_haveSeed = false;
            {
                std::lock_guard<std::mutex> lk(g_sendMx);
                closesocket(g_sock);
                g_sock = INVALID_SOCKET;
            }
            if (g_running) printf("[net] disconnected\n");
        }
        return 0;
    }

    static void Start() {
        if (g_cfg.mode == NetMode::Off) {
            printf("[net] network off (mode=off in coop.ini)\n");
            return;
        }
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("[net] WSAStartup failed\n");
            return;
        }
        g_thread = CreateThread(nullptr, 0, NetThread, nullptr, 0, nullptr);
    }

    static void Stop() {
        if (g_cfg.mode == NetMode::Off) return;
        SetConnected(false);
        {
            std::lock_guard<std::mutex> lk(g_sendMx);
            if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
        }
        if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
        if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = nullptr; }
        WSACleanup();
    }
}

// ===========================================================================
// Lockstep
//
// Стартует автоматически на первом игровом тике уровня, если генератор этого уровня
// заполнен из seed сессии. Тики считаются с 0 у обоих. На тике t каждый игрок:
//   1) отправляет свои новые команды на тик t + delay;
//   2) ждёт команды второго игрока на тик t;
//   3) выполняет команды обоих (сначала игрок 1, затем 2);
//   4) раз в 100 тиков сравнивает отпечаток состояния.
// ===========================================================================

static uintptr_t GetChooserForLog() {
    uintptr_t app = GetApp(), ch = 0;
    if (app) SafeRead(app + sc::App_Chooser, ch);
    return ch;
}

namespace ls {
    // Только поток игры
    static uint32_t  g_tick = 0;
    static uint32_t  g_epoch = 0;
    static int       g_delay = 10;
    static uintptr_t g_board = 0;
    static uint32_t  g_gen = 0;             // поколение уровня, на котором идёт lockstep
    static uint32_t  g_lastStartedGen = 0;
    static std::map<uint32_t, std::vector<net::Cmd>> g_localTicks;
    static std::map<uint32_t, net::HashRec> g_localHashes;
    static int       g_desyncReports = 0;
    static uint32_t  g_firstDesyncTick = 0xFFFFFFFF;

    // Очередь команд от клавиатуры (пишет поток консоли)
    static std::mutex g_queueMx;
    static std::vector<net::Cmd> g_queue;

    static void QueueCommand(const net::Cmd& c) {
        std::lock_guard<std::mutex> lk(g_queueMx);
        g_queue.push_back(c);
    }

    // Отпечаток по частям: солнце, счётчик тиков от начала синхронной игры, пакеты, генератор
    struct HashParts {
        int      sun = 0;
        int      counterRel = 0;   // mMainCounter минус его значение на тике 0
        uint32_t packets = 0;
        uint32_t rng = 0;
        uint32_t Total() const {
            uint32_t h = 2166136261u;
            for (uint32_t v : { (uint32_t)sun, (uint32_t)counterRel, packets, rng })
                for (int i = 0; i < 4; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 16777619u; }
            return h;
        }
    };

    static HashParts ComputeParts(uintptr_t board, int counterBase) {
        HashParts hp;
        int counter = 0;
        SafeRead(board + addr::Board_Sun, hp.sun);
        SafeRead(board + addr::Board_MainCounter, counter);
        hp.counterRel = counter - counterBase;
        uint32_t h = 2166136261u;
        auto mix = [&h](uint32_t v) {
            for (int i = 0; i < 4; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 16777619u; }
            };
        uintptr_t bank = GetSeedBankOf(board);
        int n = GetNumPackets(bank);
        mix((uint32_t)n);
        for (int i = 0; i < n; ++i) {
            PacketInfo p;
            if (!ReadPacket(bank, i, p)) continue;
            mix((uint32_t)p.type); mix((uint32_t)p.timesUsed);
            mix((uint32_t)p.active); mix((uint32_t)p.refreshCounter);
        }
        hp.packets = h;
        hp.rng = rng::StateHash();
        return hp;
    }

    static int g_counterBase = 0;   // mMainCounter на тике 0 lockstep

    static net::HashRec ComputeHash(uintptr_t board) {
        HashParts hp = ComputeParts(board, g_counterBase);
        net::HashRec r;
        r.v[0] = hp.Total(); r.v[1] = (uint32_t)hp.sun; r.v[2] = (uint32_t)hp.counterRel;
        r.v[3] = hp.packets; r.v[4] = hp.rng;
        return r;
    }

    static void Begin(uintptr_t board, uint32_t epoch, int delay) {
        g_tick = 0;
        g_epoch = epoch;
        g_delay = delay;
        g_board = board;
        g_gen = session::g_seedGen;
        g_lastStartedGen = g_gen;
        SafeRead(board + addr::Board_MainCounter, g_counterBase);
        g_localTicks.clear();
        g_localHashes.clear();
        g_desyncReports = 0;
        g_firstDesyncTick = 0xFFFFFFFF;
        {
            std::lock_guard<std::mutex> lk(g_queueMx);
            g_queue.clear();
        }
        for (int k = 0; k < delay; ++k) g_localTicks[(uint32_t)k] = {};
        {
            std::lock_guard<std::mutex> lk(net::g_rxMx);
            // Убираем всё от прошлых уровней
            for (auto it = net::g_remoteTicks.begin(); it != net::g_remoteTicks.end();)
                it = ((uint32_t)(it->first >> 32) < epoch) ? net::g_remoteTicks.erase(it) : std::next(it);
            for (auto it = net::g_remoteHashes.begin(); it != net::g_remoteHashes.end();)
                it = ((uint32_t)(it->first >> 32) < epoch) ? net::g_remoteHashes.erase(it) : std::next(it);
            for (int k = 0; k < delay; ++k) {
                uint64_t key = net::Key(epoch, (uint32_t)k);
                if (!net::g_remoteTicks.count(key)) net::g_remoteTicks[key] = {};
            }
        }
        ResetCursorsForSync(board);
        g_lockstepActive = true;
        net::SendLevel(epoch, net::LV_START, 0);
        rtrace::Begin(epoch);
        printf("[ls] LOCKSTEP STARTED: level %u, delay=%d ticks (%d ms). I am player %d. "
            "Level counter=%d, seed selection %s\n",
            epoch, delay, delay * 10, LocalPlayer(), g_counterBase, GetChooserForLog() ? "exists" : "none");
    }

    // notify = сообщить напарнику, что мы ушли с этого уровня
    static void End(const char* reason, bool notify = false) {
        if (!g_lockstepActive) return;
        g_lockstepActive = false;
        rtrace::Stop();
        if (g_foleyProtected)
            printf("[rng] sounds played on this level without touching the game random: %u\n", g_foleyProtected);
        g_foleyProtected = 0;
        if (rtrace::g_outsideTotal)
            printf("[rng] %u random numbers were taken outside the game update on this level\n",
                rtrace::g_outsideTotal);
        if (notify) net::SendLevel(g_epoch, net::LV_LEFT, g_tick);
        printf("[ls] lockstep stopped at tick %u: %s\n", g_tick, reason);
        if (g_firstDesyncTick != 0xFFFFFFFF)
            printf("[ls] first desync was at tick %u\n", g_firstDesyncTick);
    }

    static void CheckHashes(uint32_t uptoTick) {
        std::lock_guard<std::mutex> lk(net::g_rxMx);
        for (auto it = net::g_remoteHashes.begin(); it != net::g_remoteHashes.end();) {
            uint32_t ep = (uint32_t)(it->first >> 32), tk = (uint32_t)it->first;
            if (ep != g_epoch) { ++it; continue; }
            auto mine = g_localHashes.find(tk);
            if (mine == g_localHashes.end()) {
                if (tk < uptoTick) it = net::g_remoteHashes.erase(it);
                else ++it;
                continue;
            }
            const net::HashRec& a = mine->second;
            const net::HashRec& b = it->second;
            if (a.v[0] != b.v[0]) {
                if (g_firstDesyncTick == 0xFFFFFFFF) {
                    g_firstDesyncTick = tk;
                    printf("[ls] *** FIRST DESYNC at tick %u: mine=%08X peer=%08X ***\n", tk, a.v[0], b.v[0]);
                    printf("[ls]     differs:%s%s%s%s   (sun mine=%d peer=%d)\n",
                        a.v[1] != b.v[1] ? " SUN" : "", a.v[2] != b.v[2] ? " TICK-COUNTER" : "",
                        a.v[3] != b.v[3] ? " PACKETS" : "", a.v[4] != b.v[4] ? " RANDOM" : "",
                        (int)a.v[1], (int)b.v[1]);
                    rtrace::Dump(tk >= 100 ? tk - 100 : 0, tk, "first desync");
                    status::Show("Games went out of sync - please restart the level", 15000);
                }
                else if (g_desyncReports % 20 == 0) {
                    printf("[ls] DESYNC at tick %u\n", tk);
                }
                ++g_desyncReports;
            }
            else if (tk % 1000 == 0) {
                printf("[ls] tick %u: in sync (%08X)\n", tk, a.v[0]);
            }
            g_localHashes.erase(mine);
            it = net::g_remoteHashes.erase(it);
        }
    }

    static void Tick(uintptr_t board) {
        const uint32_t t = g_tick;

        // 1) Свои новые команды -> на тик t + delay
        std::vector<net::Cmd> mine;
        {
            std::lock_guard<std::mutex> lk(g_queueMx);
            mine.swap(g_queue);
        }
        g_localTicks[t + g_delay] = mine;
        net::SendTick(g_epoch, t + g_delay, mine);

        // 2) Ждём команды второго игрока на тик t
        std::vector<net::Cmd> theirs;
        bool got = false;
        const uint64_t key = net::Key(g_epoch, t);
        DWORD waitStart = GetTickCount(), lastNote = waitStart;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(net::g_rxMx);
                auto it = net::g_remoteTicks.find(key);
                if (it != net::g_remoteTicks.end()) {
                    theirs = std::move(it->second);
                    net::g_remoteTicks.erase(it);
                    got = true;
                }
            }
            if (got || !net::g_connected || !g_running) break;
            if (GetTickCount() - lastNote > 1000) {
                printf("[ls] waiting for peer at tick %u (%u ms)%s\n", t, GetTickCount() - waitStart,
                    t < (uint32_t)g_delay ? " - peer has not started this level yet?" : "");
                lastNote = GetTickCount();
            }
            Sleep(1);
        }
        if (!got) {
            End(g_running ? "peer disconnected" : "mod unloading");
            return;
        }

        // 3) Команды обоих игроков
        std::vector<net::Cmd> all = std::move(g_localTicks[t]);
        g_localTicks.erase(t);
        all.insert(all.end(), theirs.begin(), theirs.end());
        std::stable_sort(all.begin(), all.end(),
            [](const net::Cmd& a, const net::Cmd& b) { return a.player < b.player; });
        for (const auto& c : all) {
            if (c.kind == net::CMD_PLANT)
                DoPlant(board, { c.player, c.slot, c.col, c.row });
            else if (c.kind == net::CMD_CLICK)
                ExecClick(board, c.player, c.x, c.y, c.clicks);
        }

        // 4) Отпечаток раз в 100 тиков
        if (t % 100 == 0) {
            net::HashRec h = ComputeHash(board);
            g_localHashes[t] = h;
            net::SendHash(g_epoch, t, h);
        }
        CheckHashes(t);

        ++g_tick;
        rtrace::g_tick = g_tick;
    }
}

// ===========================================================================
// Проверка детерминизма на одном компьютере: запись и повтор
//
//   record: на первом игровом тике генератор заполняется из [test] seed, команды F3/F4
//           выполняются на следующем тике и пишутся в coop_replay.txt вместе с отпечатками
//           состояния (каждые 100 тиков).
//   replay: тот же уровень, тот же seed; команды берутся из файла (клавиши F3/F4 игнорируются),
//           отпечатки сравниваются с записанными. Совпадение = игра детерминирована.
//
// Формат файла: "C tick slot col row" — посадка (slot = -1: +100 солнца, F1), "H tick hash" — отпечаток.
// ===========================================================================

namespace replay {
    // kind 1: посадка F3/F4 (slot -1 = +100 солнца, F1); kind 2: клик (x, y, clicks)
    struct Cmd { int kind = 1, slot = 0, col = 0, row = 0, x = 0, y = 0, clicks = 0; };

    // Только поток игры
    static bool      g_active = false;
    static uintptr_t g_board = 0;
    static uint32_t  g_gen = 0;
    static uint32_t  g_lastStartedGen = 0;
    static uint32_t  g_tick = 0;
    static FILE* g_file = nullptr;               // record
    static std::map<uint32_t, std::vector<Cmd>> g_cmds; // replay
    static std::map<uint32_t, ls::HashParts> g_hashes;  // replay
    static int       g_counterBase = 0;
    static int       g_bad[4] = {};   // сколько контрольных точек разошлось: sun, counter, packets, rng
    static uint32_t  g_checked = 0, g_firstMismatch = 0xFFFFFFFF, g_lastRecordedTick = 0;

    // Очередь клавиатуры (record)
    static std::mutex g_queueMx;
    static std::vector<Cmd> g_queue;

    static void Path(char* out, size_t n) {
        strcpy_s(out, n, g_cfg.dir);
        strcat_s(out, n, "coop_replay.txt");
    }

    static void Queue(int slot, int col, int row) {
        Cmd c; c.kind = 1; c.slot = slot; c.col = col; c.row = row;
        std::lock_guard<std::mutex> lk(g_queueMx);
        g_queue.push_back(c);
    }

    static void QueueClick(int x, int y, int clicks) {
        Cmd c; c.kind = 2; c.x = x; c.y = y; c.clicks = clicks;
        std::lock_guard<std::mutex> lk(g_queueMx);
        g_queue.push_back(c);
    }

    static void Begin(uintptr_t board) {
        g_active = true;
        g_board = board;
        g_gen = session::g_seedGen;
        g_lastStartedGen = g_gen;
        g_tick = 0;
        g_checked = 0;
        g_firstMismatch = 0xFFFFFFFF;
        g_lastRecordedTick = 0;
        for (int& b : g_bad) b = 0;
        {
            std::lock_guard<std::mutex> lk(g_queueMx);
            g_queue.clear();
        }
        rng::SeedGlobal(g_cfg.testSeed ^ 0xA5A5A5A5u); // как и в сетевой игре
        ResetCursorsForSync(board);
        SafeRead(board + addr::Board_MainCounter, g_counterBase);
        printf("[test] tick 0 = level counter %d\n", g_counterBase);

        char path[MAX_PATH];
        Path(path, sizeof(path));
        if (g_cfg.test == Config::Test::Record) {
            if (fopen_s(&g_file, path, "w") != 0) g_file = nullptr;
            if (g_file) fprintf(g_file, "# seed %u\n", g_cfg.testSeed);
            printf("[test] RECORDING started -> %s\n", path);
            printf("[test] Play with the MOUSE as usual (clicks are recorded) and/or F3/F4/F1.\n");
            printf("[test] Leave the level via Menu -> Main menu to finish (do not win it).\n");
        }
        else {
            g_cmds.clear();
            g_hashes.clear();
            FILE* f = nullptr;
            if (fopen_s(&f, path, "r") != 0 || !f) {
                printf("[test] cannot open %s - record first\n", path);
                g_active = false;
                return;
            }
            char line[128];
            unsigned a = 0, b = 0;
            while (fgets(line, sizeof(line), f)) {
                int sl = 0, cl = 0, rw = 0;
                if (sscanf_s(line, "C %u %d %d %d", &a, &sl, &cl, &rw) == 4) {
                    Cmd c; c.kind = 1; c.slot = sl; c.col = cl; c.row = rw;
                    g_cmds[a].push_back(c);
                    if (a > g_lastRecordedTick) g_lastRecordedTick = a;
                }
                else if (sscanf_s(line, "K %u %d %d %d", &a, &sl, &cl, &rw) == 4) {
                    Cmd c; c.kind = 2; c.x = sl; c.y = cl; c.clicks = rw;
                    g_cmds[a].push_back(c);
                    if (a > g_lastRecordedTick) g_lastRecordedTick = a;
                }
                else {
                    ls::HashParts hp;
                    unsigned pk = 0, rg = 0;
                    if (sscanf_s(line, "H %u %X %d %d %X %X", &a, &b, &hp.sun, &hp.counterRel, &pk, &rg) == 6) {
                        hp.packets = pk; hp.rng = rg;
                        g_hashes[a] = hp;
                        if (a > g_lastRecordedTick) g_lastRecordedTick = a;
                    }
                }
            }
            fclose(f);
            printf("[test] REPLAY started: %u commands, %u checkpoints, up to tick %u\n",
                (unsigned)g_cmds.size(), (unsigned)g_hashes.size(), g_lastRecordedTick);
            printf("[test] Do not touch the game (alt-tab, moving the window etc. is OK and even useful).\n");
        }
    }

    static void End(const char* reason) {
        if (!g_active) return;
        g_active = false;
        if (g_file) { fclose(g_file); g_file = nullptr; }
        if (g_cfg.test == Config::Test::Record) {
            printf("[test] recording finished at tick %u (%s)\n", g_tick, reason);
        }
        else {
            printf("[test] replay finished at tick %u (%s): %u checkpoints compared, %s\n",
                g_tick, reason, g_checked,
                g_firstMismatch == 0xFFFFFFFF ? "ALL MATCH - deterministic!" : "MISMATCH");
            printf("[test] differing checkpoints: sun=%d counter=%d packets=%d rng=%d\n",
                g_bad[0], g_bad[1], g_bad[2], g_bad[3]);
            if (g_firstMismatch != 0xFFFFFFFF)
                printf("[test] first mismatch at tick %u\n", g_firstMismatch);
        }
    }

    // Поток игры, каждый тик
    static void Tick(uintptr_t board) {
        const uint32_t t = g_tick;
        std::vector<Cmd> cmds;

        if (g_cfg.test == Config::Test::Record) {
            {
                std::lock_guard<std::mutex> lk(g_queueMx);
                cmds.swap(g_queue);
            }
            for (const auto& c : cmds) {
                if (c.kind == 2) {
                    if (g_file) fprintf(g_file, "K %u %d %d %d\n", t, c.x, c.y, c.clicks);
                    printf("[test] t=%u: click recorded\n", t);
                    continue;
                }
                if (g_file) fprintf(g_file, "C %u %d %d %d\n", t, c.slot, c.col, c.row);
                if (c.slot < 0) printf("[test] t=%u: +100 sun (recorded)\n", t);
                else printf("[test] t=%u: slot %d -> col=%d row=%d (recorded)\n", t, c.slot + 1, c.col, c.row);
            }
        }
        else {
            auto it = g_cmds.find(t);
            if (it != g_cmds.end()) cmds = it->second;
        }

        for (const auto& c : cmds) {
            if (c.kind == 2) {
                ExecClick(board, 1, c.x, c.y, c.clicks);
            }
            else if (c.slot < 0) {
                int sun = 0;
                SafeRead(board + addr::Board_Sun, sun);
                SafeWrite(board + addr::Board_Sun, sun + 100);
            }
            else {
                DoPlant(board, { 1, c.slot, c.col, c.row });
            }
        }

        if (t % 100 == 0) {
            ls::HashParts now = ls::ComputeParts(board, g_counterBase);
            if (g_cfg.test == Config::Test::Record) {
                if (g_file) {
                    fprintf(g_file, "H %u %08X %d %d %08X %08X\n", t, now.Total(),
                        now.sun, now.counterRel, now.packets, now.rng);
                    fflush(g_file);
                }
            }
            else {
                auto it = g_hashes.find(t);
                if (it != g_hashes.end()) {
                    const ls::HashParts& rec = it->second;
                    ++g_checked;
                    bool d[4] = { now.sun != rec.sun, now.counterRel != rec.counterRel,
                                  now.packets != rec.packets, now.rng != rec.rng };
                    for (int i = 0; i < 4; ++i) g_bad[i] += d[i];
                    if (d[3] && !d[0] && !d[1] && !d[2] && g_bad[3] <= 10)
                        printf("[test] tick %u: only rng differs (mti now=%d)\n", t,
                            [] { int m = -1; SafeRead(rng::GlobalMTRand + rng::Mti, m); return m; }());
                    bool any = d[0] || d[1] || d[2] || d[3];
                    if (any && g_firstMismatch == 0xFFFFFFFF) {
                        g_firstMismatch = t;
                        printf("[test] *** FIRST MISMATCH at tick %u ***\n", t);
                        printf("[test]     sun     now=%d recorded=%d %s\n", now.sun, rec.sun, d[0] ? "<-- DIFF" : "");
                        printf("[test]     counter now=%d recorded=%d %s\n", now.counterRel, rec.counterRel, d[1] ? "<-- DIFF" : "");
                        printf("[test]     packets now=%08X recorded=%08X %s\n", now.packets, rec.packets, d[2] ? "<-- DIFF" : "");
                        printf("[test]     rng     now=%08X recorded=%08X %s\n", now.rng, rec.rng, d[3] ? "<-- DIFF" : "");
                    }
                    if (t % 1000 == 0) {
                        if (!any) printf("[test] tick %u: matches recording\n", t);
                        else printf("[test] tick %u: differs:%s%s%s%s\n", t,
                            d[0] ? " sun" : "", d[1] ? " counter" : "", d[2] ? " packets" : "", d[3] ? " rng" : "");
                    }
                }
            }
        }
        if (g_cfg.test == Config::Test::Replay && t >= g_lastRecordedTick) {
            ++g_tick;
            End("end of recording");
            return;
        }
        ++g_tick;
    }
}

// --- Экран выбора растений: исполнение действий и перехват кликов ---
using ChooserMouseDown_t = void(__thiscall*)(void* chooser, int x, int y, int clicks);
static ChooserMouseDown_t g_origChooserMouseDown = nullptr;
static bool  g_execChooser = false;   // клик делает сам мод
static uintptr_t g_startApproved = 0;  // экран выбора, для которого «Let's Rock» одобрен хостом
static DWORD g_scWaitSince = 0;

// Кнопка «Let's Rock» (координаты экрана выбора)
constexpr int kLetsRockX = 231, kLetsRockY = 565;
// Кнопка примерно x 160..305, y 545..590 — берём с запасом (ниже кнопки на экране выбора ничего нет)
static bool InLetsRock(int x, int y) { return x >= 140 && x <= 330 && y >= 530 && y <= 600; }

static uintptr_t GetChooser() {
    uintptr_t app = GetApp(), ch = 0;
    if (app) SafeRead(app + sc::App_Chooser, ch);
    return ch;
}

static uintptr_t SeedEntry(uintptr_t ch, int i) { return ch + sc::Seeds + i * sc::SeedSize; }
static int SeedState(uintptr_t ch, int i) { int v = -1; SafeRead(SeedEntry(ch, i) + sc::S_State, v); return v; }
static bool IsFlying(int st) { return st == 0 || st == 2; } // 0 — летит в панель, 2 — летит обратно

static void ClickChooser(uintptr_t ch, int x, int y) {
    g_execChooser = true;
    g_origChooserMouseDown((void*)ch, x, y, 1);
    g_execChooser = false;
}

// Растения игрока 1 — в слоты 0..2, игрока 2 — в 3..5 (по порядку их выбора)
static void ReorderBankForCoop(uintptr_t ch) {
    std::vector<int> seq;
    {
        std::lock_guard<std::mutex> lk(scm::g_mx);
        seq = scm::g_order[1];
        seq.insert(seq.end(), scm::g_order[2].begin(), scm::g_order[2].end());
    }
    int k = 0;
    for (int seed : seq)
        if (SeedState(ch, seed) == sc::StateInBank)
            SafeWrite(SeedEntry(ch, seed) + sc::S_BankIndex, k++);
    printf("[sc] bank reordered: slots 1-3 = player 1, slots 4-6 = player 2\n");
}

// Запасной путь: уровень начался НЕ через «Let's Rock» мода (у кого-то игра стартовала иначе),
// а выбор 3 + 3 у обоих есть. Тогда раскладываем пакеты прямо на панели уровня:
// слоты 1–3 — растения игрока 1, 4–6 — игрока 2 (в порядке выбора), и включаем разделение.
// Вызывается на старте синхронной игры у обоих, результат одинаковый.
static bool CopyMem(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void FixSplitAtLevelStart(uintptr_t board) {
    if (!board || board == g_splitBoard) return;
    std::vector<int> seq;
    {
        std::lock_guard<std::mutex> lk(scm::g_mx);
        if ((int)scm::g_order[1].size() != scm::kPerPlayer || (int)scm::g_order[2].size() != scm::kPerPlayer)
            return;
        seq = scm::g_order[1];
        seq.insert(seq.end(), scm::g_order[2].begin(), scm::g_order[2].end());
        scm::g_started = true;
    }
    uintptr_t bank = GetSeedBankOf(board);
    int n = GetNumPackets(bank);
    if (!bank || n != (int)seq.size()) {
        printf("[sc] level started outside the mod, cannot split the panel (packets=%d)\n", n);
        return;
    }
    const size_t kFrom = 0x24, kLen = addr::SeedPacket_Size - kFrom; // поля SeedPacket (без позиции)
    uint8_t blob[6][0x50] = {};
    int type[6] = {};
    for (int i = 0; i < n; ++i) {
        uintptr_t pk = bank + addr::SeedBank_Packets + i * addr::SeedPacket_Size;
        if (!CopyMem(blob[i], (void*)pk, addr::SeedPacket_Size)) return;
        memcpy(&type[i], blob[i] + addr::Packet_Type, 4);
    }
    int src[6]; bool used[6] = {};
    for (int k = 0; k < n; ++k) {
        src[k] = -1;
        for (int i = 0; i < n; ++i)
            if (!used[i] && type[i] == seq[k]) { src[k] = i; used[i] = true; break; }
        if (src[k] < 0) {
            printf("[sc] level started outside the mod: plant %d not found on the panel\n", seq[k]);
            return;
        }
    }
    for (int k = 0; k < n; ++k) {
        uintptr_t pk = bank + addr::SeedBank_Packets + k * addr::SeedPacket_Size;
        int index = k;
        CopyMem((void*)(pk + kFrom), blob[src[k]] + kFrom, kLen);
        SafeWrite(pk + addr::Packet_Index, index);
    }
    g_splitBoard = board;
    printf("[sc] level started without the mod's Let's Rock - panel rearranged: "
        "slots 1-3 = player 1, slots 4-6 = player 2\n");
}

// Откатить действие в модели, если игра его не приняла (одинаково у обоих — игра одинаковая)
static void RollbackModel(const scm::Ev& e) {
    std::lock_guard<std::mutex> lk(scm::g_mx);
    if (e.action == scm::A_PICK) {
        scm::g_owner[e.seed] = 0;
        auto& v = scm::g_order[e.player];
        v.erase(std::remove(v.begin(), v.end(), (int)e.seed), v.end());
    }
    else if (e.action == scm::A_REMOVE) {
        scm::g_owner[e.seed] = e.player;
        scm::g_order[e.player].push_back(e.seed);
    }
}

// Вернуть true, если действие выполнено (или выполнять нечего), false — подождать
static bool TryExecChooser(uintptr_t ch, const scm::Ev& e) {
    if (e.action == scm::A_PICK || e.action == scm::A_REMOVE) {
        const int i = e.seed;
        const int from = e.action == scm::A_PICK ? sc::StateInChooser : sc::StateInBank;
        const int want = e.action == scm::A_PICK ? sc::StateInBank : sc::StateInChooser;
        int st = SeedState(ch, i);
        if (st == want) return true;
        if (IsFlying(st)) return false;          // летит — подождём
        if (st != from) { printf("[sc] plant %d is in unexpected state %d, skipped\n", i, st); return true; }
        int x = 0, y = 0;
        SafeRead(SeedEntry(ch, i) + sc::S_X, x);
        SafeRead(SeedEntry(ch, i) + sc::S_Y, y);
        ClickChooser(ch, x + 25, y + 35);
        if (SeedState(ch, i) == from) {
            // Игра не приняла клик (растение не открыто, панель полна и т.п.)
            printf("[sc] game refused %s %d - rolled back\n", scm::ActionName(e.action), i);
            RollbackModel(e);
        }
        return true;
    }
    if (e.action == scm::A_START) {
        for (int i = 0; i < sc::NumSeeds; ++i)
            if (IsFlying(SeedState(ch, i))) return false;   // дождёмся, пока всё долетит
        int inBank = 0;
        SafeRead(ch + sc::SeedsInBank, inBank);
        if (inBank != scm::kPerPlayer * 2) return false;      // у этого игрока ещё не всё пришло
        ReorderBankForCoop(ch);
        g_splitBoard = GetBoard();
        g_startApproved = ch;
        ClickChooser(ch, kLetsRockX, kLetsRockY);
        printf("[sc] Let's Rock!\n");
        return true;
    }
    return true;
}

// Выполнить следующее действие из очереди (поток игры, вызывается часто)
static void ProcessChooserQueue() {
    uintptr_t ch = GetChooser();
    if (!ch || !g_origChooserMouseDown) return;

    if (g_cfg.mode == NetMode::Host && net::g_connected) {
        bool restartSelection = false;
        {
            std::lock_guard<std::mutex> lk(scm::g_mx);
            int inBank = -1;
            SafeRead(ch + sc::SeedsInBank, inBank);
            if (scm::g_started && scm::g_queue.empty() && inBank == 0) {
                scm::ResetLocked();
                restartSelection = true;
            }
        }
        if (restartSelection) {
            g_splitBoard = 0;
            net::SendScReset();
            printf("[sc] seed selection opened again - new selection started\n");
        }
    }
    scm::Ev e;
    {
        std::lock_guard<std::mutex> lk(scm::g_mx);
        if (scm::g_queue.empty()) return;
        e = scm::g_queue.front();
    }
    if (TryExecChooser(ch, e)) {
        std::lock_guard<std::mutex> lk(scm::g_mx);
        if (!scm::g_queue.empty()) scm::g_queue.pop_front();
        g_scWaitSince = 0;
    }
    else if (!g_scWaitSince) {
        g_scWaitSince = GetTickCount();
    }
    else if (GetTickCount() - g_scWaitSince > 5000) {
        printf("[sc] still waiting to do: player %d %s %d\n", e.player, scm::ActionName(e.action), e.seed);
        g_scWaitSince = GetTickCount();
    }
}

// --- Хук SeedChooserScreen::OnStartButton (0x493C70, usercall: esi = экран выбора, ret) ---
// Через неё уровень стартует при ЛЮБОМ способе нажать «Let's Rock» (мышью по кнопке,
// после диалога и т.п.). В кооперативе старт разрешён только после решения хоста,
// иначе нажатие превращается в запрос START.
constexpr uintptr_t Fn_ChooserOnStart = 0x493C70;
static void* g_origChooserOnStart = nullptr;
static uint32_t g_allowStartTmp = 0;

static uint32_t __stdcall AllowChooserStart(uintptr_t ch) {
    if (g_cfg.mode == NetMode::Off || !net::g_connected) return 1;
    if (g_execChooser) return 1;
    if (g_startApproved == ch) { g_startApproved = 0; return 1; }   // одобрено хостом, один раз
    // Нажали «Let's Rock» в обход мода — отправляем как запрос, уровень пока не начинаем
    if (g_cfg.mode == NetMode::Host) {
        net::HostDecide(LocalPlayer(), scm::A_START, 0);
    }
    else {
        net::SendScReq(scm::A_START, 0);
        printf("[sc] request: start (Let's Rock)\n");
    }
    return 0;
}

__declspec(naked) static void Hook_ChooserOnStart() {
    __asm {
        pushad
        push esi
        call AllowChooserStart
        mov  g_allowStartTmp, eax
        popad
        cmp  g_allowStartTmp, 0
        je   blocked
        jmp[g_origChooserOnStart]
        blocked:
        ret
    }
}

static void __fastcall Hook_ChooserMouseDown(void* chooser, void* /*edx*/, int x, int y, int clicks) {
    // Без сети, правый клик или клик самого мода — как обычно
    if (g_execChooser || clicks < 0 || g_cfg.mode == NetMode::Off || !net::g_connected) {
        g_origChooserMouseDown(chooser, x, y, clicks);
        return;
    }
    uintptr_t c = (uintptr_t)chooser;
    int hit = -1, hitState = -1;
    for (int i = 0; i < sc::NumSeeds; ++i) {
        int sx = 0, sy = 0, st = 0;
        SafeRead(SeedEntry(c, i) + sc::S_X, sx); SafeRead(SeedEntry(c, i) + sc::S_Y, sy);
        st = SeedState(c, i);
        if (st != sc::StateInChooser && st != sc::StateInBank && !IsFlying(st)) continue; // скрытые
        if (x >= sx && x < sx + 50 && y >= sy && y < sy + 70) { hit = i; hitState = st; break; }
    }
    uint8_t action = 0;
    int seed = 0;
    if (hit >= 0 && hitState == sc::StateInChooser) { action = scm::A_PICK; seed = hit; }
    else if (hit >= 0 && hitState == sc::StateInBank) { action = scm::A_REMOVE; seed = hit; }
    else if (hit >= 0) return;                               // карточка летит — игнорируем
    else if (InLetsRock(x, y)) action = scm::A_START;
    else {
        // Остальное — как обычно, но запоминаем в логе, куда кликнули
        printf("[sc] other click x=%d y=%d (passed to the game)\n", x, y);
        g_origChooserMouseDown(chooser, x, y, clicks);
        if (!GetChooser() || GetChooser() != c)
            printf("[sc] WARNING: the level started without the mod (Let's Rock was not recognized) - "
                "slots are NOT split. Tell Claude these coordinates.\n");
        return;
    }

    if (g_cfg.mode == NetMode::Host) {
        net::HostDecide(LocalPlayer(), action, seed);
    }
    else {
        net::SendScReq(action, (uint8_t)seed);
        printf("[sc] request: %s %d\n", scm::ActionName(action), seed);
    }
}

// --- Хук Board::MouseDown ---
// __fastcall(ecx, edx, ...) совместим с thiscall: ECX = board, EDX не используется,
// аргументы на стеке снимает вызываемая функция.
static void __fastcall Hook_BoardMouseDown(void* board, void* /*edx*/, int x, int y, int clicks) {
    if (!g_execClick) {
        if (g_lockstepActive) {
            net::Cmd c;
            c.kind = net::CMD_CLICK; c.player = (uint8_t)LocalPlayer();
            c.x = (int16_t)x; c.y = (int16_t)y; c.clicks = (int8_t)clicks;
            ls::QueueCommand(c);
            return; // выполнится на тике +delay у обоих
        }
        if (g_cfg.test == Config::Test::Record && replay::g_active) {
            replay::QueueClick(x, y, clicks);
            return; // выполнится на следующем тике и попадёт в запись
        }
        if (g_cfg.test == Config::Test::Replay && replay::g_active) {
            return; // во время повтора свои клики не принимаются
        }
    }
    g_origBoardMouseDown(board, x, y, clicks);
}

// ===========================================================================
// Тик Board (ECX = board) — сюда игра заходит перед каждым игровым тиком
// ===========================================================================

static void* g_origBoardTick = nullptr;

// Одиночный режим (без сети): запросы с клавиатуры
static std::mutex g_soloMx;
static std::vector<PlantRequest> g_soloRequests;

// Функция 0x418D70 вызывается чаще, чем игра реально делает шаг: иногда (пауза, неактивное
// окно и т.п.) она пропускает увеличение mMainCounter. Тик для синхронизации = только тот вызов,
// при котором игра продвинулась, т.е. mMainCounter изменился с прошлого вызова.
static uintptr_t g_stepBoard = 0;
static int       g_stepCounter = 0;

static bool GameAdvanced(uintptr_t board) {
    int c = 0;
    SafeRead(board + addr::Board_MainCounter, c);
    // Новый уровень — только запоминаем счётчик: сам факт появления уровня ещё не шаг игры
    // (уровень создаётся ещё до экрана выбора растений)
    bool advanced = board == g_stepBoard && c != g_stepCounter;
    g_stepBoard = board;
    g_stepCounter = c;
    return advanced;
}

static void __stdcall OnBoardTick(uintptr_t board) {
    const bool advanced = GameAdvanced(board);
    static bool s_announced = false;
    if (!s_announced && GetChooser()) {
        s_announced = true;
        printf("[sc] (info) BoardTick runs while seed selection is open\n");
    }
    ProcessChooserQueue();

    // Уровень сменился — старый lockstep закончен
    if (g_lockstepActive && (board != ls::g_board || ls::g_gen != session::g_seedGen))
        ls::End("level changed", true);

    // Уровень открыт ДО подключения напарника — зомби у вас будут разные, синхронно играть нельзя
    if (advanced && !g_lockstepActive && net::g_connected && session::g_haveSeed &&
        board == session::g_seededBoard && board != session::g_goodBoard &&
        session::g_badStartWarned != board) {
        session::g_badStartWarned = board;
        printf("[ls] ==================================================\n");
        printf("[ls] This level was opened BEFORE the partner connected - it cannot be played together.\n");
        printf("[ls] Both players: go to the main menu and open the level again.\n");
        printf("[ls] ==================================================\n");
        status::Show("Level opened before connecting - reopen it from the main menu", 15000);
    }

    // Автостарт: первый игровой тик уровня, генератор которого заполнен из seed сессии
    if (advanced && !g_lockstepActive && net::g_connected && session::g_haveSeed &&
        board == session::g_goodBoard &&
        board == session::g_seededBoard && session::g_seedGen != ls::g_lastStartedGen) {
        // Экран выбора растений у каждого длился по-разному и потратил разное
        // количество случайных чисел — выравниваем генератор ещё раз
        uint32_t epoch = session::g_seededEpoch;
        rng::SeedGlobal(session::LevelSeed(epoch) ^ 0xA5A5A5A5u);
        FixSplitAtLevelStart(board);
        ls::Begin(board, epoch, session::g_delay);
    }

    if (g_lockstepActive) {
        if (advanced) ls::Tick(board);
        return;
    }

    // Проверка детерминизма (record / replay)
    if (g_cfg.test != Config::Test::Off) {
        if (replay::g_active && (board != replay::g_board || replay::g_gen != session::g_seedGen))
            replay::End("level changed");
        if (advanced && !replay::g_active && board == session::g_seededBoard &&
            session::g_seedGen != 0 && session::g_seedGen != replay::g_lastStartedGen)
            replay::Begin(board);
        if (replay::g_active) {
            if (advanced) replay::Tick(board);
            return;
        }
    }

    std::vector<PlantRequest> reqs;
    {
        std::lock_guard<std::mutex> lk(g_soloMx);
        reqs.swap(g_soloRequests);
    }
    for (const auto& r : reqs) DoPlant(board, r);
}

__declspec(naked) static void Hook_BoardTick() {
    __asm {
        pushad
        push ecx                // board
        call OnBoardTick
        popad
        jmp[g_origBoardTick]
    }
}

// ===========================================================================
// Board::Update — «ворота» lockstep (без зависания игры).
// Если следующий игровой тик требует команд напарника, а они ещё не пришли,
// пропускаем весь Board::Update: поле замирает как на паузе, но окно игры
// отвечает, а под панелью пишется «Waiting for partner...».
// Так же обрабатываются уход напарника с уровня и обрыв связи.
// ===========================================================================
constexpr uintptr_t Fn_BoardUpdate = 0x419190; // thiscall Board::Update(), virtual
using BoardUpdate_t = void(__fastcall*)(void* board, void* edx);
static BoardUpdate_t g_origBoardUpdate = nullptr;

namespace gate {
    static bool  g_waiting = false;
    static DWORD g_waitStart = 0, g_lastNote = 0;

    static void PartnerGone(const char* why, const char* screen) {
        ls::End(why);
        g_splitBoard = 0;           // дальше играешь один: все слоты твои, панель обычная
        g_waiting = false;
        status::Show(screen, 8000);
        printf("[ls] ==================================================\n");
        printf("[ls] %s. You continue alone on this level.\n", why);
        printf("[ls] To play together again: both go to the main menu and start the same level.\n");
        printf("[ls] ==================================================\n");
    }

    // true = пропустить этот Board::Update
    static bool Hold(uintptr_t board) {
        if (!g_lockstepActive || board != ls::g_board) {
            if (g_waiting) { g_waiting = false; status::Hide(); }
            return false;
        }
        if (!net::g_connected) {
            PartnerGone("Partner disconnected", "Partner disconnected - playing alone");
            return false;
        }
        int c = 0;
        SafeRead(board + addr::Board_MainCounter, c);
        const bool needTick = board == g_stepBoard && c != g_stepCounter;
        if (!needTick) return false;

        bool have = false; net::PeerLevel peer;
        {
            std::lock_guard<std::mutex> lk(net::g_rxMx);
            have = net::g_remoteTicks.count(net::Key(ls::g_epoch, ls::g_tick)) != 0;
            peer = net::g_peerLevel;
        }
        if (have) {
            if (g_waiting) {
                DWORD waited = GetTickCount() - g_waitStart;
                if (waited >= 1000) printf("[ls] partner is back after %u ms\n", waited);
                g_waiting = false;
                status::Hide();
            }
            return false;
        }
        // Напарник ушёл с этого уровня — его команд больше не будет
        if (peer.left && peer.leftEpoch == ls::g_epoch) {
            PartnerGone("Partner left the level (restart or main menu)", "Partner left the level - playing alone");
            return false;
        }
        DWORD now = GetTickCount();
        if (!g_waiting) { g_waiting = true; g_waitStart = now; g_lastNote = now; }
        if (now - g_waitStart > 300) status::Show("Waiting for partner...");
        if (now - g_lastNote > 3000) {
            g_lastNote = now;
            const char* hint = "";
            if (!peer.started || peer.startEpoch < ls::g_epoch)
                hint = " - partner has not started this level yet";
            else if (peer.startEpoch > ls::g_epoch)
                hint = " - partner is on a NEWER level (did they restart?)";
            printf("[ls] waiting for partner at tick %u (%u s)%s\n",
                ls::g_tick, (now - g_waitStart) / 1000, hint);
        }
        return true;
    }
}

// Widget::MarkDirty (vtable +0x44) — чтобы поле перерисовывалось и надпись была видна,
// пока Board::Update пропускается
static void MarkBoardDirty(void* board) {
    using MarkDirty_t = void(__fastcall*)(void* w, void* edx);
    uintptr_t vt = 0, fn = 0;
    if (!SafeRead((uintptr_t)board, vt) || !vt || !SafeRead(vt + 0x44, fn) || !fn) return;
    __try { reinterpret_cast<MarkDirty_t>(fn)(board, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void __fastcall Hook_BoardUpdate(void* board, void* edx) {
    if (gate::Hold((uintptr_t)board)) { MarkBoardDirty(board); return; }
    g_inBoardUpdate = true;
    g_origBoardUpdate(board, edx);
    g_inBoardUpdate = false;
}

// ===========================================================================
// Зонд: ищем Board::MouseDown — функцию, которая вызывает MouseDownWithPlant.
// При запуске мод находит в коде все вызовы 0x413250, ставит хук на начало вызывающих
// функций и при каждом клике печатает регистры и первые аргументы на стеке.
// ===========================================================================

namespace probe {
    static void* g_orig[2] = {};
    static uintptr_t g_target[2] = {};
    static volatile LONG g_count = 0;

    // r: EDI ESI EBP ESP EBX EDX ECX EAX (порядок pushad), затем адрес возврата и аргументы стека
    static void __stdcall OnProbe(int id, const uint32_t* r) {
        LONG n = InterlockedIncrement(&g_count);
        if (n > 300) return;
        const uint32_t board = (uint32_t)GetBoard();
        auto tag = [board](uint32_t v) { return v == board && board ? "(board)" : ""; };
        printf("[probe%d] #%ld fn=%08X ret=%08X\n", id, n, (unsigned)g_target[id], r[8]);
        printf("[probe%d]   eax=%08X%s ecx=%08X%s edx=%08X%s ebx=%08X%s esi=%08X%s edi=%08X%s\n", id,
            r[7], tag(r[7]), r[6], tag(r[6]), r[5], tag(r[5]), r[4], tag(r[4]), r[1], tag(r[1]), r[0], tag(r[0]));
        printf("[probe%d]   stack args: %08X%s %08X%s %08X%s %08X%s %08X%s\n", id,
            r[9], tag(r[9]), r[10], tag(r[10]), r[11], tag(r[11]), r[12], tag(r[12]), r[13], tag(r[13]));
        printf("[probe%d]   as int:     %d %d %d %d %d   | ecx=%d edx=%d\n", id,
            (int)r[9], (int)r[10], (int)r[11], (int)r[12], (int)r[13], (int)r[6], (int)r[5]);
    }

    __declspec(naked) static void Hook0() {
        __asm {
            pushad
            push esp            // указатель на сохранённые регистры
            push 0              // id
            call OnProbe
            popad
            jmp[g_orig + 0]
        }
    }
    __declspec(naked) static void Hook1() {
        __asm {
            pushad
            push esp
            push 1
            call OnProbe
            popad
            jmp[g_orig + 4]
        }
    }

    // Найти функции, которые вызывают target (call rel32), и вернуть их начала
    static std::vector<uintptr_t> FindCallerFunctions(uintptr_t target) {
        std::vector<uintptr_t> result;
        auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t* b = base + sec->VirtualAddress;
            uint8_t* e = b + sec->Misc.VirtualSize;
            for (uint8_t* c = b; c + 5 <= e; ++c) {
                if (*c != 0xE8) continue;
                int32_t rel; memcpy(&rel, c + 1, 4);
                if ((uintptr_t)(c + 5 + rel) != target) continue;
                uint8_t* fn = c;
                bool found = false;
                for (int back = 0; back < 0x4000 && fn > b + 2; ++back, --fn)
                    if (fn[-1] == 0xCC && fn[-2] == 0xCC) { found = true; break; }
                if (!found) {
                    printf("[probe] call to %08X at %08X: function start NOT found, skipped\n",
                        (unsigned)target, (unsigned)(uintptr_t)c);
                    continue;
                }
                printf("[probe] call to %08X at %08X, caller function starts at %08X\n",
                    (unsigned)target, (unsigned)(uintptr_t)c, (unsigned)(uintptr_t)fn);
                if (std::find(result.begin(), result.end(), (uintptr_t)fn) == result.end())
                    result.push_back((uintptr_t)fn);
            }
        }
        return result;
    }
}

static bool CreateHook(uintptr_t target, void* detour, void** original, const char* name);

static void InstallProbes() {
    auto callers = probe::FindCallerFunctions(addr::Fn_MouseDownWithPlant);
    void* hooks[2] = { (void*)&probe::Hook0, (void*)&probe::Hook1 };
    for (size_t i = 0; i < callers.size() && i < 2; ++i) {
        probe::g_target[i] = callers[i];
        char name[32];
        sprintf_s(name, "probe%u", (unsigned)i);
        CreateHook(callers[i], hooks[i], &probe::g_orig[i], name);
    }
    if (callers.empty()) printf("[probe] no callers of MouseDownWithPlant found\n");
}

// ===========================================================================
// Установка хуков
// ===========================================================================

static bool CreateHook(uintptr_t target, void* detour, void** original, const char* name) {
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
    if (st != MH_OK) {
        printf("[coop] hook %s @%08X failed: %s\n", name, (unsigned)target, MH_StatusToString(st));
        return false;
    }
    printf("[coop] hook %s @%08X created\n", name, (unsigned)target);
    return true;
}

// ===========================================================================
// Оформление панели: правая половина (слоты 4–6, растения игрока 2) — сиреневая.
// Рисуется только на уровне, где панель поделена 3 + 3. Чисто визуально:
// на синхронизацию не влияет.
//
// Как: SeedBank::Draw рисует фон панели (картинка + правый край), потом пакеты.
// Сразу после правого края (вызов DrawImage с адресом возврата 0x497E15)
// перерисовываем фон правее раздела:
//   1) та же картинка с окрашиванием (умножение) — гасим красный/зелёный;
//   2) заливка в режиме сложения — добавляем сиреневый.
// Текстура (тени, углубления) сохраняется, меняется только цвет.
// ===========================================================================
namespace ui {
    constexpr uintptr_t Fn_SeedBankDraw = 0x497CD0; // usercall: esi = Graphics*, [esp+4] = SeedBank*, ret 4
    constexpr uintptr_t Fn_DrawImage = 0x59F180; // thiscall Graphics::DrawImage(img, x, y)
    constexpr uintptr_t Fn_DrawImageSrc = 0x59F2C0; // thiscall Graphics::DrawImage(img, x, y, const Rect& src)
    constexpr uintptr_t Fn_FillRect = 0x59EB80; // thiscall Graphics::FillRect(x, y, w, h)
    constexpr uintptr_t Fn_SetClipRect = 0x59FB10; // thiscall Graphics::SetClipRect(x, y, w, h)
    constexpr uintptr_t Ret_BankEdge = 0x497E15; // адрес возврата после рисования правого края панели
    constexpr uintptr_t Fn_AppGetImage = 0x5E04E0; // thiscall SexyAppBase::GetImage(const std::string&, bool commitBits) -> Image*
    constexpr uintptr_t Fn_StrCtor = 0x405F00; // thiscall std::string::string(const char*)
    constexpr uintptr_t Fn_StrDtor = 0x401AE0; // thiscall std::string::~string()
    constexpr uintptr_t Var_ImageSeedBank = 0x72F618; // Sexy::IMAGE_SEEDBANK (Image*)
    constexpr const char* kCustomFile = "coop_seedbank.png"; // своя панель, рядом с Coop.dll
    // Graphics
    constexpr uintptr_t G_Clip = 0x20;          // Rect {x, y, w, h}
    constexpr uintptr_t G_Color = 0x30;          // Color {r, g, b, a}
    constexpr uintptr_t G_DrawMode = 0x44;          // 0 = обычный, 1 = сложение
    constexpr uintptr_t G_Colorize = 0x48;          // bool: умножать картинки на цвет
    // Image
    constexpr uintptr_t Img_W = 0x24, Img_H = 0x28;
    // SeedBank / SeedPacket
    constexpr uintptr_t Bank_Board = 0x4;
    constexpr uintptr_t P_X = 0x8;

    struct Rgba { int r, g, b, a; };
    // Подбор цвета: итог ≈ текстура * kMul + kAdd
    //   оранжевая рамка (205,95,30)  -> ~ (187,137,205)
    //   коричневые ниши (115,50,20)  -> ~ (147,110,195)
    constexpr Rgba kMul = { 115, 153, 255, 255 };  // 0.45, 0.60, 1.00
    constexpr Rgba kAdd = { 95,  80, 175, 255 };
    constexpr int  kCornerR = 8;                   // скругление правых углов панели

    using DrawImage_t = void(__fastcall*)(void* g, void* edx, void* img, int x, int y);
    using DrawImageSrc_t = void(__fastcall*)(void* g, void* edx, void* img, int x, int y, void* src);
    using Rect4_t = void(__fastcall*)(void* g, void* edx, int x, int y, int w, int h);

    static DrawImageSrc_t g_origDrawImageSrc = nullptr;
    static void* g_origSeedBankDraw = nullptr;
    static volatile uintptr_t g_curBank = 0;      // SeedBank, который сейчас рисуется
    static bool g_inTint = false;
    static bool g_reported = false;

    static void SetColor(uintptr_t g, const Rgba& c) {
        SafeWrite(g + G_Color + 0x0, c.r); SafeWrite(g + G_Color + 0x4, c.g);
        SafeWrite(g + G_Color + 0x8, c.b); SafeWrite(g + G_Color + 0xC, c.a);
    }
    static void Fill(uintptr_t g, int x, int y, int w, int h) {
        if (w > 0 && h > 0) reinterpret_cast<Rect4_t>(Fn_FillRect)((void*)g, nullptr, x, y, w, h);
    }
    static int CornerInset(int r, int dy) {
        if (r <= 0 || dy >= r) return 0;
        double t = r - dy - 0.5;
        return (int)(r - sqrt((double)r * r - t * t) + 0.5);
    }
    // Залитый прямоугольник: левый край прямой, правые углы скруглены
    static void FillRoundRight(uintptr_t g, int x, int y, int w, int h, int r) {
        int runStart = 0, prev = -1;
        for (int row = 0; row <= h; ++row) {
            int cur = -1;
            if (row < h) cur = CornerInset(r, row < h / 2 ? row : h - 1 - row);
            if (row == h || cur != prev) {
                if (prev >= 0) Fill(g, x, y + runStart, w - prev, row - runStart);
                runStart = row; prev = cur;
            }
        }
    }

    // Диагностика: каждая причина пропуска печатается один раз
    static unsigned g_dbgMask = 0;
    static void Dbg(unsigned bit, const char* fmt, ...) {
        if (g_dbgMask & bit) return;
        g_dbgMask |= bit;
        char buf[256];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        printf("[ui] %s\n", buf);
    }

    static void TintRightHalf(uintptr_t g, uintptr_t bank, void* img, int ex, int ey, void* src) {
        uintptr_t board = 0;
        SafeRead(bank + Bank_Board, board);
        if (!board || board != g_splitBoard) return;
        int num = 0;
        if (!SafeRead(bank + addr::SeedBank_NumPackets, num) || num < 6) { Dbg(0x20, "skip: packets=%d", num); return; }
        int x3 = 0, imgH = 0, srcW = 0;
        SafeRead(bank + addr::SeedBank_Packets + 3 * addr::SeedPacket_Size + P_X, x3);
        SafeRead((uintptr_t)img + Img_H, imgH);
        SafeRead((uintptr_t)src + 0x8, srcW);
        if (x3 <= 0 || imgH <= 0 || srcW <= 0) {
            Dbg(0x40, "skip: x3=%d imgH=%d srcW=%d edge=(%d,%d)", x3, imgH, srcW, ex, ey);
            return;
        }

        const int splitX = x3 - 4;          // середина промежутка между 3-м и 4-м пакетом
        const int endX = ex + srcW;       // правый край панели (с учётом удлинения)
        if (endX <= splitX) return;

        if (!g_reported) {
            g_reported = true;
            printf("[ui] tint right half: x=%d..%d h=%d\n", splitX, endX, imgH);
        }

        int clip[4] = {}, color[4] = {}, mode = 0; unsigned char colorize = 0;
        for (int i = 0; i < 4; ++i) { SafeRead(g + G_Clip + i * 4, clip[i]); SafeRead(g + G_Color + i * 4, color[i]); }
        SafeRead(g + G_DrawMode, mode);
        SafeRead(g + G_Colorize, colorize);

        g_inTint = true;
        reinterpret_cast<Rect4_t>(Fn_SetClipRect)((void*)g, nullptr, splitX, ey - 10, endX - splitX + 10, imgH + 20);

        // 1) перерисовать фон справа с умножением цвета
        SafeWrite(g + G_DrawMode, 0);
        SafeWrite(g + G_Colorize, (unsigned char)1);
        SetColor(g, kMul);
        reinterpret_cast<DrawImage_t>(Fn_DrawImage)((void*)g, nullptr, img, 0, 0);
        g_origDrawImageSrc((void*)g, nullptr, img, ex, ey, src);

        // 2) добавить сиреневый (режим сложения), не задевая прозрачные углы
        SafeWrite(g + G_Colorize, (unsigned char)0);
        SafeWrite(g + G_DrawMode, 1);
        SetColor(g, kAdd);
        FillRoundRight(g, splitX, 0, endX - splitX, imgH, kCornerR);
        g_inTint = false;

        for (int i = 0; i < 4; ++i) { SafeWrite(g + G_Clip + i * 4, clip[i]); SafeWrite(g + G_Color + i * 4, color[i]); }
        SafeWrite(g + G_DrawMode, mode);
        SafeWrite(g + G_Colorize, colorize);
    }

    // ---------------- своя картинка панели (coop_seedbank.png) ----------------
    // Грузится через движок игры (SexyAppBase::GetImage), один раз на уровень 3+3.
    // Если файла нет — используется программная покраска (TintRightHalf).
    static uintptr_t g_customImg = 0;
    static uintptr_t g_customFor = 0;      // для какого Board уже пробовали загрузить
    static uintptr_t g_savedImg = 0;
    static bool g_swapped = false;

    using StrCtor_t = void* (__fastcall*)(void* str, void* edx, const char* text);
    using StrDtor_t = void(__fastcall*)(void* str, void* edx);
    using GetImage_t = void* (__fastcall*)(void* app, void* edx, void* str, bool commitBits);

    static uintptr_t LoadGameImage(const char* path) {
        uintptr_t app = GetApp();
        if (!app) return 0;
        alignas(4) unsigned char str[32] = {};          // std::string игры: 28 байт
        reinterpret_cast<StrCtor_t>(Fn_StrCtor)(str, nullptr, path);
        void* img = nullptr;
        __try {
            img = reinterpret_cast<GetImage_t>(Fn_AppGetImage)((void*)app, nullptr, str, false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            img = nullptr;
        }
        reinterpret_cast<StrDtor_t>(Fn_StrDtor)(str, nullptr);
        return (uintptr_t)img;
    }

    static void EnsureCustomImage(uintptr_t board) {
        if (g_customFor == board) return;
        g_customFor = board;
        char path[MAX_PATH];
        strcpy_s(path, g_cfg.dir);
        strcat_s(path, kCustomFile);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            if (!g_customImg) Dbg(0x100, "no %s - using built-in purple tint", kCustomFile);
            return;                                    // уже загруженную оставляем
        }
        uintptr_t img = LoadGameImage(path);
        if (!img) { printf("[ui] failed to load %s\n", path); return; }
        g_customImg = img;
        int w = 0, h = 0, ow = 0, oh = 0; uintptr_t orig = 0;
        SafeRead(img + Img_W, w); SafeRead(img + Img_H, h);
        if (SafeRead(Var_ImageSeedBank, orig) && orig) { SafeRead(orig + Img_W, ow); SafeRead(orig + Img_H, oh); }
        printf("[ui] custom panel loaded: %s (%dx%d, original %dx%d)%s\n", kCustomFile, w, h, ow, oh,
            (w != ow || h != oh) ? "  WARNING: size differs from the original!" : "");
    }

    static void __cdecl PreBankDraw(uintptr_t bank) {
        g_swapped = false;
        uintptr_t board = 0;
        if (!SafeRead(bank + Bank_Board, board) || !board || board != g_splitBoard) return;
        EnsureCustomImage(board);
        if (!g_customImg) return;
        if (!SafeRead(Var_ImageSeedBank, g_savedImg) || !g_savedImg) return;
        SafeWrite(Var_ImageSeedBank, g_customImg);    // на время рисования панели — наша картинка
        g_swapped = true;
    }

    // ---------------- текст под панелью («Waiting for partner...» и т.п.) ----------------
    constexpr uintptr_t Var_FontSun = 0x72EDE4;   // шрифт, которым пишется сумма солнца
    using StrDraw_t = void(__fastcall*)(void* font, void* edx, void* g, int x, int y,
        void* str, void* color, void* clip); // Font vtable +0x28

    static void DrawStatusText(uintptr_t g, const char* text, int x, int y) {
        uintptr_t font = 0, vt = 0, fn = 0;
        if (!SafeRead(Var_FontSun, font) || !font) return;
        if (!SafeRead(font, vt) || !vt || !SafeRead(vt + 0x28, fn) || !fn) return;
        alignas(4) unsigned char str[32] = {};
        reinterpret_cast<StrCtor_t>(Fn_StrCtor)(str, nullptr, text);
        int shadow[4] = { 0, 0, 0, 220 };
        int main[4] = { 255, 235, 120, 255 };
        __try {
            reinterpret_cast<StrDraw_t>(fn)((void*)font, nullptr, (void*)g, x + 2, y + 2, str, shadow, (void*)(g + G_Clip));
            reinterpret_cast<StrDraw_t>(fn)((void*)font, nullptr, (void*)g, x, y, str, main, (void*)(g + G_Clip));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Dbg(0x200, "text drawing failed");
        }
        reinterpret_cast<StrDtor_t>(Fn_StrDtor)(str, nullptr);
    }

    static void __cdecl PostBankDraw(uintptr_t g) {
        if (g_swapped) SafeWrite(Var_ImageSeedBank, g_savedImg);
        g_swapped = false;
        if (const char* t = status::Current()) DrawStatusText(g, t, 12, 87 + 24);
    }

    static void __fastcall Hook_DrawImageSrc(void* g, void* edx, void* img, int x, int y, void* src) {
        void* ret = _ReturnAddress();
        g_origDrawImageSrc(g, edx, img, x, y, src);
        if (g_inTint || g_swapped || !g_curBank) return;
        if (ret == (void*)Ret_BankEdge) TintRightHalf((uintptr_t)g, g_curBank, img, x, y, src);
    }

    // Обёртка SeedBank::Draw (usercall: esi = Graphics*, [esp+4] = SeedBank*, ret 4)
    __declspec(naked) static void Hook_SeedBankDraw() {
        __asm {
            pushad
            push dword ptr[esp + 0x24]          // SeedBank*
            call PreBankDraw
            add  esp, 4
            popad
            mov  eax, [esp + 4]
            mov  dword ptr[g_curBank], eax
            push eax                             // SeedBank* (оригинал снимет его: ret 4)
            call dword ptr[g_origSeedBankDraw]  // esi = Graphics* сохраняется
            mov  dword ptr[g_curBank], 0
            pushad
            push esi                             // Graphics*
            call PostBankDraw
            add  esp, 4
            popad
            ret  4
        }
    }
}

// ===========================================================================
// Звуки (TodFoley) не должны трогать игровой генератор случайных чисел.
//
// Найдено по трассировке рассинхрона: TodFoley::PlayFoley (0x5237C0) берёт случайное число
// (какой вариант звука сыграть), но только если такой звук не играл последние 10 обновлений
// ПРИЛОЖЕНИЯ — а этот счётчик идёт по-разному у двух компьютеров (пауза, ожидание напарника,
// разная частота кадров). В итоге один компьютер берёт случайное число, другой нет,
// и дальше у них по-разному выходят зомби и падает солнце.
//
// Решение: на время проигрывания звука сохраняем состояние генератора и потом возвращаем.
// Звук по-прежнему случайный, а игра у обоих одинаковая.
//   0x5237C0: usercall eax = тип звука, ecx = TodFoley*, [esp+4] = float, ret 4
//   0x5239D0: usercall esi = тип звука, [esp+4] = ..., ret 4 (случайная высота + вызов 0x5237C0)
// ===========================================================================
namespace foley {
    constexpr uintptr_t Fn_PlayFoleyPitch = 0x5237C0;
    constexpr uintptr_t Fn_PlayFoley = 0x5239D0;
    constexpr size_t    kStateSize = rng::Mti + 4;      // mt[624] + mti
    static uint8_t g_saved[kStateSize];
    static bool    g_haveSaved = false;
    static void* g_origPitch = nullptr;
    static void* g_origPlay = nullptr;

    static void __cdecl Enter() {
        if (g_inFoley++ == 0 && g_lockstepActive) {
            g_haveSaved = CopyMem(g_saved, (void*)rng::GlobalMTRand, kStateSize);
        }
    }
    static void __cdecl Leave() {
        if (--g_inFoley == 0 && g_haveSaved) {
            CopyMem((void*)rng::GlobalMTRand, g_saved, kStateSize);
            g_haveSaved = false;
            ++g_foleyProtected;
        }
        if (g_inFoley < 0) g_inFoley = 0;
    }

    __declspec(naked) static void Hook_Pitch() {
        __asm {
            pushad
            call Enter
            popad
            push dword ptr[esp + 4]           // тот же аргумент (float)
            call dword ptr[g_origPitch]       // eax/ecx уже восстановлены popad; ret 4 снимет копию
            pushad                              // сохраняем результат (eax) вместе с остальными
            call Leave
            popad
            ret  4
        }
    }
    __declspec(naked) static void Hook_Play() {
        __asm {
            pushad
            call Enter
            popad
            push dword ptr[esp + 4]
            call dword ptr[g_origPlay]
            pushad
            call Leave
            popad
            ret  4
        }
    }
}

// ===========================================================================
// Сложность: «бюджет» волн зомби.
//
// Board::PickZombieWaves (0x40C7D0) при создании уровня для каждой волны считает очки
// (обычно номер_волны / 3 + 1, на флаговой волне ещё ×2,5) и «покупает» на них зомби:
// обычный стоит 1, конус 2, ведро 4, футболист 7 и т.д. (таблица по 0x723B20, шаг 0x1C).
// В точке 0x40CC22 очки волны уже посчитаны и лежат в ECX — умножаем их на процент.
// Больше очков = больше зомби и чаще дорогие. Волны создаются при открытии уровня,
// поэтому процент должен быть известен обоим ДО этого: его присылает хост при подключении.
// ===========================================================================
namespace waves {
    constexpr uintptr_t Hook_Points = 0x40CC22;   // внутри PickZombieWaves, ECX = очки волны
    static void* g_orig = nullptr;
    static DWORD g_lastNote = 0;

    static int CurrentPct() {
        if (g_netConnected && session::g_haveSeed) return session::g_wavePct;
        return g_cfg.waveBudget;
    }

    static uint32_t __stdcall Scale(uint32_t pts) {
        int pct = CurrentPct();
        if (pct == 100 || (int)pts <= 0) return pts;
        uint32_t scaled = (uint32_t)(((uint64_t)pts * (uint32_t)pct + 50) / 100);
        if (scaled < 1) scaled = 1;
        DWORD now = GetTickCount();
        if (now - g_lastNote > 3000) {   // одна строка на уровень, а не на каждую волну
            g_lastNote = now;
            printf("[game] zombie wave budget %d%% (first wave: %u -> %u points)\n", pct, pts, scaled);
        }
        return scaled;
    }

    __declspec(naked) static void Hook() {
        __asm {
            pushad
            push ecx
            call Scale
            mov[esp + 0x18], eax       // новое значение ECX (место ECX в pushad)
            popad
            jmp[g_orig]
        }
    }
}

static bool InstallHooks() {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK) {
        printf("[coop] MH_Initialize failed: %s\n", MH_StatusToString(st));
        return false;
    }
    CreateHook(addr::Fn_TakeSun, &Hook_TakeSun, &g_origTakeSun, "TakeSunMoney");
    CreateHook(addr::Fn_MouseDownWithPlant, &Hook_MouseDownWithPlant,
        &g_origMouseDownWithPlant, "MouseDownWithPlant");
    CreateHook(addr::Fn_BoardTick, &Hook_BoardTick, &g_origBoardTick, "BoardTick");
    CreateHook(rng::Fn_NextNoAssert, &Hook_NextNoAssert, &g_origNextNoAssert, "MTRand::NextNoAssert");
    CreateHook(Fn_BoardMouseDown, (void*)&Hook_BoardMouseDown, (void**)&g_origBoardMouseDown, "Board::MouseDown");
    CreateHook(sc::Fn_MouseDown, (void*)&Hook_ChooserMouseDown, (void**)&g_origChooserMouseDown, "SeedChooser::MouseDown");
    CreateHook(Fn_ChooserOnStart, (void*)&Hook_ChooserOnStart, &g_origChooserOnStart, "SeedChooser::OnStartButton");
    CreateHook(Fn_BoardUpdate, (void*)&Hook_BoardUpdate, (void**)&g_origBoardUpdate, "Board::Update");
    CreateHook(waves::Hook_Points, (void*)&waves::Hook, &waves::g_orig, "PickZombieWaves (wave budget)");
    CreateHook(foley::Fn_PlayFoleyPitch, (void*)&foley::Hook_Pitch, &foley::g_origPitch, "TodFoley::PlayFoleyPitch");
    CreateHook(foley::Fn_PlayFoley, (void*)&foley::Hook_Play, &foley::g_origPlay, "TodFoley::PlayFoley");
    CreateHook(ui::Fn_SeedBankDraw, (void*)&ui::Hook_SeedBankDraw, &ui::g_origSeedBankDraw, "SeedBank::Draw");
    CreateHook(ui::Fn_DrawImageSrc, (void*)&ui::Hook_DrawImageSrc, (void**)&ui::g_origDrawImageSrc, "Graphics::DrawImage(src)");

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK && st != MH_ERROR_NOT_CREATED) {
        printf("[coop] MH_EnableHook failed: %s\n", MH_StatusToString(st));
        return false;
    }
    return true;
}

static void RemoveHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

// ===========================================================================
// Диагностика
// ===========================================================================

// F2: поиск счётчиков тиков внутри Board
static void FindTickCounters() {
    constexpr size_t kBoardScanSize = 0x6000;
    uintptr_t board = GetBoard();
    if (!board) { printf("[scan] not in a level\n"); return; }

    std::vector<int> a(kBoardScanSize / 4), b(kBoardScanSize / 4);
    for (size_t i = 0; i < a.size(); ++i) SafeRead(board + i * 4, a[i]);
    DWORD t0 = GetTickCount();
    Sleep(1000);
    for (size_t i = 0; i < b.size(); ++i) SafeRead(board + i * 4, b[i]);
    DWORD dt = GetTickCount() - t0;
    if (GetBoard() != board) { printf("[scan] board changed, try again\n"); return; }

    printf("[scan] board=%08X, %u ms. Candidates:\n", (unsigned)board, (unsigned)dt);
    int found = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int d = b[i] - a[i];
        if (d >= 80 && d <= 120) {
            printf("[scan]   +%04X: %d -> %d (%+d)\n", (unsigned)(i * 4), a[i], b[i], d);
            ++found;
        }
    }
    if (!found) printf("[scan]   nothing found — was the game window focused and unpaused?\n");
}

// F7/F8: снимок и сравнение панели семян
static std::vector<int> g_seedBankSnap;
static uintptr_t g_seedBankSnapAddr = 0;

static void SnapSeedBank() {
    uintptr_t bank = GetSeedBankOf(GetBoard());
    if (!bank) { printf("[bank] no seedbank\n"); return; }
    g_seedBankSnap.assign(0x400 / 4, 0);
    for (size_t i = 0; i < g_seedBankSnap.size(); ++i) SafeRead(bank + i * 4, g_seedBankSnap[i]);
    g_seedBankSnapAddr = bank;
    printf("[bank] snapshot saved (%08X)\n", (unsigned)bank);
}

static void DiffSeedBank() {
    uintptr_t bank = GetSeedBankOf(GetBoard());
    if (!bank || bank != g_seedBankSnapAddr || g_seedBankSnap.empty()) {
        printf("[bank] no snapshot for this seedbank, press F7 first\n");
        return;
    }
    int changed = 0;
    for (size_t i = 0; i < g_seedBankSnap.size(); ++i) {
        int v = 0;
        SafeRead(bank + i * 4, v);
        if (v != g_seedBankSnap[i]) {
            printf("[bank] +%03X: %d -> %d  (%08X -> %08X)\n", (unsigned)(i * 4),
                g_seedBankSnap[i], v, (unsigned)g_seedBankSnap[i], (unsigned)v);
            ++changed;
        }
    }
    printf("[bank] %d ints changed\n", changed);
}

// F9: поиск генератора случайных чисел (Mersenne Twister) по его константам в коде игры.
// Для каждой найденной константы печатаем адрес, начало функции (после блока int 3)
// и несколько мест, откуда эта функция вызывается.
static void FindRandomGenerator() {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);

    // Ищем только в секциях с кодом
    struct Range { uint8_t* begin; uint8_t* end; };
    std::vector<Range> code;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            code.push_back({ base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize });
    }

    struct Pattern { uint32_t value; const char* meaning; };
    const Pattern patterns[] = {
        { 0x6C078965, "MT seeding multiplier (SRand)" },
        { 0x9908B0DF, "MT matrix A (state regeneration)" },
        { 0x9D2C5680, "MT tempering mask B (Next)" },
        { 0xEFC60000, "MT tempering mask C (Next)" },
    };

    printf("[rng] scanning game code for Mersenne Twister constants...\n");
    for (const auto& pat : patterns) {
        int hits = 0;
        for (const auto& r : code) {
            for (uint8_t* p = r.begin; p + 4 <= r.end; ++p) {
                uint32_t v;
                memcpy(&v, p, 4);
                if (v != pat.value) continue;

                // Начало функции: идём назад до блока из CC (int 3)
                uint8_t* fn = p;
                for (int back = 0; back < 0x400 && fn > r.begin; ++back, --fn) {
                    if (fn[-1] == 0xCC && fn[-2] == 0xCC) break;
                }
                printf("[rng] %08X: %s  -> function starts at %08X\n",
                    (unsigned)(uintptr_t)p, pat.meaning, (unsigned)(uintptr_t)fn);

                // Кто вызывает эту функцию (call rel32 = E8 xx xx xx xx)
                int callers = 0;
                for (const auto& r2 : code) {
                    for (uint8_t* c = r2.begin; c + 5 <= r2.end; ++c) {
                        if (*c != 0xE8) continue;
                        int32_t rel;
                        memcpy(&rel, c + 1, 4);
                        if (c + 5 + rel != fn) continue;
                        if (callers < 6)
                            printf("[rng]      called from %08X\n", (unsigned)(uintptr_t)c);
                        ++callers;
                    }
                }
                printf("[rng]      total callers: %d\n", callers);
                if (++hits >= 4) break;
            }
            if (hits >= 4) break;
        }
        if (!hits) printf("[rng] %08X (%s): not found\n", pat.value, pat.meaning);
    }
    printf("[rng] done\n");
}

// --- Поиск объекта глобального генератора (gMTRand) ---
//
// MTRand: unsigned long mt[624]; int mti;  -> mti лежит по смещению 624*4 = 0x9C0.
// Обёртки Next()/Next(range)/Next(float) вызываются как thiscall: перед вызовом в ECX
// кладут адрес генератора, часто прямо константой: B9 xx xx xx xx (mov ecx, imm32).
// Собираем такие константы перед вызовами обёрток и считаем, какая встречается чаще.

constexpr uintptr_t MTRand_Mti = 0x9C0;

static uint8_t* FuncStart(uint8_t* p, uint8_t* lowest) {
    for (int back = 0; back < 0x400 && p > lowest + 2; ++back, --p)
        if (p[-1] == 0xCC && p[-2] == 0xCC) return p;
    return p;
}

static void FindRngInstance() {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uint8_t* imageEnd = base + nt->OptionalHeader.SizeOfImage;

    struct Range { uint8_t* begin; uint8_t* end; };
    std::vector<Range> code;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            code.push_back({ base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize });
    if (code.empty()) return;

    // 1) функция NextNoAssert — та, где маска 0x9D2C5680
    uint8_t* nextFn = nullptr;
    for (const auto& r : code) {
        for (uint8_t* p = r.begin; p + 4 <= r.end && !nextFn; ++p) {
            uint32_t v; memcpy(&v, p, 4);
            if (v == 0x9D2C5680) nextFn = FuncStart(p, r.begin);
        }
    }
    if (!nextFn) { printf("[rng] NextNoAssert not found\n"); return; }

    // 2) обёртки = функции, которые вызывают NextNoAssert
    auto findCallSites = [&](uint8_t* target, std::vector<uint8_t*>& out) {
        for (const auto& r : code) {
            for (uint8_t* c = r.begin; c + 5 <= r.end; ++c) {
                if (*c != 0xE8 && *c != 0xE9) continue; // call / jmp rel32
                int32_t rel; memcpy(&rel, c + 1, 4);
                if (c + 5 + rel == target) out.push_back(c);
            }
        }
        };
    std::vector<uint8_t*> sites;
    findCallSites(nextFn, sites);
    std::vector<uint8_t*> wrappers;
    for (uint8_t* s : sites) {
        uint8_t* w = FuncStart(s, code[0].begin);
        if (std::find(wrappers.begin(), wrappers.end(), w) == wrappers.end()) wrappers.push_back(w);
    }
    wrappers.push_back(nextFn); // и сам NextNoAssert тоже могут вызывать напрямую

    // 3) константы mov ecx, imm32 перед вызовами обёрток
    std::map<uintptr_t, int> votes;
    int totalCalls = 0;
    for (uint8_t* w : wrappers) {
        std::vector<uint8_t*> calls;
        findCallSites(w, calls);
        totalCalls += (int)calls.size();
        printf("[rng] wrapper %08X: %u call sites\n", (unsigned)(uintptr_t)w, (unsigned)calls.size());
        for (uint8_t* c : calls) {
            for (int back = 5; back <= 24; ++back) {
                uint8_t* m = c - back;
                if (*m != 0xB9) continue;
                uint32_t imm; memcpy(&imm, m + 1, 4);
                if (imm >= (uintptr_t)base && imm < (uintptr_t)imageEnd) { votes[imm]++; break; }
            }
        }
    }

    // 4) кандидаты по убыванию, с проверкой mti (должен быть 0..624)
    std::vector<std::pair<int, uintptr_t>> sorted;
    for (auto& kv : votes) sorted.push_back({ kv.second, kv.first });
    std::sort(sorted.rbegin(), sorted.rend());
    printf("[rng] %d calls total. Candidates for the global generator:\n", totalCalls);
    for (size_t i = 0; i < sorted.size() && i < 6; ++i) {
        uintptr_t a = sorted[i].second;
        int mti = -1; uint32_t mt0 = 0;
        SafeRead(a + MTRand_Mti, mti);
        SafeRead(a, mt0);
        printf("[rng]   %08X  used %3d times   mti=%d %s  mt[0]=%08X\n",
            (unsigned)a, sorted[i].first, mti,
            (mti >= 0 && mti <= 624) ? "(OK)" : "(not MTRand?)", mt0);
    }
    if (sorted.empty())
        printf("[rng]   no 'mov ecx, imm32' found before calls — generator is passed differently\n");
}

// ===========================================================================
// Исследование экрана выбора растений (F7 — найти и показать, F8 — что изменилось)
//
// 1) Ищем объект экрана: среди указателей в LawnApp и Board ищем объект, внутри которого
//    подряд с одинаковым шагом лежат числа 0, 1, 2, ..., 7 — это номера растений в массиве
//    «растение в списке» (ChosenSeed). Печатаем, где нашли, шаг (размер структуры)
//    и смещение поля «номер растения».
// 2) Обработчик клика: у Board в таблице виртуальных функций есть MouseDown (0x415430).
//    Берём тот же номер в таблице экрана выбора.
// 3) Печатаем первые 12 записей массива целиком (по 4 байта), F8 — только изменившиеся числа.
// ===========================================================================

namespace chooser {
    static uintptr_t g_obj = 0;          // объект экрана выбора
    static uintptr_t g_arrayOff = 0;     // смещение начала массива внутри объекта
    static uintptr_t g_stride = 0;       // размер одной записи
    static uintptr_t g_typeOff = 0;      // смещение поля «номер растения» внутри записи
    static std::vector<int> g_prev;      // прошлый снимок для F8

    constexpr int kSeedsToShow = 12;

    // Ищем в объекте obj последовательность 0..7 с постоянным шагом
    static bool FindSeedArray(uintptr_t obj, uintptr_t& arrayOff, uintptr_t& stride, uintptr_t& typeOff) {
        for (uintptr_t st = 0x20; st <= 0x80; st += 4) {
            for (uintptr_t o = 0; o + st * 8 < 0x3000; o += 4) {
                bool ok = true;
                for (int k = 0; k < 8 && ok; ++k) {
                    int v = -1;
                    if (!SafeRead(obj + o + k * st, v) || v != k) ok = false;
                }
                if (!ok) continue;
                // Отсекаем случайные совпадения: следующее значение тоже должно быть 8
                int v8 = -1;
                if (!SafeRead(obj + o + 8 * st, v8) || v8 != 8) continue;
                // o — это адрес поля «номер растения» записи 0. Начало записи неизвестно:
                // считаем, что массив начинается в первой записи, а поле лежит где-то внутри.
                typeOff = 0;
                arrayOff = o;
                stride = st;
                return true;
            }
        }
        return false;
    }

    static int VtableSlotOf(uintptr_t obj, uintptr_t fn) {
        uintptr_t vt = 0;
        if (!SafeRead(obj, vt) || !vt) return -1;
        for (int i = 0; i < 200; ++i) {
            uintptr_t f = 0;
            if (!SafeRead(vt + i * 4, f)) return -1;
            if (f == fn) return i;
        }
        return -1;
    }

    static void Find() {
        uintptr_t app = GetApp(), board = GetBoard();
        if (!app) { printf("[chooser] no LawnApp\n"); return; }
        g_obj = 0;
        struct Owner { const char* name; uintptr_t base; };
        Owner owners[] = { { "app", app },{ "board", board } };
        for (const auto& ow : owners) {
            if (!ow.base) continue;
            for (uintptr_t off = 0; off < 0x1000 && !g_obj; off += 4) {
                uintptr_t p = 0;
                if (!SafeRead(ow.base + off, p) || p < 0x10000 || (p & 3) || p == app || p == board) continue;
                // Объект экрана большой: проверяем, что вся область читается (иначе поиск будет медленным)
                int probeV;
                if (!SafeRead(p, probeV) || !SafeRead(p + 0x1800, probeV) || !SafeRead(p + 0x2FFC, probeV)) continue;
                uintptr_t ao, st, to;
                if (FindSeedArray(p, ao, st, to)) {
                    g_obj = p; g_arrayOff = ao; g_stride = st; g_typeOff = to;
                    printf("[chooser] found: %s+%03X -> %08X, seed numbers at +%X, stride 0x%X\n",
                        ow.name, (unsigned)off, (unsigned)p, (unsigned)ao, (unsigned)st);
                }
            }
        }
        if (!g_obj) { printf("[chooser] not found (is the seed selection screen open?)\n"); return; }

        // Обработчик клика по номеру в таблице виртуальных функций
        if (board) {
            int slot = VtableSlotOf(board, Fn_BoardMouseDown);
            uintptr_t vt = 0, fn = 0;
            SafeRead(g_obj, vt);
            if (slot >= 0 && vt) SafeRead(vt + slot * 4, fn);
            printf("[chooser] Board::MouseDown is vtable slot %d; chooser vtable=%08X, slot %d -> %08X\n",
                slot, (unsigned)vt, slot, (unsigned)fn);
        }
        g_prev.clear();
    }

    // Печать записей. Каждая запись выводится от «поля номера растения» назад на 8 int и вперёд
    // до конца шага — так будут видны поля и до, и после номера растения.
    static void Dump(bool onlyChanges) {
        if (!g_obj) { Find(); if (!g_obj) return; onlyChanges = false; }
        const int back = 8;                         // int'ов до поля номера растения
        const int perSeed = (int)(g_stride / 4);    // int'ов на запись
        std::vector<int> cur;
        for (int k = 0; k < kSeedsToShow; ++k)
            for (int i = -back; i < perSeed - back; ++i) {
                int v = 0;
                SafeRead(g_obj + g_arrayOff + k * g_stride + i * 4, v);
                cur.push_back(v);
            }

        if (!onlyChanges || g_prev.size() != cur.size()) {
            printf("[chooser] fields relative to 'seed number' field (+0). One line per seed:\n");
            printf("[chooser]      ");
            for (int i = -back; i < perSeed - back; ++i) printf("%+5X", i * 4);
            printf("\n");
            for (int k = 0; k < kSeedsToShow; ++k) {
                printf("[chooser] #%2d ", k);
                for (int i = 0; i < perSeed; ++i) printf("%5d", cur[k * perSeed + i]);
                printf("\n");
            }
        }
        else {
            int n = 0;
            for (int k = 0; k < kSeedsToShow; ++k)
                for (int i = 0; i < perSeed; ++i) {
                    int idx = k * perSeed + i;
                    if (cur[idx] != g_prev[idx]) {
                        printf("[chooser] seed #%d field %+X: %d -> %d\n", k, (i - back) * 4, g_prev[idx], cur[idx]);
                        ++n;
                    }
                }
            printf("[chooser] %d fields changed\n", n);
            // Ищем и счётчик «растений в панели»: небольшие int'ы в самом объекте, которые изменились
        }
        g_prev = cur;
    }

    // Снимок самого объекта (вне массива) — чтобы найти счётчик выбранных растений
    static std::vector<int> g_objPrev;
    static void DiffObject() {
        if (!g_obj) return;
        std::vector<int> cur(0x3000 / 4);
        for (size_t i = 0; i < cur.size(); ++i) SafeRead(g_obj + i * 4, cur[i]);
        if (g_objPrev.size() == cur.size()) {
            uintptr_t arrBegin = g_arrayOff - 8 * 4, arrEnd = g_arrayOff - 8 * 4 + 49 * g_stride; // 49 растений в списке
            int n = 0;
            for (size_t i = 0; i < cur.size(); ++i) {
                uintptr_t off = i * 4;
                if (off >= arrBegin && off < arrEnd) continue; // массив смотрим отдельно
                if (cur[i] != g_objPrev[i] && cur[i] >= -1 && cur[i] <= 20 && g_objPrev[i] >= -1 && g_objPrev[i] <= 20) {
                    printf("[chooser] object +%03X: %d -> %d\n", (unsigned)off, g_objPrev[i], cur[i]);
                    ++n;
                }
            }
            printf("[chooser] %d small object fields changed\n", n);
        }
        g_objPrev = cur;
    }
}

// F11: статус сети
// F10: сохранить образ игры из памяти в game_dump.bin (для поиска функций рисования)
static void DumpGameImage() {
    HMODULE mod = GetModuleHandleW(nullptr);
    uintptr_t base = (uintptr_t)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    std::vector<uint8_t> buf(size, 0);
    DWORD copied = 0;
    for (DWORD off = 0; off < size; off += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)(base + off), &mbi, sizeof(mbi))) continue;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        DWORD n = (size - off < 0x1000) ? size - off : 0x1000;
        memcpy(&buf[off], (void*)(base + off), n);
        copied += n;
    }
    char path[MAX_PATH];
    strcpy_s(path, g_cfg.dir);
    strcat_s(path, "game_dump.bin");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        fwrite(buf.data(), 1, buf.size(), f);
        fclose(f);
        printf("[dump] base=%08X size=%u copied=%u -> %s\n", (unsigned)base, (unsigned)size, (unsigned)copied, path);
    }
    else {
        printf("[dump] cannot write %s\n", path);
    }
}

static void PrintNetStatus() {
    size_t remote = 0;
    {
        std::lock_guard<std::mutex> lk(net::g_rxMx);
        remote = net::g_remoteTicks.size();
    }
    int mti = -1;
    SafeRead(rng::GlobalMTRand + rng::Mti, mti);
    printf("[net] mode=%s connected=%s seed=%s%08X lockstep=%s level=%u tick=%u buffered=%u rng_mti=%d\n",
        g_cfg.mode == NetMode::Host ? "host" : g_cfg.mode == NetMode::Client ? "client" : "off",
        net::g_connected ? "yes" : "no",
        session::g_haveSeed ? "" : "(none) ", (unsigned)session::g_seed,
        g_lockstepActive ? "ON" : "off", ls::g_epoch, ls::g_tick, (unsigned)remote, mti);
}

// ===========================================================================
// Главный поток мода (клавиатура, консоль)
// ===========================================================================

static int g_selCol = 4, g_selRow = 2; // клетка своего игрока

static void OnPlantKey(int slot) {
    if (g_cfg.test == Config::Test::Replay && replay::g_active) {
        printf("[test] replay is running - keys are ignored\n");
        return;
    }
    if (g_cfg.test == Config::Test::Record && replay::g_active) {
        replay::Queue(slot, g_selCol, g_selRow);
        return;
    }
    if (g_lockstepActive) {
        net::Cmd c;
        c.kind = net::CMD_PLANT; c.player = (uint8_t)LocalPlayer(); c.slot = (uint8_t)slot;
        c.col = (uint8_t)g_selCol; c.row = (uint8_t)g_selRow;
        ls::QueueCommand(c);
        printf("[p%d] queued: slot %d -> col=%d row=%d\n", LocalPlayer(), slot + 1, g_selCol, g_selRow);
    }
    else {
        std::lock_guard<std::mutex> lk(g_soloMx);
        g_soloRequests.push_back({ LocalPlayer(), slot, g_selCol, g_selRow });
    }
}

static DWORD WINAPI MainThread(LPVOID) {
    FILE* f = nullptr;
    SetupConsole(f);
    printf("[coop] DLL loaded\n");
    PrintGameInfo();
    LoadConfig();

    for (int i = 0; i < 100 && !GetApp(); ++i) Sleep(100);
    printf(GetApp() ? "[coop] LawnApp found\n" : "[coop] LawnApp NOT found\n");

    timeBeginPeriod(1); // точный Sleep(1) для ожидания в lockstep
    bool hooksOk = InstallHooks();
    net::Start();

    while (g_running) {
        uintptr_t board = GetBoard();

        // Ушли с уровня (главное меню, экран награды...) — синхронная игра закончилась
        if (!board) session::g_noBoardSeen = true;
        if (!board && g_lockstepActive) ls::End("left the level", true);

        const bool f1 = (GetAsyncKeyState(VK_F1) & 1) != 0;
        if (f1 && g_cfg.test != Config::Test::Off && replay::g_active) {
            if (g_cfg.test == Config::Test::Record) replay::Queue(-1, 0, 0); // +100 солнца — в запись
            else printf("[test] replay is running - keys are ignored\n");
        }
        else if (f1 && g_lockstepActive) {
            printf("[coop] F1 (+100 sun) is disabled during co-op play - it would desync the games\n");
        }
        else if (f1) {
            int sun = 0;
            if (board && SafeRead(board + addr::Board_Sun, sun)) {
                SafeWrite(board + addr::Board_Sun, sun + 100);
                printf("[coop] +100 sun%s\n", g_lockstepActive ? " (WARNING: desync!)" : "");
            }
        }
        if (GetAsyncKeyState(VK_F2) & 1) FindTickCounters();

        bool moved = false;
        if ((GetAsyncKeyState(VK_LEFT) & 1) && g_selCol > 0) { g_selCol--; moved = true; }
        if ((GetAsyncKeyState(VK_RIGHT) & 1) && g_selCol < lawn::Cols - 1) { g_selCol++; moved = true; }
        if ((GetAsyncKeyState(VK_UP) & 1) && g_selRow > 0) { g_selRow--; moved = true; }
        if ((GetAsyncKeyState(VK_DOWN) & 1) && g_selRow < lawn::Rows - 1) { g_selRow++; moved = true; }
        if (moved) printf("[p%d] cell col=%d row=%d\n", LocalPlayer(), g_selCol, g_selRow);

        if (GetAsyncKeyState(VK_F3) & 1) OnPlantKey(0);
        if (GetAsyncKeyState(VK_F4) & 1) OnPlantKey(1);
        if (GetAsyncKeyState(VK_F5) & 1) DumpCursor();
        if (GetAsyncKeyState(VK_F6) & 1) PrintPackets();
        if (GetAsyncKeyState(VK_F7) & 1) { chooser::Find(); chooser::Dump(false); chooser::DiffObject(); }
        if (GetAsyncKeyState(VK_F8) & 1) { chooser::Dump(true); chooser::DiffObject(); }
        if (GetAsyncKeyState(VK_F9) & 1) { FindRandomGenerator(); FindRngInstance(); }
        if (GetAsyncKeyState(VK_F10) & 1) DumpGameImage();
        if (GetAsyncKeyState(VK_F11) & 1) PrintNetStatus();
        if (GetAsyncKeyState(VK_F12) & 1) g_running = false;

        Sleep(10);
    }

    printf("[coop] unloading\n");
    if (replay::g_file) { fclose(replay::g_file); replay::g_file = nullptr; }
    Sleep(50);            // поток игры выходит из ожидания lockstep (он проверяет g_running)
    net::Stop();
    if (hooksOk) RemoveHooks();
    Sleep(100);           // даём игре выйти из наших функций
    timeEndPeriod(1);
    if (g_logFile) { FILE* lf = g_logFile; g_logFile = nullptr; fclose(lf); }
    if (f) fclose(f);
    FreeConsole();
    FreeLibraryAndExitThread(g_self, 0);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
