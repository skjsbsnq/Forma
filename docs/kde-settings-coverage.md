# KDE 系统设置覆盖度研究

研究日期：2026 年 10 月 18 日。对象：Forma 系统设置（`applications/system_settings`）对比
KDE Plasma 6.8 的「系统设置」（systemsettings）。

结论先行：**模块层面覆盖约一半，选项层面覆盖约三成**，且缺失集中在需要列表/编辑/测试界面
的硬件与窗口管理模块。

---

## 1. 研究方法

只看源码容易低估 KDE 的选项数量（很多模块的界面是运行时从 QML/插件生成的），所以这一轮的
做法是「把模块真正加载出来数控件」：

1. **模块清单**：`/usr/lib/qt6/plugins/plasma/kcms/{systemsettings,systemsettings_qwidgets}` 下
   的 81 个 `kcm_*.so`。用 `KPluginMetaData` 读取 `KPlugin.Name`、
   `X-KDE-System-Settings-Parent-Category` 与 `KPlugin.FormFactors`。
2. **真实控件树**：写了一个临时程序（链接 `KF6KCMUtils`），用
   `KCModuleLoader::loadModule()` 把每个模块装进离屏窗口，遍历
   `QQuickWidget::rootObject()` 的 QML 控件树，识别 `FormEntry`、`CheckBox`、`ComboBox`、
   `RadioButton`、`Slider`、`Switch`、`TextField`；可滚动区域按 `contentY += 0.85 × height`
   逐屏翻页采集。QWidget 型模块（`kcmshell6` 那批）直接遍历
   `QGroupBox` / `QComboBox` / `QAbstractButton` / `QTabWidget`。
3. **配置层交叉验证**：解析 `/usr/share/config.kcfg` 的 66 个 KConfigXT schema（480 个
   `<entry>`），与 `panels_kde.cpp` 等文件里 `kcfg::key / flagKey / multiKey / appliedKey`
   实际使用的键名比对。
4. **项目侧统计**：`ui::toggleRow / pickerRow / sliderRow / sliderValueRow / textRow` 以及
   直接构造的 `FormRow`。

采集结果：**69 个模块，151 个表单行，413 个控件/标签，208 个小节标题**。

局限：桌面特效、KWin 脚本、窗口规则、后台服务、备份、屏幕键盘这 6 个模块的内容在运行时从
插件/规则动态生成，本机环境为空（未装插件），因此它们的选项数是**下界**。

---

## 2. 总体数字

| 指标 | 数值 |
|---|---|
| Plasma 设置模块（插件文件） | 81 |
| 桌面形态可见模块（剔除 5 个仅 handset/tablet 模块） | **76** |
| 项目侧边栏面板 | 19（Plasma 下隐藏「健康使用」→ 18） |
| 与项目有交集的 KDE 模块 | **35** |
| 完全没有对应界面的模块 | **41** |
| 应用实际写入的 KConfig 键 | **66** + 15 个 KWin InputDevice 属性 |
| 桌面设置模块用到的 KConfigXT 声明条目 | 306（共 480） |
| KDE 可设置项估算总数 | **350–400** |
| 项目可操作的 KDE 设置项估算 | **≈110** |
| **选项级覆盖率** | **约 28%–32%** |

被剔除的 5 个仅移动端模块：`kcm_mobile_wifi`、`kcm_mobile_wired`、`kcm_cellular_network`、
`kcm_mobile_hotspot`、`kcm_mobile_power`（其 `KPlugin.FormFactors` 只有 `handset`/`tablet`，
桌面 systemsettings 不显示）。

---

## 3. 完全缺失的模块（41 个）

加粗项为 P0 修复目标。

### 3.1 硬件与外设（8）

| 模块 | 内容 |
|---|---|
| **`kcm_printer_manager`** | CUPS 队列、添加/删除、默认打印机、打印任务 |
| `kcm_bolt` | 雷电设备授权、安全等级 |
| `kcm_tablet` | 数位板压感、映射区、按钮、测试 |
| `kcm_gamecontroller` | 手柄按键映射、当作指针/键盘 |
| `kcm_touchscreen` | 触摸屏启用、映射到哪块屏 |
| `kcm_kwintouchscreen` | 触摸屏边缘手势 |
| `kcm_soundtheme` | 通知音主题、通知音开关 |
| `kcm_fontinst` | 安装/预览/删除字体 |

