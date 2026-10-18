#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo="$base/../xdg-desktop-portal-kde"
if git -C "$repo" apply --check "$base/forma-portal.patch" 2>/dev/null; then
    git -C "$repo" apply "$base/forma-portal.patch"
elif ! git -C "$repo" apply --reverse --check "$base/forma-portal.patch"; then
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
