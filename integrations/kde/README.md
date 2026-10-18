# Forma 桌面门户：KDE 功能服务 + Forma 界面

本地 fork 固定 KDE `v6.8.0`，保留 D-Bus 接口与功能实现，统一门户自己绘制的桌面窗口。**不会自动安装或替换当前会话的服务。**

- 文件选择主窗口由 Forma / Mayonnaise（SDL）独立绘制，复用 Finder 的文件模型、图标、网格、列表与缩略图。
- 授权、打开方式、账户、截图、屏幕共享、远程桌面、输入捕获、快捷键、USB、壁纸与启动器窗口保留 Qt Quick 的数据绑定，换成私有 Forma 窗口框架与控件样式。
- 打印、后台运行提示及文件选择回退窗口保留 Qt Widgets 的交互，使用 Forma 调色板、圆角控件与三色窗口按钮。

后两类不是 SDL 重写，屏幕/窗口模型、快捷键冲突检测、打印设置等仍由原实现处理。参见 [逐项覆盖和外部界面边界](STYLE_COVERAGE.md)。

![Forma 门户只读组件预览](../../docs/screenshots/forma-portals-overview.png)

## Arch Linux 安装包

本地打包配方和安装/恢复命令见 [packaging/arch-portal](../../packaging/arch-portal/README.md)。包内已配置 Forma 文件选择器的启动路径。

## 源码与构建

`../xdg-desktop-portal-kde/` 是上游子模块；`forma-portal.patch` 保存入口、样式注册及现有组件的适配；`style/` 和 `formastyle.*` 保存新窗口框架与私有样式；`formafilepicker.*` 连接独立文件选择器。KDE 侧新增代码遵循 LGPL-2.0-or-later，字体保留 Inter 的 OFL 许可证。

新克隆或更新子模块后运行 prepare.sh。它检查补丁，冲突时拒绝覆盖，重复运行不会重写相同文件。

```sh
git submodule update --init integrations/xdg-desktop-portal-kde
./integrations/kde/prepare.sh
cmake -S . -B build
cmake --build build --target forma-file-picker -j4
```

文件选择器额外依赖 Qt6 Core，仅用于 JSON、MIME 和 URL。没有该依赖时主项目跳过该前端。

KDE 服务需要上游开发依赖：ECM、Qt6、KF6、KWayland、KPipeWire、PipeWire，以及 `plasma-wayland-protocols`、`wayland-protocols`。桌面运行时已安装不代表协议开发文件齐全。

```sh
cmake -S integrations/xdg-desktop-portal-kde -B build-portal-kde \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-portal-kde --target xdg-desktop-portal-kde forma-portal-preview -j4
```

## 独立预览

文件选择窗口：

```sh
./build/applications/portal/forma-file-picker --preview
```

其他门户窗口的预览使用真实组件与示例数据，不注册门户服务、不授予权限，也不启动打印任务。关闭预览即可退出。

```sh
./build-portal-kde/bin/forma-portal-preview AccessDialog
./build-portal-kde/bin/forma-portal-preview AppChooserDialog
./build-portal-kde/bin/forma-portal-preview ScreenChooserDialog
./build-portal-kde/bin/forma-portal-preview print
```

完整组件名称见 `tests/portal/style_smoke.py` 的 CASES。传入第二个参数可以保存 PNG，追加 `--test` 则检查后自动退出。

`FORMA_APPEARANCE=light|dark` 强制整套界面的外观，包括启动的 Forma 文件选择进程；也兼容 `MAYO_APPEARANCE`。默认跟随桌面明暗设置。样式嵌在此 fork 内，不安装为全局 Qt/KDE 主题。

## 自动验证

```sh
# X11 / Xwayland：实际 SDL 窗口的文件筛选、选择、取消、目录、中文保存和覆盖确认
python tests/portal/picker_smoke.py build/applications/portal/forma-file-picker

# 独立 D-Bus 会话：结果、取消、Request.Close、非法 URI 回退和真实授权窗口拒绝
# 不替换登录会话里的服务
dbus-run-session -- python tests/portal/backend_smoke.py \
  build-portal-kde/bin/xdg-desktop-portal-kde

# 无需桌面窗口：14 类实际组件的明暗渲染、QML 绑定、关闭拒绝、父窗口/模态检查
python tests/portal/style_smoke.py build-portal-kde/bin/forma-portal-preview \
  --output /tmp/forma-portal-previews
```

这些是界面与协议边界检查，不替代真实录屏、远程控制、USB 或打印的端到端验收。只读预览通过后仍需在 KDE 会话中逐项实际验收。

## 启用与回退

修改版服务启动后统一应用 Forma 样式。若要启用独立的 SDL 文件选择器，还需给服务设置 `FORMA_PORTAL_PICKER` 为前端绝对路径；空值则在 PATH 查找 `forma-file-picker`。未设置时仍用 KDE 文件组件，但它也应用 Forma Widgets 外观。

```sh
dbus-run-session -- env \
  FORMA_PORTAL_PICKER="$PWD/build/applications/portal/forma-file-picker" \
  ./build-portal-kde/bin/xdg-desktop-portal-kde
```

上例只用于独立会话试用。正式切换需要安装修改后的 KDE 服务并设置服务环境，本项目未自动执行。服务名仍是 KDE，因此不需要为了此 fork 修改 `portals.conf`。

文件选择的私有通信是每进程一行 `version: 1` JSON：stdin 收请求、stdout 回结果、stderr 留诊断。KDE 侧验证状态、URI 和数量。辅助进程缺失、崩溃或输出不合法时回退到已应用 Forma 样式的 KDE 对话框；用户取消和 Request.Close 不触发第二个窗口。

## 尚需完善

- SDL 文件窗口的跨进程父窗口绑定、Wayland 激活和严格模态行为。
- 带 `choices` 的文件选择请求当前使用 Forma 外观的 KDE 文件组件；SDL 侧尚未绘制这些额外控件。
- SDL 文件窗口当前浏览本地文件；网络/KIO、挂载设备侧栏、最近使用及分栏视图尚未接入。KDE 回退保留原有能力。
- 非 UTF-8 Unix 文件名的私有 JSON 传递尚未覆盖。
- 此 KDE 版本未实现 SaveFiles，本 fork 没有新增该方法。
- 通知气泡、托盘宿主、KWin 自绘交互效果及外部设置应用不由门户进程绘制，仍由对应桌面组件负责。

视觉上已覆盖门户自带的桌面窗口；正式成为默认门户前，仍需补齐上述集成问题并完成真实会话验收。
