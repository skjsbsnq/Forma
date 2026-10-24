#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo="$base/../xdg-desktop-portal-kde"

# 子模块固定在 skjsbsnq fork 的 489f05eb（"Integrate Forma styling, file picker and
# previews into KDE portal"），该提交已经内含 Forma 集成；forma-portal.patch 只适用于
# 更早的 KDE 检出。先判断是否已集成，避免重复打补丁导致 prepare() 失败。
integrated() {
    [ -f "$repo/src/formastyle.cpp" ] && grep -q 'formastyle\.cpp' "$repo/src/CMakeLists.txt"
}

if integrated; then
    :
elif git -C "$repo" apply --check "$base/forma-portal.patch" 2>/dev/null; then
    git -C "$repo" apply "$base/forma-portal.patch"
elif ! git -C "$repo" apply --reverse --check "$base/forma-portal.patch" 2>/dev/null; then
    echo 'KDE checkout has incompatible changes; refusing to overwrite.' >&2
    exit 1
fi
copy() {
    if ! cmp -s "$1" "$2"; then cp "$1" "$2"; fi
}
for file in formafilepicker.cpp formafilepicker.h formastyle.cpp formastyle.h formapreview.cpp; do
    copy "$base/$file" "$repo/src/$file"
done
copy "$base/style/PortalDialog.qml" "$repo/src/PortalDialog.qml"
mkdir -p "$repo/src/forma/fonts" "$repo/src/forma/Forma"
for file in "$base"/style/Forma/*; do
    copy "$file" "$repo/src/forma/Forma/$(basename "$file")"
done
copy "$base/../../assets/fonts/InterVariable.ttf" "$repo/src/forma/fonts/InterVariable.ttf"
copy "$base/style/check.svg" "$repo/src/forma/check.svg"
copy "$base/../../assets/fonts/Inter-LICENSE.txt" "$repo/src/forma/fonts/Inter-LICENSE.txt"
