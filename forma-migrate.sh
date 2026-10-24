#!/usr/bin/env bash
# Forma 全家桶 <=> KDE 全家桶 迁移工具（Arch / CachyOS）
#
#   ./forma-migrate.sh status      查看现状与可替换关系（只读，推荐先跑）
#   ./forma-migrate.sh deps        安装构建依赖
#   ./forma-migrate.sh build       构建全部 Forma 安装包
#   ./forma-migrate.sh install     安装 Forma，「不可卸载」的 KDE 包由 Forma 包替代
#   ./forma-migrate.sh replace     卸载功能对应的 KDE 应用（安全列表，动态校验反向依赖）
#   ./forma-migrate.sh defaults    设置默认应用 / 终端 / 门户 / polkit 服务
#   ./forma-migrate.sh all         deps + build + install + replace + defaults
#   ./forma-migrate.sh rollback    恢复 KDE 全家桶
#
# 选项：--dry-run 只打印命令不执行；--without-portal 跳过门户替换（保留 KDE 门户）；
#       --jobs N 并行构建数；-y/--yes 不询问。
#
# 必须以普通桌面用户运行（makepkg 禁止 root），脚本内部自行调用 sudo。

set -euo pipefail
export LC_ALL=C

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PACKAGING="$REPO_ROOT/packaging"
BUILD_DIR="${FORMA_BUILD_DIR:-$REPO_ROOT/build-all}"
ARTIFACTS="$REPO_ROOT/.forma-artifacts.txt"
JOBS="${JOBS:-$(nproc)}"
DRY_RUN=0
WITH_PORTAL=1
ASSUME_YES=0

# ---- 构建单元：目录 => 该目录产出的包名 ----
BUILD_UNITS=(
  "arch-texteditor:forma-texteditor"
  "arch-finder:forma-finder"
  "arch-suite:forma-terminal forma-settings"
  "arch-media:forma-photos forma-video"
  "arch-extras:forma-calculator forma-music"
  "arch-mail:forma-mail"
  "arch-taskmanager:forma-taskmanager"
  "arch-appstore:forma-appstore"
  "arch-screenshot:forma-screenshot"
  "polkit-agent:forma-polkit-agent"
  "arch-portal:xdg-desktop-portal-forma"
)

# ---- 可直接移除的 KDE 应用：KDE包:Forma包 ----
KDE_REMOVABLE=(
  "dolphin:forma-finder"
  "kate:forma-texteditor"
  "konsole:forma-terminal"
  "plasma-systemmonitor:forma-taskmanager"
  "gwenview:forma-photos"
  "haruna:forma-video"
  "kcalc:forma-calculator"
  "spectacle:forma-screenshot"
)

# ---- 被 Plasma 硬依赖、不能单独移除的 KDE 包：由 Forma 包 provides/conflicts 顶替 ----
# 注意：systemsettings 不在列表里 —— 按需求与 forma-settings 共存，不动 KDE 系统设置。
KDE_PROVIDED=(
  "polkit-kde-agent:forma-polkit-agent"
  "xdg-desktop-portal-kde:xdg-desktop-portal-forma"
)

# ---- 与 Forma 共存、不做替换的 KDE 包 ----
KDE_COEXIST=(
  "systemsettings:forma-settings"
)

# ---- Forma 桌面 ID ----
ID_FINDER="io.github.egely1337.Mayonnaise.Finder.desktop"
ID_EDITOR="io.github.egely1337.Mayonnaise.TextEditor.desktop"
ID_PHOTOS="io.github.egely1337.Mayonnaise.Photos.desktop"
ID_VIDEO="io.github.egely1337.Mayonnaise.VideoPlayer.desktop"
ID_MUSIC="io.github.egely1337.Mayonnaise.Music.desktop"
ID_TERMINAL="io.github.egely1337.Mayonnaise.Terminal.desktop"

c_reset=$'\033[0m'; c_b=$'\033[1m'; c_g=$'\033[32m'; c_y=$'\033[33m'; c_r=$'\033[31m'; c_c=$'\033[36m'

