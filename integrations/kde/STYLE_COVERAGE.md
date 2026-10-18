# Forma 门户界面覆盖

本地 fork 保留 KDE 6.8.0 的功能服务，对**门户进程自己绘制的桌面窗口**统一外观。

文件选择主界面继续由 Forma / Mayonnaise（SDL）绘制；其余窗口保留 Qt Quick / Qt Widgets 的交互实现，使用私有 Forma 样式、Inter 字体、明暗配色、圆角与三色窗口按钮。它们不是重写成 SDL 的功能副本。这个取舍保留了屏幕/窗口模型、快捷键冲突处理、打印设置等已有能力。

| 功能 | 原有前端 | Forma 接入位置 |
|---|---|---|
| 打开、保存、选目录 | KFileWidget / QFileDialog | Forma 原生文件选择器；回退窗口也应用 Forma Widgets 样式 |
| 通用访问授权、额外选项 | AccessDialog | 统一窗口框架 + 复选框 / 下拉控件 |
| 应用选择、打开方式 | AppChooserDialog | 统一窗口框架 + 搜索框、应用网格、默认应用控件 |
| 账户信息 | UserInfoDialog | 统一窗口框架、头像与内容区域 |
| 屏幕共享、录屏来源选择 | ScreenChooserDialog / PipeWireDelegate | 统一窗口框架、圆角来源卡片、筛选与记住选择控件 |
| 远程控制 | RemoteDesktopDialog | 统一窗口框架 + 权限与记住选择控件 |
| 截图 | ScreenshotDialog | 统一窗口框架 + 截图模式、延迟与选项控件 |
| 共享区域选择叠层 | RegionSelectOverlay | 沿用选择逻辑，更新浮动提示材质与圆角 |
| 输入捕获 | InputCaptureDialog | 统一窗口框架 + 授权与持久化选项 |
| 全局快捷键 | GlobalShortcutsDialog | 统一窗口框架 + 原有快捷键编辑与冲突模型 |
| 动态启动器 | DynamicLauncherDialog | 统一窗口框架 + 启动器信息与编辑控件 |
| 壁纸确认 | WallpaperDialog | 统一窗口框架 + 图片预览 |
| USB 授权 | UsbDialog | 统一窗口框架 + 设备列表和移除按钮 |
| 后台运行许可 | QMessageBox | Forma Widgets 窗口装饰、按钮和调色板 |
| 打印与打印选项 | QPrintDialog | Forma Widgets 窗口装饰、输入框、标签页和按钮 |
| 辅助文件浏览、回退对话框 | FileDialog / QFileDialog | Forma Widgets 统一样式 |

## 不属于门户窗口的界面

通知气泡、托盘弹出菜单的宿主外壳由 Plasma Shell 绘制；KWin 自己显示的屏幕指示或选择效果由合成器绘制；点击“系统设置”“Discover”“邮件客户端”等启动的是独立应用。这些不在 xdg-desktop-portal-kde 的窗口实现中，不能通过本 fork 给外部进程换皮。

Settings、Secret、OpenURI、通知转发等接口可能没有门户自带窗口，保留功能实现即可。KDE 移动端文件选择器不属于本次 Linux 桌面验收范围。

## 验证边界

`forma-portal-preview` 不注册服务，使用只读示例数据加载真实 QML 组件与 Qt 打印/消息/文件对话框。自动检查两套配色的渲染、QML 错误、关闭返回拒绝，以及 Widgets 的父窗口与模态属性；这不是摄像头、USB、远程控制、录屏或真实打印的硬件/会话端到端验收。

预览中的“允许”等按钮只是显示，不连接真实权限服务。屏幕预览不会建立门户录屏会话；关闭测试不会持久化授权。