### 3.2 桌面与窗口管理（6）

| 模块 | 内容 |
|---|---|
| **`kcm_kwindecoration`** | 装饰插件、边框大小、标题栏按钮（左右两侧逐按钮编辑） |
| **`kcm_kwin_effects`** | 每个桌面特效的开关与配置 |
| **`kcm_kwin_scripts`** | 第三方 KWin 脚本启停、从文件安装 |
| `kcm_kwinrules` | 按窗口匹配的强制规则 |
| `kcm_activities` | 活动的创建/删除/命名 |
| `kcm_kwinxwayland` | Xwayland 键盘窃听、崩溃策略 |
| `kcm_qtquicksettings` | Plasma 渲染后端（GL/Vulkan/软件）、线程模式 |

### 3.3 系统与会话（7）

| 模块 | 内容 |
|---|---|
| **`kcm_autostart`** | `~/.config/autostart` 条目管理 |
| **`kcm_desktoppaths`** | 个人文件夹位置（Desktop/Documents/Downloads…） |
| `kcm_plasmalogin` | SDDM：自动登录、默认用户/会话、主题 |
| `kcm_smserver` | 注销确认、会话恢复、忽略应用、重启进固件 |
| `kcm_updates` | Discover 更新策略 |
| `kcm_kup` | 定时备份计划 |
| `kcm_kded` | KDED 后台服务启停 |

### 3.4 网络、账户与安全（6）

| 模块 | 内容 |
|---|---|
| `kcm_kaccounts` | KAccounts（Google/Nextcloud 等） |
| `kcm_firewall` | ufw/firewalld 规则 |
| `kcm_krdpserver` | KRDP 服务器、证书、质量 |
| `kcm_netpref` | FTP 被动模式、断点续传标记 |
| `kcm_app-permissions` | Flatpak 权限 |
| `kcm_feedback` | 遥测开关 |

### 3.5 区域与输入（5）

| 模块 | 内容 |
|---|---|
| `kcm_fcitx5` | Fcitx5 配置入口 |
| `kcm_spellchecking` | 语言、自动检测、忽略词表 |
| `kcm_nighttime` | 日出日落定位（壁纸明暗联动） |
| `kcm_virtualkeyboard` | 使用哪个虚拟键盘 |
| `kcm_plasmakeyboard` | 屏幕键盘语言、按键反馈、延迟 |
| `kcm_webshortcuts` | KRunner 的 `gg:` 之类关键词 |

### 3.6 外观与杂项（6）

| 模块 | 内容 |
|---|---|
| `kcm_filetypes` | MIME → 应用、嵌入组件 |
| `kcm_solid_actions` | 插入设备时执行的动作 |
| `kcm_plasmasearch` | KRunner 插件逐个启停 |
| `kcm_splashscreen` | 启动画面主题 |
| `kcm_landingpage` | 「快速设置」首页卡片 |
| `kcm_animations` | 动画速度与样式 |

---

## 4. 有交集但深度不足的模块

面板存在，但只做了皮毛。这一类比「完全没有」更隐蔽。

