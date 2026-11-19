# -*- coding: utf-8 -*-
"""
paktool.py - распаковка и упаковка main.pak из Plants vs. Zombies (движок PopCap).

Формат main.pak:
  весь файл закодирован XOR 0xF7;
  заголовок: uint32 0xBAC04AC0, uint32 версия (0);
  таблица файлов: [uint8 флаг][uint8 длина имени][имя][uint32 размер][uint64 время],
  флаг 0x80 - конец таблицы;
  дальше подряд лежат данные файлов в том же порядке.

Команды:
  python paktool.py list   [main.pak]               - список файлов
  python paktool.py find   ТЕКСТ [main.pak]         - найти файлы по части имени
  python paktool.py unpack [main.pak] [папка]       - распаковать всё (по умолчанию в main_unpacked)
  python paktool.py pack   ПАПКА новый.pak          - упаковать папку обратно в .pak

Если main.pak не указан, скрипт ищет его в библиотеках Steam сам.
Исходный main.pak скрипт никогда не изменяет.
"""
import os
import struct
import sys

MAGIC = 0xBAC04AC0
XOR_TABLE = bytes(b ^ 0xF7 for b in range(256))
FLAG_END = 0x80
GAME_DIR_NAMES = ("Plants Vs Zombies", "Plants vs. Zombies", "Plants vs Zombies")


def xor(data):
    return data.translate(XOR_TABLE)


# --------------------------------------------------------------------------- поиск main.pak

