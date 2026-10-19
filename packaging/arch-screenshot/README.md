# Forma 截图：Arch Linux 独立应用

此配方只构建 Forma 框架和截图应用，产物为 `forma-screenshot-0.1.0-3-x86_64.pkg.tar.zst`。Forma 静态编译进应用，字体与渲染着色器内嵌；安装后不需要本仓库、不需要独立安装 Forma 框架，也不依赖其他 Forma 应用或 Spectacle。

目标环境为 Arch Linux x86_64、KDE Plasma Wayland。运行依赖由 pacman 管理；复制需要 wl-clipboard，快捷键设置依赖 Python GObject 与 KDE 服务。可选 libnotify 提供完成通知。

## 安装

```sh
sudo pacman -U ./forma-screenshot-0.1.0-3-x86_64.pkg.tar.zst
forma-screenshot-setup
```

第二条命令以桌面用户运行，不用 sudo。它迁移 Super+Shift+S、启动预热进程，并把旧的 `~/.local` 原型启动器、图标及程序备份到 `~/.local/state/forma-screenshot/legacy-install-*`，避免用户目录的旧版本覆盖 `/usr` 安装版。此命令只处理 Forma 截图的已知文件；原快捷键备份继续保留。

安装包提供独立启动器“Forma 截图”、区域／全屏／窗口三种右键动作，以及 KDE 登录预热入口。启动器与图标采用 `io.github.skjsbsnq.Forma.Screenshot` 标识。

```sh
forma-screenshot-shortcut --status
forma-screenshot-shortcut --restore
forma-screenshot --quit
```

移除使用 `sudo pacman -Rns forma-screenshot`；如需恢复 Spectacle 快捷键，先执行上面的 `--restore`。

## 从当前本地源码重新构建

```sh
cd packaging/arch-screenshot
makepkg -sf
```

默认并行度为 4，可设置 `FORMA_BUILD_JOBS=8`。构建关闭应用功能测试，并通过 `FORMA_SCREENSHOT_ONLY=ON` 排除其他应用及其额外构建依赖。

SVG 为原创 Forma 家族图标：连续圆角、青蓝渐变、玻璃截图卡片与取景框。源文件位于 `applications/screenshot/data/io.github.skjsbsnq.Forma.Screenshot.svg`，可通过 `python tools/make_forma_icons.py screenshot` 重新生成。
