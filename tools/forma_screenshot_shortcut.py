#!/usr/bin/env python3
"""Move KDE's Meta+Shift+S binding to Forma, preserving a restorable snapshot."""

import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys

import gi

gi.require_version("GioUnix", "2.0")
from gi.repository import Gio, GioUnix, GLib


DESKTOP_ID = "io.github.skjsbsnq.Forma.Screenshot.desktop"
ACTION = [DESKTOP_ID, "_launch", "Forma 截图", "截图"]
META_SHIFT_S = 0x10000000 | 0x02000000 | ord("S")
BUS_NAME = "org.kde.kglobalaccel"
BUS_PATH = "/kglobalaccel"
BUS_INTERFACE = "org.kde.KGlobalAccel"
STATE_HOME = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state")))
BACKUP = STATE_HOME / "forma-screenshot" / "shortcut-backup.json"


class Shortcuts:
    def __init__(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(self, method, signature, args):
        result = self.bus.call_sync(
            BUS_NAME, BUS_PATH, BUS_INTERFACE, method,
            GLib.Variant(signature, args), None, Gio.DBusCallFlags.NONE, 5000, None,
        ).unpack()
        return result[0] if result else None

    def owners(self):
        infos = self.call("getGlobalShortcutsByKey", "(i)", (META_SHIFT_S,))
        return [
            {"action": [info[2], info[0], info[3], info[1]], "keys": list(info[6])}
            for info in infos
        ]

    def keys(self, action):
        return self.call("shortcut", "(as)", (action,))

    def set(self, action, keys):
        # SetPresent | NoAutoloading: update the live accelerator and persist it.
        actual = self.call("setShortcut", "(asaiu)", (action, keys, 6))
        if set(actual) != set(keys):
            raise RuntimeError(f"KDE 未接受快捷键设置：{action[0]} / {action[1]}")



def write_backup(snapshot):
    BACKUP.parent.mkdir(parents=True, exist_ok=True)
    # Never overwrite a snapshot needed to restore the previous binding.
    with BACKUP.open("x", encoding="utf-8") as file:
        json.dump(snapshot, file, ensure_ascii=False, indent=2)
        file.write("\n")
    BACKUP.chmod(0o600)


def enable(shortcuts):
    app = GioUnix.DesktopAppInfo.new(DESKTOP_ID)
    command = shlex.split(app.get_commandline()) if app is not None else []
    if not command or not shutil.which(command[0]):
        raise RuntimeError("请先安装 Forma 截图程序及其 desktop 文件。")
    cache_builder = shutil.which("kbuildsycoca6")
    if cache_builder:
        subprocess.run([cache_builder, "--noincremental"], check=True)


    owners = shortcuts.owners()
    unrelated = [owner for owner in owners if owner["action"][0] not in
                 (DESKTOP_ID, "org.kde.spectacle.desktop")]
    if unrelated:
        raise RuntimeError("Meta+Shift+S 已被其他应用占用，未修改：" +
                           ", ".join(owner["action"][2] for owner in unrelated))
    if owners and all(owner["action"][:2] == ACTION[:2] for owner in owners):
        print("Meta+Shift+S 已绑定 Forma 截图。")
        return
    if BACKUP.exists():
        raise RuntimeError(f"已有迁移备份，请先执行 --restore：{BACKUP}")

    shortcuts.call("doRegister", "(as)", (ACTION,))
    snapshot = {
        "version": 1,
        "key": META_SHIFT_S,
        "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "target": {"action": ACTION, "keys": shortcuts.keys(ACTION)},
        "displaced": [owner for owner in owners if owner["action"][:2] != ACTION[:2]],
    }
    write_backup(snapshot)
    try:
        for owner in snapshot["displaced"]:
            shortcuts.set(owner["action"], [key for key in owner["keys"] if key != META_SHIFT_S])
        keys = list(snapshot["target"]["keys"])
        if META_SHIFT_S not in keys:
            keys.append(META_SHIFT_S)
        shortcuts.set(ACTION, keys)
        owners = shortcuts.owners()
        if len(owners) != 1 or owners[0]["action"][:2] != ACTION[:2]:
            raise RuntimeError("快捷键所有者验证失败。")
    except Exception:
        shortcuts.set(ACTION, snapshot["target"]["keys"])
        for owner in snapshot["displaced"]:
            shortcuts.set(owner["action"], owner["keys"])
        BACKUP.unlink()
        raise
    print("已将 Meta+Shift+S 绑定到 Forma 截图；保留其他快捷键。")
    print(f"恢复备份：{BACKUP}")


def restore(shortcuts):
    if not BACKUP.exists():
        raise RuntimeError(f"没有可恢复的迁移备份：{BACKUP}")
    snapshot = json.loads(BACKUP.read_text(encoding="utf-8"))
    if snapshot.get("version") != 1 or snapshot.get("key") != META_SHIFT_S:
        raise RuntimeError("不支持的快捷键备份格式。")
    original_owners = {tuple(owner["action"][:2]) for owner in snapshot["displaced"]}
    for owner in shortcuts.owners():
        if tuple(owner["action"][:2]) not in original_owners | {tuple(ACTION[:2])}:
            raise RuntimeError("快捷键已经被其他应用接管，保留当前设置。")

    # Preserve any unrelated shortcut edits made since installation.
    target_keys = [key for key in shortcuts.keys(ACTION) if key != META_SHIFT_S]
    for key in snapshot["target"]["keys"]:
        if key not in target_keys:
            target_keys.append(key)
    shortcuts.set(ACTION, target_keys)
    for owner in snapshot["displaced"]:
        keys = list(shortcuts.keys(owner["action"]))
        if META_SHIFT_S not in keys:
            keys.append(META_SHIFT_S)
        shortcuts.set(owner["action"], keys)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    archived = BACKUP.with_name(f"shortcut-backup.restored-{stamp}.json")
    BACKUP.rename(archived)
    print("已恢复截图快捷键的原始归属。")


def main():
    parser = argparse.ArgumentParser(description="迁移或恢复 KDE 的 Super+Shift+S 截图快捷键")
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--enable", action="store_true", help="将快捷键从 Spectacle 迁移到 Forma")
    modes.add_argument("--restore", action="store_true", help="恢复迁移前的快捷键归属")
    modes.add_argument("--status", action="store_true", help="查看当前快捷键归属")
    args = parser.parse_args()
    shortcuts = Shortcuts()
    if args.enable:
        enable(shortcuts)
    elif args.restore:
        restore(shortcuts)
    else:
        print(json.dumps({"shortcut": "Meta+Shift+S", "owners": shortcuts.owners(),
                          "backup": str(BACKUP) if BACKUP.exists() else None},
                         ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (GLib.Error, OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print(f"Forma 截图快捷键：{error}", file=sys.stderr)
        sys.exit(1)
