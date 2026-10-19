#!/usr/bin/env bash
# Run as the desktop user after installing the Arch package.
set -euo pipefail
if [[ $(id -u) == 0 ]]; then
  printf '请以当前桌面用户运行 forma-screenshot-setup，不要使用 sudo。\n' >&2
  exit 1
fi
forma_binary=/usr/bin/forma-screenshot
forma_shortcut=/usr/bin/forma-screenshot-shortcut
forma_desktop=io.github.skjsbsnq.Forma.Screenshot.desktop
if [[ ! -x "$forma_binary" || ! -f "/usr/share/applications/$forma_desktop" ]]; then
  printf '请先通过 pacman 安装 forma-screenshot。\n' >&2
  exit 1
fi
forma_data="${XDG_DATA_HOME:-$HOME/.local/share}"
forma_config="${XDG_CONFIG_HOME:-$HOME/.config}"
forma_state="${XDG_STATE_HOME:-$HOME/.local/state}/forma-screenshot"
forma_backup="$forma_state/legacy-install-$(date -u +%Y%m%dT%H%M%S)-$$"

# End the old user's resident before moving its launcher/executable out of PATH.
if [[ -x "$HOME/.local/bin/forma-screenshot" ]]; then
  "$HOME/.local/bin/forma-screenshot" --quit >/dev/null 2>&1 || true
fi
"$forma_binary" --quit >/dev/null 2>&1 || true
systemctl --user stop forma-screenshot.service >/dev/null 2>&1 || true
for forma_attempt in {1..40}; do
  [[ ! -S "${XDG_RUNTIME_DIR:-/tmp}/forma-capture-$(id -u).sock" ]] && break
  sleep 0.05
 done

# Keep cached launcher paths alive: KDE 6.8 retains KService objects even after
# rebuilding its desktop cache. Back up known files, then forward them to /usr.
for forma_old in \
  "$HOME/.local/bin/forma-screenshot" \
  "$HOME/.local/bin/forma-screenshot-shortcut" \
  "$forma_data/applications/$forma_desktop" \
  "$forma_data/icons/hicolor/scalable/apps/io.github.skjsbsnq.Forma.Screenshot.svg" \
  "$forma_config/autostart/$forma_desktop"; do
  if [[ -f "$forma_old" || -L "$forma_old" ]]; then
    forma_relative="${forma_old#/}"
    mkdir -p "$forma_backup/$(dirname "$forma_relative")"
    cp -a -- "$forma_old" "$forma_backup/$forma_relative"
  fi
 done

python3 - <<'PY'
import os
from pathlib import Path
from gi.repository import GLib

home = Path.home()
data = Path(os.environ.get('XDG_DATA_HOME', str(home / '.local/share')))
config = Path(os.environ.get('XDG_CONFIG_HOME', str(home / '.config')))
identity = 'io.github.skjsbsnq.Forma.Screenshot.desktop'
desktop = data / 'applications' / identity
desktop.parent.mkdir(parents=True, exist_ok=True)
keys = GLib.KeyFile.new()
keys.load_from_file('/usr/share/applications/' + identity, GLib.KeyFileFlags.KEEP_TRANSLATIONS)
for group in keys.get_groups()[0]:
    if 'Exec' in keys.get_keys(group)[0]:
        command = keys.get_string(group, 'Exec')
        program, separator, arguments = command.partition(' ')
        if program == 'forma-screenshot':
            keys.set_string(group, 'Exec', '/usr/bin/forma-screenshot' + separator + arguments)
keys.set_string('Desktop Entry', 'TryExec', '/usr/bin/forma-screenshot')
desktop.write_text(keys.to_data()[0], encoding='utf-8')

# Existing cached Exec values may still name ~/.local/bin. These aliases always
# resolve to pacman's application, including after later package upgrades.
aliases = {
    home / '.local/bin/forma-screenshot': Path('/usr/bin/forma-screenshot'),
    home / '.local/bin/forma-screenshot-shortcut': Path('/usr/bin/forma-screenshot-shortcut'),
    data / 'icons/hicolor/scalable/apps/io.github.skjsbsnq.Forma.Screenshot.svg':
        Path('/usr/share/icons/hicolor/scalable/apps/io.github.skjsbsnq.Forma.Screenshot.svg'),
}
for alias, target in aliases.items():
    alias.parent.mkdir(parents=True, exist_ok=True)
    temporary = alias.with_name(alias.name + '.forma-new')
    temporary.unlink(missing_ok=True)
    temporary.symlink_to(target)
    temporary.replace(alias)
startup = config / 'autostart' / identity
if startup.exists():
    startup.write_text(Path('/etc/xdg/autostart/' + identity).read_text(encoding='utf-8'), encoding='utf-8')
PY

"$forma_shortcut" --enable
if command -v systemd-run >/dev/null && systemctl --user show-environment >/dev/null 2>&1; then
  systemd-run --user --collect --unit=forma-screenshot --description='Forma 截图预热' \
    --property=PartOf=graphical-session.target "$forma_binary" --background
else
  mkdir -p "$forma_state"
  nohup "$forma_binary" --background >"$forma_state/resident.log" 2>&1 </dev/null &
fi
printf '\n已切换到 Arch 安装版，按 Super+Shift+S 截图。\n'
if [[ -d "$forma_backup" ]]; then printf '旧原型文件备份：%s\n' "$forma_backup"; fi