say()  { printf '%s\n' "$*"; }
head1() { printf '\n%s==> %s%s\n' "$c_b$c_c" "$*" "$c_reset"; }
ok()   { printf '%s  ok %s %s\n' "$c_g" "$c_reset" "$*"; }
warn() { printf '%s  !! %s %s\n' "$c_y" "$c_reset" "$*"; }
err()  { printf '%s err %s %s\n' "$c_r" "$c_reset" "$*" >&2; }
die()  { err "$*"; exit 1; }

run() {
  if (( DRY_RUN )); then printf '  [dry-run] %s\n' "$*"; return 0; fi
  "$@"
}
run_sh() {
  if (( DRY_RUN )); then printf '  [dry-run] %s\n' "$*"; return 0; fi
  bash -c "$*"
}

# pacman 的冲突提示默认回答 No，需要显式喂 y。
# 注意：在 pipefail 下 `yes` 被 SIGPIPE 杀掉会让管道整体判成失败，这里单独关掉。
run_yes() {
  if (( DRY_RUN )); then printf '  [dry-run] yes | %s\n' "$*"; return 0; fi
  local rc
  set +o pipefail
  yes | "$@"
  rc=$?
  set -o pipefail
  return "$rc"
}

pkgs_installed() { pacman -Q "$@" >/dev/null 2>&1; }
pkg_installed() { pacman -Qi "$1" >/dev/null 2>&1; }
required_by() {
  local rb
  rb=$(pacman -Qi "$1" 2>/dev/null | sed -n 's/^Required By *: //p')
  [[ "$rb" == "None" ]] && rb=""
  printf '%s' "$rb"
}

parse_args() {
  local skip_next=0
  CMD=""
  for arg in "$@"; do
    if (( skip_next )); then skip_next=0; continue; fi
    case "$arg" in
      --dry-run) DRY_RUN=1 ;;
      --without-portal) WITH_PORTAL=0 ;;
      --jobs) skip_next=1 ;;
      --jobs=*) JOBS="${arg#--jobs=}" ;;
      -j*) JOBS="${arg#-j}" ;;
      -y|--yes) ASSUME_YES=1 ;;
      -h|--help) usage; exit 0 ;;
      -*) die "未知选项：$arg" ;;
      *) CMD="$arg" ;;
    esac
  done
  [[ -n "$CMD" ]] || { usage; exit 1; }
}

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

preflight() {
  local need_user="${1:-1}"
  if (( need_user )) && (( EUID == 0 )); then
    die "请用普通桌面用户运行（makepkg 禁止 root）：不要 sudo ./forma-migrate.sh；只看现状可用非 root 的 status"
  fi
  [[ -f /etc/arch-release || -f /etc/cachyos-release ]] || warn "非 Arch 系发行版，脚本未验证"
  pkg_installed pacman || die "找不到 pacman"
  [[ -d "$REPO_ROOT/applications" ]] || die "请在 Forma 仓库根目录运行（当前：$REPO_ROOT）"
  if [[ -n "$(find "$REPO_ROOT" -maxdepth 1 ! -user "$(id -u)" -print -quit 2>/dev/null)" ]]; then
    warn "仓库存在非当前用户拥有的文件，makepkg 会失败。建议先执行："
    warn "  sudo chown -R $(id -un):$(id -gn) \"$REPO_ROOT\""
  fi
  command -v makepkg >/dev/null || die "缺少 makepkg（base-devel）"
  command -v sudo >/dev/null || die "缺少 sudo"
}