| 模块 | KDE 侧规模 | 项目覆盖 | 主要缺口 |
|---|---|---|---|
| `kcm_kwinoptions` 窗口行为 | 93 个 schema 条目 / 39 个控件 | 6 | 鼠标/中键/滚轮/标题栏动作、焦点窃取防护、自动抬升延迟、吸附区、多屏行为 |
| `kcm_powerdevilprofilesconfig` 电源 | 3 档 × 十余项 | 8 | 低电量/严重低电量动作、睡眠方式、键盘背光、每档亮度、执行脚本 |
| `kcm_kwintabbox` 任务切换器 | 2 套 × ~13 项 | 2 | 过滤条件、排序、可视化样式、每套快捷键、轮廓、高亮 |
| `kcm_kwinscreenedges` 屏幕边缘 | 8 角 + 8 边动作 | 3 | 每个角/边的独立动作、全屏时是否生效、触发延迟 |
| `kcm_baloofile` 文件搜索 | 10 条目 + 位置列表 | 3 | 索引位置增删、暂停索引、删除索引数据、排除规则 |
| `kcm_notifications` 通知 | 25 条目 / 17 控件 | 12 | 勿扰自动触发（镜像/共享/全屏）、超时指示条、低优先级历史、进度通知保持 |
| `kcm_access` 无障碍 | 37 条目 | 17 | 缩放倍率/区域/像素网格/指针跟随/文本光标跟踪/锐化、Dwell Clicker 细节 |
| `kcm_keyboard` 键盘 | 13 条目 + 布局 UI | 9 | 键盘型号、备用布局、按键测试区 |
| `kcm_pulseaudio` 声音 | 27 控件 | ~12 | 设备配置文件、重命名设备、测试扬声器、显示非活动设备 |
| `kcm_wallpaper` 壁纸 | 15 控件 | ~4 | 壁纸类型、定位、模糊、按屏设置、幻灯片、日夜两套 |
| `kcm_style` 应用程序样式 | 14 控件 | 1 | 按钮/菜单图标、工具栏文字、GTK 样式、样式引擎配置 |
| `kcm_icons` / `kcm_cursortheme` / `kcm_desktoptheme` / `kcm_lookandfeel` | 各 4–8 | 1 each | 只有选择器，没有安装/删除/预览/从文件安装 |
| `kcm_kscreen` 显示器 | 15+ | ~8 | 拖动排列、识别屏幕、X11 缩放策略、允许画面撕裂 |
| `kcm_users` 用户 | 9 | ~5 | 改密码、删除用户、改账户类型、指纹、邮箱 |
| `kcm_regionandlang` 区域与语言 | 11 | ~4 | LC_* 各分类（数字/货币/日期/排序）、翻译安装 |
| `kcm_recentFiles` 最近文件 | 6 | 2 | 只记特定应用、排除列表、「不记录」 |
| `kcm_workspace` 常规行为 | 21 控件 | 2 | 滚动条行为、平滑滚动、单击/双击策略、拖放策略、平板模式 |
| `kcm_animations` 动效 | 速度滑杆 + 样式 | 1 | 只剩「减少动画」 |
| `kcm_componentchooser` 默认应用 | 13 类 | 9 | 终端、压缩包、地图、电话 |
| `kcm_proxy` 代理 | 11 | 7 | 自动检测、环境变量、使用系统代理 |
| `kcm_mouse` / `kcm_touchpad` | 各 6–10 行 | 6 / 10 | 设备选择、滚动速度、中键方式、水平滚动 |

`kcm_krunnersettings` 是唯一一个**完整覆盖**的模块（3/3）。

---

## 5. 结构性观察

### 5.1 项目走的是「跨后端」路线

Plasma 下 Wi-Fi/有线/VPN 用 NetworkManager，蓝牙用 BlueZ，声音用 PipeWire/pactl，用户用
AccountsService，时间用 timedated。好处是同一套面板在 GNOME 与 Plasma 上都能用；代价是
**KDE 独有选项全部丢失**：

- NetworkManager 面板给不了 Plasma 的「连接首选项」（FTP 被动模式）、连接导出/导入、
  802.1X 企业网细节；
- BlueZ 面板给不了 BlueDevil 的适配器命名/可见性；
- PipeWire 面板给不了 Phonon 的设备配置文件、重命名设备。

### 5.2 写 KConfig 是「直接落键」

`panels_kde.cpp` 里 `kwinrc` 的 `NightColor.*`、`TabBox.*`、`Windows.Placement/FocusPolicy`
等键在 `kwin.kcfg` 里**没有声明**——它们是 KWin 运行时自己管理的键。目前能工作，但没有
schema 兜底，KWin 大版本变更时可能静默失效。

对 schema 声明键的命中情况：

| 文件 | schema 条目 | 项目使用 | 命中 |
|---|---|---|---|
| `kwinrc` | 33（`kwin.kcfg`，另有 60 个无 label 条目） | 22 | 7 |
| `kdeglobals` | 39 | 12 | 8 |
| `kaccessrc` | 21 | 8 | 5 |
| `kxkbrc` | 9 | 4 | 4 |
| `kscreenlockerrc` | 8 | 4 | 4 |
| `kcminputrc` | 6 | 6 | 6 |

