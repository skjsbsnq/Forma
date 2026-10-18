# Arch Linux：Forma 桌面门户

KDE 6.8.0 的 Forma 界面分支，包含 KDE 功能服务、统一外观和独立文件选择器。

## 构建

从完整的 Forma 本地源码构建，包含当前未提交的修改：

```sh
git submodule update --init integrations/xdg-desktop-portal-kde
cd packaging/arch-portal
makepkg -s
```

`makepkg -s` 安装缺少的构建依赖，不安装生成的门户包。`FORMA_BUILD_JOBS=2 makepkg -s` 可降低并行构建内存用量。产物为 `xdg-desktop-portal-forma-6.8.0-1-x86_64.pkg.tar.zst`。

构建运行 28 项明暗组件检查与 5 项独立 D-Bus 检查，不授予权限或切换当前服务。

## 安装与启用

```sh
sudo pacman -U ./xdg-desktop-portal-forma-6.8.0-1-x86_64.pkg.tar.zst
systemctl --user daemon-reload
systemctl --user restart plasma-xdg-desktop-portal-kde.service xdg-desktop-portal.service
```

pacman 会提示与 `xdg-desktop-portal-kde` 冲突，确认移除官方包即可。新包提供兼容依赖，保留 Plasma 的依赖关系；它不是和官方包并存的第二个后端。

启动入口已启用 `/usr/bin/forma-file-picker`，systemd 和直接 D-Bus 激活都使用此入口，无需另外设置环境或修改 `portals.conf`。已有用户级 systemd 覆盖若修改过 ExecStart，需要自行检查。

日志：`journalctl --user -u plasma-xdg-desktop-portal-kde.service -b`

独立文件窗口预览：`forma-file-picker --preview`

## 恢复官方版本

```sh
sudo pacman -S xdg-desktop-portal-kde
systemctl --user daemon-reload
systemctl --user restart plasma-xdg-desktop-portal-kde.service xdg-desktop-portal.service
```

确认移除冲突的 Forma 包即可恢复。

## 当前边界

SDL 文件窗口尚需完善跨进程父窗口关联、Wayland 激活和严格模态行为。带额外选项时使用 Forma 外观的 KDE 文件组件。通知气泡、托盘宿主、KWin 自绘效果仍由桌面组件负责。真实录屏、远程控制、USB、打印等流程仍需会话和设备验收。参见安装文档中的 STYLE_COVERAGE.md。