# ---------------------------------------------------------------- status
cmd_status() {
  head1 "环境"
  say "仓库：$REPO_ROOT"
  say "桌面：${XDG_CURRENT_DESKTOP:-未知}  会话：${XDG_SESSION_TYPE:-未知}"
  say "构建目录：$BUILD_DIR"

  head1 "可以直接卸载（无反向依赖）"
  printf '  %-24s %-10s %s\n' "KDE 包" "状态" "Forma 替代"
  for entry in "${KDE_REMOVABLE[@]}"; do
    kde="${entry%%:*}"; forma="${entry##*:}"
    if pkg_installed "$kde"; then
      rb=$(required_by "$kde")
      if [[ -z "$rb" ]]; then
        printf '  %-24s %-10s %s\n' "$kde" "可卸载" "$forma"
      else
        printf '  %-24s %-10s %s（依赖它：%s）\n' "$kde" "有依赖!" "$forma" "$rb"
      fi
    else
      printf '  %-24s %-10s %s\n' "$kde" "未安装" "$forma"
    fi
  done

  head1 "被 Plasma 硬依赖：只能由 Forma 包 provides/conflicts 顶替"
  printf '  %-24s %-28s %s\n' "KDE 包" "依赖它的包" "Forma 替代"
  for entry in "${KDE_PROVIDED[@]}"; do
    kde="${entry%%:*}"; forma="${entry##*:}"
    if pkg_installed "$kde"; then
      printf '  %-24s %-28s %s\n' "$kde" "$(required_by "$kde" | tr -s ' ' ',')" "$forma"
    else
      printf '  %-24s %-28s %s\n' "$kde" "已由 Forma 顶替" "$forma"
    fi
  done

  head1 "与 Forma 共存（保留 KDE 原包）"
  printf '  %-24s %-10s %s\n' "KDE 包" "状态" "共存的 Forma 应用"
  for entry in "${KDE_COEXIST[@]}"; do
    kde="${entry%%:*}"; forma="${entry##*:}"
    if pkg_installed "$kde"; then
      printf '  %-24s %-10s %s\n' "$kde" "保留" "$forma"
    else
      printf '  %-24s %-10s %s\n' "$kde" "未安装" "$forma"
    fi
  done

  head1 "Forma 安装情况"
  for entry in "${BUILD_UNITS[@]}"; do
    for p in ${entry##*:}; do
      [[ "$p" == "xdg-desktop-portal-forma" ]] && (( ! WITH_PORTAL )) && continue
      if pkg_installed "$p"; then
        printf '  %-28s %s\n' "$p" "${c_g}已安装 $(pacman -Q "$p" | awk '{print $2}')${c_reset}"
      else
        printf '  %-28s %s\n' "$p" "未安装"
      fi
    done
  done

  head1 "无法替换、建议保留的 KDE 包（Forma 无对应功能）"
  local keep=(ark filelight kdialog kdeconnect partitionmanager kwalletmanager kio-admin kde-cli-tools)
  local line=""
  for p in "${keep[@]}"; do pkg_installed "$p" && line+="$p "; done
  say "  ${line:-（无）}"
}

# ---------------------------------------------------------------- deps
cmd_deps() {
  head1 "安装构建依赖"
  run sudo pacman -S --needed --asdeps \
    base-devel cmake pkgconf ninja extra-cmake-modules git gettext \
    sdl3 freetype2 fontconfig libglvnd vulkan-headers shaderc \
    glib2 ffmpeg curl libsecret json-glib flatpak
  (( DRY_RUN )) || ok "构建依赖就绪"
}

# ---------------------------------------------------------------- build
cmd_build() {
  head1 "构建 Forma 安装包（并行度 $JOBS）"
  (( DRY_RUN )) || : >"$ARTIFACTS"
  local failed=()
  for entry in "${BUILD_UNITS[@]}"; do
    local dir="${entry%%:*}"
    [[ "$dir" == "arch-portal" ]] && (( ! WITH_PORTAL )) && { warn "跳过 arch-portal（--without-portal）"; continue; }
    say ""
    say "${c_b}-- $dir${c_reset}"
    if (( DRY_RUN )); then
      say "  [dry-run] (cd packaging/$dir && FORMA_BUILD_DIR=$BUILD_DIR makepkg -sf --noconfirm)"
      continue
    fi
    touch "$REPO_ROOT/.build-stamp"
    if ( cd "$PACKAGING/$dir" && \
         FORMA_BUILD_DIR="$BUILD_DIR" FORMA_BUILD_JOBS="$JOBS" \
         makepkg -sf --noconfirm >"$REPO_ROOT/.build-$dir.log" 2>&1 ); then
      local found
      found=$(find "$PACKAGING/$dir" -maxdepth 1 -name '*.pkg.tar.zst' -newer "$REPO_ROOT/.build-stamp" | sort)
      if [[ -z "$found" ]]; then
        failed+=("$dir"); err "$dir 未产出安装包，见 .build-$dir.log"
      else
        while IFS= read -r f; do [[ -n "$f" ]] && printf '%s\n' "$f" >>"$ARTIFACTS"; done <<<"$found"
        ok "$dir -> $(echo "$found" | xargs -n1 basename | tr '\n' ' ')"
      fi
    else
      failed+=("$dir"); err "$dir 构建失败，见 .build-$dir.log（末尾如下）"
      tail -n 15 "$REPO_ROOT/.build-$dir.log" >&2 || true
    fi
  done
  rm -f "$REPO_ROOT/.build-stamp"

  (( DRY_RUN )) && return 0
  if (( ${#failed[@]} )); then
    err "以下单元构建失败：${failed[*]}"
    err "已成功产出的安装包记录在 $ARTIFACTS"
    [[ " ${failed[*]} " == *" arch-portal "* ]] && \
      err "门户单独失败时可加 --without-portal 只装应用、保留 KDE 门户"
    return 1
  fi
  ok "全部安装包已生成，清单：$ARTIFACTS"
}

# ---------------------------------------------------------------- install
cmd_install() {
  head1 "安装 Forma 全家桶"
  if (( DRY_RUN )) && [[ ! -s "$ARTIFACTS" ]]; then
    say "  [dry-run] 尚未构建；真实执行时会安装 .forma-artifacts.txt 里列出的包"
    return 0
  fi
  [[ -s "$ARTIFACTS" ]] || die "找不到 $ARTIFACTS，请先执行：./forma-migrate.sh build"
  mapfile -t files <"$ARTIFACTS"
  (( ${#files[@]} )) || die "安装包清单为空"

  # 一次性事务：pacman 在同一事务里移除 polkit-kde-agent / xdg-desktop-portal-kde，
  # 并安装声明了 provides 的 Forma 包，依赖关系不中断。
  # systemsettings 不参与替换，与 forma-settings 共存。
  say "将安装：$(printf '%s\n' "${files[@]}" | xargs -n1 basename | sed 's/-x86_64.*//' | tr '\n' ' ')"
  if (( ASSUME_YES == 0 )) && (( DRY_RUN == 0 )); then
    read -r -p "确认继续？[y/N] " ans
    [[ "$ans" == [yY] ]] || die "已取消"
  fi
  run sudo -v || true

  if (( DRY_RUN )); then
    say "  [dry-run] sudo pacman -U ${files[*]}"
  else
    run_yes sudo pacman -U "${files[@]}" || die "pacman 事务失败，请按上方输出排查后用 ./forma-migrate.sh status 复检"
    ok "事务完成"
  fi

  head1 "校验"
  local missing=()
  for entry in "${BUILD_UNITS[@]}"; do
    local dir="${entry%%:*}"
    [[ "$dir" == "arch-portal" ]] && (( ! WITH_PORTAL )) && continue
    for p in ${entry##*:}; do
      if (( DRY_RUN )); then continue; fi
      pkg_installed "$p" && ok "$p" || missing+=("$p")
    done
  done
  (( ${#missing[@]} )) && { err "未安装：${missing[*]}"; return 1; }

  if (( ! DRY_RUN )); then
    systemctl --user daemon-reload || true
    command -v update-desktop-database >/dev/null && update-desktop-database -q 2>/dev/null || true
    command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
  fi
  return 0
}

# ---------------------------------------------------------------- replace
cmd_replace() {
  head1 "卸载功能对应的 KDE 应用"
  local remove=() keep=()
  for entry in "${KDE_REMOVABLE[@]}"; do
    local kde="${entry%%:*}" forma="${entry##*:}"
    pkg_installed "$kde" || continue
    if ! pkg_installed "$forma" && (( ! DRY_RUN )); then
      warn "$forma 未安装，保留 $kde 以免功能缺失"; keep+=("$kde"); continue
    fi
    local rb; rb=$(required_by "$kde")
    if [[ -n "$rb" ]]; then
      warn "$kde 被 [$rb] 依赖，跳过（请改用替代依赖方案）"; keep+=("$kde"); continue
    fi
    remove+=("$kde")
  done

  if (( ${#remove[@]} == 0 )); then
    ok "没有需要移除的 KDE 应用"
  else
    say "将移除：${remove[*]}"
    # 不用 -Rns：不级联删除依赖，避免误伤仍被 Plasma 使用的库
    run sudo pacman -R --noconfirm "${remove[@]}"
    (( DRY_RUN )) || ok "已移除 ${#remove[@]} 个 KDE 应用"
  fi
  (( ${#keep[@]} )) && warn "保留：${keep[*]}"

  if (( ! DRY_RUN )); then
    command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    local orphans; orphans=$(pacman -Qtdq 2>/dev/null || true)
    if [[ -n "$orphans" ]]; then
      warn "存在孤立依赖（可用 sudo pacman -Rns \$(pacman -Qtdq) 清理）："
      say "$orphans" | sed 's/^/    /'
    fi
  fi
}

# ---------------------------------------------------------------- defaults
cmd_defaults() {
  head1 "设置默认应用"
  local mimes
  xdg_mime() {
    if (( DRY_RUN )); then
      printf '  [dry-run] xdg-mime default %s %s\n' "$1" "${*:2}"
      return 0
    fi
    # xdg-mime 在没有 qtpaths 的会话里会刷一堆探测警告，这里静音但不吞掉失败
    xdg-mime default "$1" "${@:2}" 2>/dev/null || warn "未能设置 $1（可稍后手动在「文件关联」里指定）"
  }

  xdg_mime "$ID_FINDER" inode/directory

  mimes=(text/plain text/markdown text/x-c++ text/x-csrc text/x-python text/x-shellscript \
         application/json application/x-yaml text/x-log)
  xdg_mime "$ID_EDITOR" "${mimes[@]}"

  mimes=(image/jpeg image/png image/gif image/bmp image/webp image/tiff image/avif)
  xdg_mime "$ID_PHOTOS" "${mimes[@]}"

  mimes=(video/mp4 video/x-matroska video/webm video/quicktime video/x-msvideo \
         video/mpeg video/ogg video/mp2t video/x-m4v)
  xdg_mime "$ID_VIDEO" "${mimes[@]}"

  mimes=(audio/mpeg audio/flac audio/ogg audio/opus audio/mp4 audio/x-m4a audio/aac audio/x-wav)
  xdg_mime "$ID_MUSIC" "${mimes[@]}"
  (( DRY_RUN )) || ok "mimeapps 默认应用已写入 ~/.config/mimeapps.list"

  head1 "KDE 终端集成"
  if command -v kwriteconfig6 >/dev/null; then
    run kwriteconfig6 --file kdeglobals --group General --key TerminalApplication forma-terminal
    run kwriteconfig6 --file kdeglobals --group General --key TerminalService "$ID_TERMINAL"
    ok "kdeglobals 中 \"打开终端\" 已指向 forma-terminal"
  else
    warn "未找到 kwriteconfig6，跳过终端集成"
  fi

  head1 "polkit 认证代理"
  if pkg_installed forma-polkit-agent && (( ! DRY_RUN )); then
    systemctl --user disable --now plasma-polkit-agent.service >/dev/null 2>&1 || true
    systemctl --user daemon-reload || true
    systemctl --user enable --now forma-polkit-agent.service
    ok "forma-polkit-agent.service 已启用（含 plasma-polkit-agent.service 别名）"
  else
    say "  [skip] 未安装 forma-polkit-agent"
  fi

  head1 "桌面门户"
  if pkg_installed xdg-desktop-portal-forma && (( ! DRY_RUN )); then
    systemctl --user daemon-reload || true
    systemctl --user restart plasma-xdg-desktop-portal-kde.service xdg-desktop-portal.service
    ok "门户服务已切换为 Forma 后端"
  else
    say "  [skip] 未安装 xdg-desktop-portal-forma（继续使用 KDE 门户）"
  fi

  head1 "截图快捷键"
  if command -v forma-screenshot-setup >/dev/null; then
    run forma-screenshot-setup || warn "forma-screenshot-setup 返回非零，请手动检查快捷键"
  else
    say "  [skip] 未安装 forma-screenshot"
  fi

  if (( ! DRY_RUN )); then
    command -v update-desktop-database >/dev/null && update-desktop-database -q ~/.local/share/applications 2>/dev/null || true
    command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t -f ~/.local/share/icons/hicolor 2>/dev/null || true
  fi
}

# ---------------------------------------------------------------- all
cmd_all() {
  cmd_deps
  cmd_build
  cmd_install
  cmd_replace
  cmd_defaults
  head1 "完成"
  say "重新登录一次会话，让菜单、门户与认证代理全部就位。"
  cmd_status
}

# ---------------------------------------------------------------- rollback
cmd_rollback() {
  head1 "回滚到 KDE 全家桶"
  if (( ASSUME_YES == 0 )) && (( DRY_RUN == 0 )); then
    read -r -p "将重新安装 KDE 应用并移除 Forma 包，确认？[y/N] " ans
    [[ "$ans" == [yY] ]] || die "已取消"
  fi

  if command -v forma-screenshot-shortcut >/dev/null && (( ! DRY_RUN )); then
    forma-screenshot-shortcut --restore >/dev/null 2>&1 || true
    ok "已尝试恢复 Spectacle 快捷键"
  fi

  local kde_pkgs=()
  for entry in "${KDE_REMOVABLE[@]}" "${KDE_PROVIDED[@]}"; do kde_pkgs+=("${entry%%:*}"); done

  run sudo -v || true
  say "重新安装：${kde_pkgs[*]}"
  if (( DRY_RUN )); then
    say "  [dry-run] yes | sudo pacman -S --needed ${kde_pkgs[*]}"
  else
    # 安装 polkit-kde-agent / xdg-desktop-portal-kde 时会与 Forma 包冲突，
    # 这里回答 y 让 pacman 在同一事务中移除对应的 Forma 包。
    run_yes sudo pacman -S --needed "${kde_pkgs[@]}" || warn "部分包安装失败，请检查输出"
  fi

  local forma_pkgs=()
  for entry in "${BUILD_UNITS[@]}"; do
    for p in ${entry##*:}; do
      pkg_installed "$p" && forma_pkgs+=("$p")
    done
  done
  if (( ${#forma_pkgs[@]} )); then
    say "移除 Forma 包：${forma_pkgs[*]}"
    run sudo pacman -Rns --noconfirm "${forma_pkgs[@]}" || warn "移除未完全成功"
  fi

  if (( ! DRY_RUN )); then
    systemctl --user daemon-reload || true
    systemctl --user enable --now plasma-polkit-agent.service >/dev/null 2>&1 || true
    systemctl --user restart plasma-xdg-desktop-portal-kde.service xdg-desktop-portal.service 2>/dev/null || true
    command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
  fi
  ok "回滚完成，建议注销重新登录"
}

main() {
  parse_args "$@"
  # status 是只读的，root 下也允许跑；其余命令必须普通用户
  if [[ "$CMD" == "status" ]]; then preflight 0; else preflight 1; fi
  case "$CMD" in
    status)   cmd_status ;;
    deps)     cmd_deps ;;
    build)    cmd_build ;;
    install)  cmd_install ;;
    replace)  cmd_replace ;;
    defaults) cmd_defaults ;;
    all)      cmd_all ;;
    rollback) cmd_rollback ;;
    *)        die "未知命令：$CMD" ;;
  esac
}

main "$@"