### 5.3 缺失项的性质

缺失的 41 个模块里，**28 个需要「列表 + 编辑 + 测试」界面**（打印机、数位板、手柄、触摸屏、
窗口规则、特效列表、活动、自启动、字体管理、文件关联、设备动作、备份、KRunner 插件…）。
这不是加几个开关能解决的，每个都要一整套列表/编辑/空状态/错误状态。

---

## 6. 修复优先级

### P0（本次已实现）

| 模块 | 面板 id | 数据来源 | 说明 |
|---|---|---|---|
| `kcm_printer_manager` | `printers` | `libcups`（dlopen，无构建依赖） | 打印机列表、状态、任务、默认打印机、暂停/取消 |
| `kcm_kwindecoration` | `decorations` | `kwinrc` `[org.kde.kdecoration2]` + 插件目录扫描 | 装饰插件、边框大小、标题栏按钮左右编辑 |
| `kcm_kwin_effects` | `effects` | `/usr/share/kwin/{effects,builtin-effects}`、`~/.local/share/kwin/effects` 的 `metadata.json` | 每个特效开关 |
| `kcm_kwin_scripts` | `effects`（同一面板下半部） | `/usr/share/kwin/scripts`、`~/.local/share/kwin/scripts` | 脚本启停、从文件安装 |
| `kcm_autostart` | `autostart` | `~/.config/autostart/*.desktop`、`/etc/xdg/autostart` | 条目启停、删除、添加应用/命令 |
| `kcm_desktoppaths` | `locations` | `~/.config/user-dirs.dirs` | 9 个个人文件夹重定向 |

实现后：模块覆盖 **35 → 41 / 76**，缺失 **41 → 35**。

### P0 实现记录（本轮已完成）

| 面板 | id | 文件 | 行为 |
|---|---|---|---|
| 打印机 | `printers` | `panels_printing.cpp` | hero 状态卡 + 打印机列表（默认打印机置顶、状态圆点）+ 全局任务列表；点击打印机打开详情弹层（型号、状态、设为默认、暂停/恢复、取消全部任务、该打印机的任务）。`libcups` 用 `dlopen` 加载，没有 CUPS 的机器显示 `ui::unavailable` |
| 窗口装饰 | `decorations` | `panels_kwin.cpp` | 装饰插件 + Aurorae 主题合成一个 Picker；左右标题栏按钮各用「徽标 + 名称 + 上/下移/删除 + 添加」编辑器（与键盘布局编辑器同款）；边框大小、跟随主题推荐、按钮提示、双击窗口菜单关闭 |
| 桌面特效 | `effects` | `panels_kwin.cpp` | 按 KDE 分类分组，每个特效一行开关；分类标题来自元数据的英文 Category，走 UI 目录翻译；特效名与说明取 `Name[<lang>]` / `Description[<lang>]` |
| KWin 脚本 | 同上（同面板下半部） | `panels_kwin.cpp` | 每脚本一行开关；「从文件安装…」调用 `kpackagetool6 --type KWin/Script --install` |
| 登录项 | `autostart` | `panels_kwin.cpp` | 用户 `~/.config/autostart` + 系统 `/etc/xdg/autostart`；`Hidden=true` 过滤；开关写 `X-GNOME-Autostart-enabled`；系统条目只能停用、不显示删除；「添加应用…」复制 .desktop 到用户目录 |
| 位置 | `locations` | `panels_desktop.cpp` | 8 个 XDG 目录（桌面/文档/下载/音乐/图片/视频/公共/模板）；自定义过的行显示「重置」；写 `~/.config/user-dirs.dirs` 保留注释与其他键 |

关键实测结论（写进代码注释，避免后人踩坑）：

- **标题栏按钮字母**：用 `kwriteconfig6` + `org.kde.KWin.reconfigure` 逐个字母实测，
  `M`=窗口菜单、`N`=在所有桌面、`S`=卷起、`I`=最小化、`A`=最大化、`X`=关闭、`H`=上下文帮助、
  `B`=保持在下、`F`=保持在上、`E`=排除截图、`_`=占位；未列出的字母 KWin 直接丢弃。
