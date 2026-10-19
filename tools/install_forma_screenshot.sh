#!/usr/bin/env bash
# Install the prototype for the current user and migrate KDE's screenshot shortcut.
set -euo pipefail

forma_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
forma_build=${FORMA_BUILD_DIR:-"$forma_root/build"}
forma_prefix="$HOME/.local"

cmake -S "$forma_root" -B "$forma_build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$forma_build" --target forma-screenshot -j"${FORMA_BUILD_JOBS:-4}"
# A previous resident keeps its executable mapped; ask it to leave before replacing it.
if [[ -x "$forma_prefix/bin/forma-screenshot" ]]; then
  "$forma_prefix/bin/forma-screenshot" --quit >/dev/null 2>&1 || true
  for forma_attempt in {1..40}; do
    [[ ! -S "${XDG_RUNTIME_DIR:-/tmp}/forma-capture-$(id -u).sock" ]] && break
    sleep 0.05
  done
fi
cmake --install "$forma_build" --prefix "$forma_prefix" --component forma-screenshot

# KDE's session PATH may exclude ~/.local/bin. Use an absolute executable in
# every installed desktop action so both shortcuts and menu actions can launch.
python3 - "$forma_prefix" <<'PY'
from pathlib import Path
import sys
from gi.repository import GLib

prefix = Path(sys.argv[1])
desktop = prefix / "share/applications/io.github.skjsbsnq.Forma.Screenshot.desktop"
keyfile = GLib.KeyFile.new()
keyfile.load_from_file(str(desktop), GLib.KeyFileFlags.KEEP_TRANSLATIONS)
binary = str(prefix / "bin/forma-screenshot")
quoted_binary = '"' + binary.replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"'
for group in keyfile.get_groups()[0]:
    if "Exec" in keyfile.get_keys(group)[0]:
        command = keyfile.get_string(group, "Exec")
        executable, separator, arguments = command.partition(" ")
        if executable == "forma-screenshot":
            keyfile.set_string(group, "Exec", quoted_binary + (separator + arguments if separator else ""))
keyfile.set_string("Desktop Entry", "TryExec", binary)
desktop.write_text(keyfile.to_data()[0], encoding="utf-8")

# Prewarm the Forma renderer at login so a hotkey only needs a fresh image.
config_home = Path(__import__('os').environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config')))
autostart = config_home / 'autostart/io.github.skjsbsnq.Forma.Screenshot.desktop'
autostart.parent.mkdir(parents=True, exist_ok=True)
startup = GLib.KeyFile.new()
startup.set_string('Desktop Entry', 'Type', 'Application')
startup.set_string('Desktop Entry', 'Name', 'Forma 截图')
startup.set_string('Desktop Entry', 'Exec', quoted_binary + ' --background')
startup.set_string('Desktop Entry', 'Icon', 'io.github.skjsbsnq.Forma.Screenshot')
startup.set_boolean('Desktop Entry', 'StartupNotify', False)
startup.set_string('Desktop Entry', 'OnlyShowIn', 'KDE;')
autostart.write_text(startup.to_data()[0], encoding='utf-8')
PY

"$forma_prefix/bin/forma-screenshot-shortcut" --enable
forma_state="${XDG_STATE_HOME:-$HOME/.local/state}/forma-screenshot"
mkdir -p "$forma_state"
if command -v systemd-run >/dev/null && systemctl --user show-environment >/dev/null 2>&1; then
  systemctl --user stop forma-screenshot.service >/dev/null 2>&1 || true
  systemd-run --user --collect --unit=forma-screenshot \
    --description='Forma 截图预热' --property=PartOf=graphical-session.target \
    "$forma_prefix/bin/forma-screenshot" --background
else
  nohup "$forma_prefix/bin/forma-screenshot" --background >"$forma_state/resident.log" 2>&1 </dev/null &
fi
printf '\nForma 截图已安装：%s\n' "$forma_prefix/bin/forma-screenshot"
printf '按 Super+Shift+S 截图，按 1/2/3 切换区域/全屏/窗口。\n'
