# -*- coding: utf-8 -*-
"""
make_release.py - готовит файлы для нового релиза на GitHub.

Запуск (из папки tools):
    python make_release.py 1.1            - версия 1.1, картинка панели включается, если есть
    python make_release.py 1.1 --no-png   - без coop_seedbank.png

Что делает:
  1. Берёт из ..\\Release файлы Coop.dll, PvZCoopLauncher.exe и coop_seedbank.png.
  2. Кладёт их копии в ..\\release_upload\\v1.1\\ и рядом пишет manifest.txt
     (версия, тег и SHA-256 каждого файла).
  3. Пишет ..\\Release\\version.txt = 1.1, чтобы твой собственный лаунчер
     не предлагал обновиться до версии, которую ты сам и собрал.

Потом на GitHub: Releases -> Draft a new release -> тег v1.1 ->
перетащи ВСЕ файлы из release_upload\\v1.1 (включая manifest.txt) -> Publish release.
"""
import hashlib
import os
import re
import shutil
import sys

FILES = ["Coop.dll", "PvZCoopLauncher.exe", "coop_seedbank.png"]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    no_png = "--no-png" in sys.argv
    if len(args) != 1 or not re.fullmatch(r"\d+(\.\d+)*", args[0]):
        print(__doc__)
        sys.exit(1)
    version = args[0]
    tag = "v" + version

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    rel = os.path.join(root, "Release")
    out = os.path.join(root, "release_upload", tag)
    os.makedirs(out, exist_ok=True)

    lines = ["# PvZ Co-op release manifest", "version=" + version, "tag=" + tag]
    for name in FILES:
        if name == "coop_seedbank.png" and no_png:
            continue
        src = os.path.join(rel, name)
        if not os.path.isfile(src):
            if name == "coop_seedbank.png":
                print("(нет %s - пропускаю)" % name)
                continue
            sys.exit("Не найден %s - сначала собери проект (Release | x86)." % src)
        with open(src, "rb") as f:
            data = f.read()
        sha = hashlib.sha256(data).hexdigest()
        shutil.copy2(src, os.path.join(out, name))
        lines.append("%s %s" % (name, sha))
        print("%-22s %8d байт  %s" % (name, len(data), sha))

    with open(os.path.join(out, "manifest.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(rel, "version.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(version + "\n")

    print("\nГотово: %s" % out)
    print("Создай на GitHub релиз с тегом %s и загрузи туда все файлы из этой папки." % tag)


if __name__ == "__main__":
    main()