- **边框大小**：`BorderSizeAuto=true` 时 `BorderSize` 完全不生效（实测 9 个取值 `borderSize` 都是 0）；
  关掉 auto 后 `Large/VeryLarge/Huge/Oversized` → 4/5/6/8。所以面板在「跟随主题推荐」开启时禁用该行，
  而不是隐藏它（避免布局跳动）。
- **KWin 脚本也用 `[Plugins]<id>Enabled`**：实测写入后 `org.kde.kwin.Scripting.isScriptLoaded` 从
  false 变 true，与特效同一个键空间，因此两者共用一个 `pluginRow`。
- **只读模式有效**：`MAYO_SCREENSHOT` / `SETTINGS_READ_ONLY=1` 下登录项开关与「位置」都不写文件
  （用 `diff -r` 验证过）；非只读运行能把 `X-GNOME-Autostart-enabled` 从 false 改回 true。

同时修掉的一个组件 bug：`Label` 在构造时就翻译文本，`.verbatim()` 只关掉后续翻译，已翻译的文本收不
回来——导致特效名「Magnifier」被 UI 词典换成「放大镜」。现在 `LabelView` 保留 `m_source`，
`verbatim()` 会还原原文；已加单元测试（`tests/test_widgets.cpp`）。

### P1（补齐已有面板深度，尚未做）

`kcm_kwinoptions` 鼠标绑定与焦点、电源低电量动作与键盘背光、任务切换器、屏幕边缘逐角动作、
baloo 索引位置、`kcm_workspace` 常规行为。

### P2（有替代后端，可长期不做）

在线账户、防火墙、远程桌面、备份、更新、拼写检查、输入法、Flatpak 权限。

---

## 7. 实现约束（与现有面板保持一致）

新面板遵守 `applications/system_settings` 既有约定，避免「看起来不像系统设置」：

- **视觉**：只用 `ui::page` / `ui::section` / `ui::hero` / `ui::DynamicSection` /
  `FormRow` / `Toggle` / `Picker` / `TextField`，颜色、圆角、字号全部来自 `mayo::Theme`；
  不新增自绘控件，不硬编码颜色。图标用 `mayo::Icon` 矢量图标 + `ui::tile`。
- **行距与卡片**：`FormRow` 统一 38pt 最小高度、12pt 内边距；分节之间 22pt；
  同类信息用 `DynamicSection`（自带空状态文案）。
- **交互**：与「键盘布局」编辑器一致的列表模式——徽标 + 名称 + 上移/删除，
  标题行右侧 `setAccessory` 放「添加」按钮；破坏性操作走 `ui::confirm`，
  失败走 `ui::message`（锚点为工具栏标题）。
- **数据**：KDE 侧一律走 `kcfg::key` / `flagKey` / `appliedKey`，写入后按需 `Reload`；
  只读模式（`MAYO_SCREENSHOT` / `SETTINGS_READ_ONLY=1`）不写任何东西。
- **翻译**：新增字符串加入 `applications/common/zh_cn.hpp`，服务值与路径不翻译。
- **降级**：服务缺失时用 `ui::unavailable(图标, 标题, 说明)`，不显示空表格。

---

## 8. 参考

- KDE 模块元数据：`KPluginMetaData`，`X-KDE-System-Settings-Parent-Category`，
  `KPlugin.FormFactors`；分类定义在 `/usr/share/systemsettings/categories/*.desktop`。
- 装饰按钮字母：`KDecoration3::DecorationButtonType` 的枚举顺序
  （`/usr/include/KDecoration3/kdecoration3/decorationdefines.h`）与实机
  `qdbus6 org.kde.KWin /KWin supportInformation` 的 `decorationButtonsRight: 6, 3, 4, 5`
  交叉核对（`H`=ContextHelp、`I`=Minimize、`A`=Maximize、`X`=Close）。
- 边框大小：`KDecoration3::BorderSize`（None、NoSides、Tiny、Normal、Large、VeryLarge、
  Huge、VeryHuge、Oversized）。
- 特效/脚本元数据：`/usr/share/kwin/builtin-effects/*.json`（单个 JSON 文件）与
  `/usr/share/kwin/{effects,scripts}/<id>/metadata.json`（包目录）。