def steam_libraries():
    roots = []
    try:
        import winreg
        for hive, key, val in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                               (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                               (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    roots.append(winreg.QueryValueEx(k, val)[0])
            except OSError:
                pass
    except ImportError:
        pass
    roots += [r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"]

    libs = []
    for root in roots:
        root = os.path.normpath(root)
        if root not in libs and os.path.isdir(root):
            libs.append(root)
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            with open(vdf, encoding="utf-8", errors="ignore") as f:
                for line in f:
                    parts = line.strip().split('"')
                    if len(parts) >= 4 and parts[1] == "path":
                        p = os.path.normpath(parts[3].replace("\\\\", "\\"))
                        if p not in libs and os.path.isdir(p):
                            libs.append(p)
    return libs


def find_main_pak():
    for lib in steam_libraries():
        for name in GAME_DIR_NAMES:
            p = os.path.join(lib, "steamapps", "common", name, "main.pak")
            if os.path.isfile(p):
                return p
    return None


def resolve_pak(arg):
    if arg:
        if not os.path.isfile(arg):
            sys.exit("Файл не найден: %s" % arg)
        return arg
    p = find_main_pak()
    if not p:
        sys.exit("main.pak не найден автоматически. Укажи путь, например:\n"
                 '  python paktool.py unpack "D:\\SteamLibrary\\steamapps\\common\\Plants Vs Zombies\\main.pak"')
    print("main.pak:", p)
    return p


# --------------------------------------------------------------------------- чтение

def read_pak(path):
    """Возвращает (entries, data), entries = [(имя, смещение, размер, время)]."""
    with open(path, "rb") as f:
        data = xor(f.read())
    if len(data) < 8:
        sys.exit("Файл слишком маленький для .pak")
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != MAGIC:
        sys.exit("Это не PopCap .pak (заголовок %08X, ожидался %08X)" % (magic, MAGIC))
    if version != 0:
        print("Внимание: версия архива %d (обычно 0), пробую читать дальше" % version)

    pos = 8
    table = []
    while True:
        if pos >= len(data):
            sys.exit("Таблица файлов оборвалась - архив повреждён?")
        flags = data[pos]; pos += 1
        if flags & FLAG_END:
            break
        n = data[pos]; pos += 1
        name = data[pos:pos + n].decode("latin-1"); pos += n
        size, ftime = struct.unpack_from("<IQ", data, pos); pos += 12
        table.append((name, size, ftime))

    entries = []
    offset = pos
    for name, size, ftime in table:
        if offset + size > len(data):
            sys.exit("Данные файла %s выходят за конец архива" % name)
        entries.append((name, offset, size, ftime))
        offset += size
    if offset != len(data):
        print("Внимание: после данных осталось %d лишних байт" % (len(data) - offset))
    return entries, data


# --------------------------------------------------------------------------- команды

def cmd_list(pak, needle=None):
    entries, _ = read_pak(pak)
    total = 0
    shown = 0
    for name, _, size, _ in entries:
        total += size
        if needle and needle.lower() not in name.lower():
            continue
        print("%10d  %s" % (size, name))
        shown += 1
    print("-- файлов: %d из %d, всего %.1f МБ" % (shown, len(entries), total / 1048576))


def cmd_unpack(pak, out):
    entries, data = read_pak(pak)
    out = os.path.abspath(out)
    for i, (name, off, size, ftime) in enumerate(entries, 1):
        rel = name.replace("\\", os.sep).replace("/", os.sep).lstrip(os.sep)
        dst = os.path.normpath(os.path.join(out, rel))
        if not dst.startswith(out + os.sep):          # защита от "..\" в именах
            print("пропуск подозрительного имени:", name)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data[off:off + size])
        if ftime:                                       # FILETIME -> время файла
            ts = (ftime - 116444736000000000) / 10000000
            if ts > 0:
                try:
                    os.utime(dst, (ts, ts))
                except (OSError, OverflowError):
                    pass
        if i % 200 == 0:
            print("  %d / %d" % (i, len(entries)))
    print("Готово: %d файлов -> %s" % (len(entries), out))


def cmd_pack(src, out):
    src = os.path.abspath(src)
    if not os.path.isdir(src):
        sys.exit("Папка не найдена: %s" % src)
    if os.path.abspath(out).lower().endswith(os.sep + "main.pak") and os.path.exists(out):
        sys.exit("Не буду перезаписывать существующий main.pak. Укажи другое имя файла.")
    files = []
    for root, _, names in os.walk(src):
        for n in sorted(names):
            full = os.path.join(root, n)
            rel = os.path.relpath(full, src).replace(os.sep, "\\")
            files.append((rel, full))
    files.sort(key=lambda x: x[0].lower())

    head = bytearray(struct.pack("<II", MAGIC, 0))
    body = bytearray()
    for rel, full in files:
        name = rel.encode("latin-1")
        if len(name) > 255:
            sys.exit("Слишком длинное имя: %s" % rel)
        with open(full, "rb") as f:
            blob = f.read()
        ftime = int(os.path.getmtime(full) * 10000000) + 116444736000000000
        head += bytes([0, len(name)]) + name + struct.pack("<IQ", len(blob), ftime)
        body += blob
    head.append(FLAG_END)
    with open(out, "wb") as f:
        f.write(xor(bytes(head + body)))
    print("Готово: %d файлов -> %s" % (len(files), os.path.abspath(out)))


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help", "help"):
        print(__doc__)
        return
    cmd = argv[1]
    if cmd == "list":
        cmd_list(resolve_pak(argv[2] if len(argv) > 2 else None))
    elif cmd == "find":
        if len(argv) < 3:
            sys.exit("Укажи, что искать: python paktool.py find seedbank")
        cmd_list(resolve_pak(argv[3] if len(argv) > 3 else None), argv[2])
    elif cmd == "unpack":
        pak = resolve_pak(argv[2] if len(argv) > 2 else None)
        out = argv[3] if len(argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "main_unpacked")
        cmd_unpack(pak, out)
    elif cmd == "pack":
        if len(argv) < 4:
            sys.exit("Использование: python paktool.py pack ПАПКА новый.pak")
        cmd_pack(argv[2], argv[3])
    else:
        sys.exit("Неизвестная команда: %s (см. python paktool.py help)" % cmd)


if __name__ == "__main__":
    main(sys.argv)
