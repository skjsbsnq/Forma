# Forma 文本编辑器 · Arch Linux

此目录从当前本地源码构建，包含尚未提交的汉化与界面修改。

```sh
cd Forma/packaging/arch-texteditor
makepkg -s
sudo pacman -U ./forma-texteditor-0.1.0-1-x86_64.pkg.tar.zst
```

安装后从应用菜单打开“Forma 文本编辑器”，或运行 `texteditor`。
包内含程序、中文桌面入口、Forma 家族 SVG 图标及许可证。
图标由 `python tools/make_forma_icons.py text_editor` 在仓库根目录重新生成。

默认使用独立的 `src/build` 目录构建 Release 版本，不更改日常开发构建目录。
可用 `FORMA_BUILD_JOBS=8 makepkg -s` 调整并行数。
字体跟随系统回退；中文推荐安装 `noto-fonts-cjk`。
