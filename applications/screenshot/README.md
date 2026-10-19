# Forma 截图

面向 KDE Wayland 的原生 C++ 截图应用。图片和选区由 Forma 画布绘制；浮动工具条直接使用 Forma 的 `ToolbarGroupView`、`SegmentedControlView`、`ButtonView`、`LabelView`，复用框架的 Liquid Glass 材质、主题、布局、键盘焦点、悬停和按压动画。界面不依赖 Qt 或 GTK。

## Arch Linux 独立安装包

安装 `forma-screenshot-0.1.0-3-x86_64.pkg.tar.zst` 后，应用可直接运行，Forma 框架、字体和着色器已内置，无需保留源码目录：

```sh
sudo pacman -U ./forma-screenshot-0.1.0-3-x86_64.pkg.tar.zst
forma-screenshot-setup
```

第二条命令使用普通桌面用户运行，处理快捷键、预热，以及旧用户级原型的备份迁移。详细说明见安装包内的 `ARCH.md`。

## 从源码安装

需要 Forma 的构建依赖、GIO、libpng、KDE Plasma Wayland，以及 `wl-clipboard`。`notify-send` 提供保存、复制成功通知；快捷键安装工具需要 Python GObject。

在仓库根目录执行：

```sh
./tools/install_forma_screenshot.sh
```

脚本安装到 `~/.local`，处理 KDE 会话 PATH 缺少用户 bin 目录的问题，将 `Super+Shift+S` 从 Spectacle 迁移到 Forma 并保留备份。同时创建 KDE 登录自启动入口并启动隐藏的预热进程。再次按快捷键通过用户私有 Unix socket 唤起同一个 Forma 窗口，保留 GPU、字体和纹理资源；隐藏期间窗口不参与帧调度。启动器的 Wayland 激活令牌会转发给常驻进程。

```sh
~/.local/bin/forma-screenshot-shortcut --status
~/.local/bin/forma-screenshot-shortcut --restore
~/.local/bin/forma-screenshot --quit
```

要停用登录预热，可删除 `~/.config/autostart/io.github.skjsbsnq.Forma.Screenshot.desktop`（设置了 `XDG_CONFIG_HOME` 时使用相应目录）。

## 截图交互

- `Super+Shift+S` 呼出区域截图；工具条或 `1 / 2 / 3` 切换区域、全屏、窗口。
- 拖动框选区域，选区内部拖动移动，四角及四边调整大小；按住 Shift 框选正方形。
- 方向键按原图像素微调位置，Shift + 方向键移动 10 像素。
- 双击选区、Enter、C 或 Ctrl+C 复制；S 或 Ctrl+S 保存；Esc 或右键取消。
- 工具条自动选择选区上方或下方的空位，使用平滑位移动画；拖动期间隐藏工具条。
- 截图、PNG 编码和文件／剪贴板传输在后台执行；输出与取消有短暂淡出，输出失败恢复原选区并显示原因。

默认保存到 XDG 图片目录内的 `Screenshots` 子文件夹，自动创建目录，文件名包含时间与毫秒。复制以真正的 `image/png` 数据交给 `wl-copy`，退出截图界面后仍可粘贴。

```sh
forma-screenshot --fullscreen --output /tmp/forma-shot.png
forma-screenshot --fullscreen --copy
forma-screenshot --region --save-dir "$HOME/Pictures/Screenshots"
forma-screenshot --once --region
forma-screenshot --background
```

`--copy`、`--save` 支持直接输出；区域模式在首次框选完成后输出。`--output FILE` 指定完整文件路径并隐含 `--save`。自定义 `--save-dir`、直接输出以及 `--once` 使用单次进程，常规快捷键入口使用常驻进程。`--background` 仅用于隐藏预热，不与单次输出选项组合。

## 实现边界

区域与全屏目前针对当前活动显示器，不包含跨屏选区或混合缩放多屏拼接。窗口模式使用 KWin 窗口选择器，预览保持宽高比，输出保留原始像素。底层使用 KWin ScreenShot2，面向 KDE Wayland；没有引入 Spectacle 作为截图依赖。标注、OCR、录屏和历史不在当前范围内。

PNG 使用单遍无损编码及快速压缩，优先减少交互等待，文件可能比高压缩 PNG 更大。尺寸映射依照实际窗口与原图大小计算，不把 150% 缩放硬编码到裁剪逻辑中。

## 构建与视觉预览

```sh
cmake -S . -B build
cmake --build build --target forma-screenshot -j4
MAYO_DEMO=selection MAYO_APPEARANCE=light \
  MAYO_SCREENSHOT=/tmp/forma-screenshot.png \
  build/applications/screenshot/forma-screenshot --once --region
```

视觉预览预选画面中部区域，不执行保存截图或复制图片动作。明暗主题可通过 `MAYO_APPEARANCE=light|dark` 预览。本轮优化只做编译、静态审阅和视觉预览，按用户要求不再运行功能测试。
