# 用 Forma 全家桶替换 KDE 全家桶

> 本机实测环境：CachyOS（Arch 系）、KDE Plasma 6.8.1、Wayland 会话、16 核 / 14 GB。
> 策略：KDE「系统设置」与 Forma 系统设置**共存**，其余同功能应用由 Forma 接手。
> 仓库位置：`/home/wwt/文档/Forma`（含 `integrations/xdg-desktop-portal-kde` 子模块，KDE v6.8.0）。

## 一、结论先说

| 情形 | 数量 | 做法 |
| --- | --- | --- |
| 没有任何包依赖它，能干净卸载 | 8 | `pacman -R` 直接移除，Forma 同功能应用接手 |
| 被 Plasma 硬依赖，**不能单独卸载** | 2 | 由 Forma 包声明 `provides` + `conflicts`，在同一 pacman 事务里顶替 |
| **按你的要求共存，不替换** | 1 | `systemsettings` 原样保留，Forma 系统设置作为第二个入口并存 |
| Forma 没有对应功能 | — | 保留（ark、filelight、kdialog、kdeconnect、partitionmanager、kwalletmanager…） |

被硬依赖的两个包及其依赖者（`pacman -Qi` 实测）：

| KDE 包 | 依赖它的包 | 顶替它的 Forma 包 | 顶替方式 |
| --- | --- | --- | --- |
| `polkit-kde-agent` | `plasma-desktop` | `forma-polkit-agent` | 上游配方已含 `provides`/`conflicts` |
| `xdg-desktop-portal-kde` | `plasma-integration`、`portable` | `xdg-desktop-portal-forma` | 上游配方已含 `provides=("xdg-desktop-portal-kde=$pkgver" xdg-desktop-portal-impl)` |

`systemsettings` 虽然同样被 `kinfocenter`、`plasma-desktop` 依赖，但**不需要顶替**：它和 `forma-settings` 是两个独立的桌面项，可以同时存在。菜单里会同时看到「系统设置」和「Forma 系统设置」，命令 `systemsettings` 也照旧指向 KDE 的。

因此**不要**先 `pacman -Rdd` 强删，那会让 `plasma-desktop` 处于依赖损坏状态。正确做法是让 `pacman -U` 在同一个事务里「移除旧的 + 装入提供同名依赖的新包」。

## 二、完整对应关系

| KDE（本机版本） | Forma 应用 | 可执行文件 / 桌面 ID | 处理 |
| --- | --- | --- | --- |
| dolphin 26.08.2 | Forma Finder | `finder` / `…Mayonnaise.Finder` | 直接卸载 |
| kate 26.08.2 | Forma 文本编辑器 | `texteditor` / `…Mayonnaise.TextEditor` | 直接卸载 |
| konsole 26.08.2 | Forma 终端 | `forma-terminal` / `…Mayonnaise.Terminal` | 直接卸载 |
| systemsettings 6.8.1 | Forma 系统设置 | `forma-settings` / `…Mayonnaise.Settings` | **共存，不替换** |
| plasma-systemmonitor 6.8.1 | Forma 任务管理器 | `forma-taskmanager` / `…Mayonnaise.TaskManager` | 直接卸载 |
| gwenview 26.08.2 | Forma 照片 | `photos` / `…Mayonnaise.Photos` | 直接卸载 |
| haruna 1.8.1 | Forma 视频 | `video_player` / `…Mayonnaise.VideoPlayer` | 直接卸载 |
| （elisa 未装） | Forma 音乐 | `music` / `…Mayonnaise.Music` | 新增打包配方 |
| kcalc 26.08.2 | Forma 计算器 | `calculator` / `…Mayonnaise.Calculator` | 新增打包配方 |
| （discover 未装） | Forma 应用商店 | `forma-app-store` | 直接安装 |
| spectacle 6.8.1 | Forma 截图 | `forma-screenshot` | 直接卸载 |
| （kmail 未装） | Forma 邮件 | `forma-mail` | 直接安装 |
| polkit-kde-agent 6.8.1 | Forma 认证代理 | `forma-polkit-agent` | **替代依赖** |
| xdg-desktop-portal-kde 6.8.1 | Forma 门户 | `xdg-desktop-portal-forma` | **替代依赖** |

## 三、怎么装

```sh
cd ~/文档/Forma
sudo chown -R "$USER":"$USER" .          # makepkg 要求目录归当前用户所有

./forma-migrate.sh status                # 先看现状（只读）
./forma-migrate.sh all                   # 依赖 + 构建 + 安装 + 卸载 KDE + 设默认应用
```

分步执行：

```sh
./forma-migrate.sh deps        # 构建依赖
./forma-migrate.sh build       # 11 个打包单元 -> .forma-artifacts.txt
./forma-migrate.sh install     # 一次性 pacman 事务，含 3 个 provides/conflicts 顶替
./forma-migrate.sh replace     # 移除 8 个可卸载的 KDE 应用（带反向依赖二次校验）
./forma-migrate.sh defaults    # 默认应用、终端、polkit、门户、截图快捷键
```

常用开关：`--dry-run`（只打印命令）、`--without-portal`（不改门户，保留 KDE 门户）、`--jobs N`、`-y`。

安装完成后**注销并重新登录一次**：菜单缓存、门户 systemd 单元、认证代理都要在新会话里生效。

### 为什么默认应用需要单独设

Forma 应用虽然声明了 `MimeType=`，但 `kdeglobals` / `mimeapps.list` 里原本指向 Dolphin、Gwenview、Haruna 的默认项不会自动改写。脚本会写 `~/.config/mimeapps.list` 并设置：

- `inode/directory` → Forma Finder
- 文本类 → Forma 文本编辑器
- `image/*` → Forma 照片，`video/*` → Forma 视频，`audio/*` → Forma 音乐
- `kdeglobals [General] TerminalApplication=forma-terminal`（"打开终端"）

## 四、为了这件事改了哪些文件

| 文件 | 改动 |
| --- | --- |
| `packaging/arch-extras/PKGBUILD` | **新增**。上游没有 calculator / music 的 Arch 配方，这里补上（`forma-calculator`、`forma-music`） |
| `arch-finder / arch-media / arch-taskmanager / arch-appstore / arch-texteditor / arch-mail / arch-suite` 的 PKGBUILD | 构建目录改为 `${FORMA_BUILD_DIR:-$srcdir/build}`，让 9 个包共用一次编译，避免框架被重复编译 9 次 |
| `forma-migrate.sh` | **新增**。status / deps / build / install / replace / defaults / all / rollback |

查看改动：`git diff`。

## 五、已经验证过的部分

1. **全量编译通过**：`cmake -DMAYONNAISE_USE_SYSTEM_DEPS=ON -DMAYONNAISE_BUILD_TESTS=OFF` + `-j16`，1 分 22 秒产出 14 个可执行文件（含 `forma-file-picker`、`forma-screenshot`、`forma-polkit-agent`）。
2. **打包函数落地正确**：直接调用 PKGBUILD 的 `package_*` 函数，确认 `forma-settings`、`forma-calculator`、`forma-music` 的文件清单（二进制、desktop、图标、许可）符合预期。
3. **split 包流程跑通**：用 `makepkg --noarchive` 实际执行了 `arch-suite`（`forma-terminal` + `forma-settings` 双包）与 `arch-extras`，`pkg/` 下按包名正确拆分。
4. **全部 PKGBUILD 语法通过** `bash -n`。
5. **脚本各子命令 dry-run** 输出正确；`status` 的反向依赖解析在本机实测（`Required By: None` 归一化、多依赖逗号分隔）。

## 六、没能验证的部分（以及原因）

当前会话运行在受限沙箱里：`/usr`、`/etc`、`/var/lib/pacman` 均只读，`chown`/`sudo` 不可用，`makepkg` 又拒绝 root 身份。因此**真正的 `pacman -U` / `pacman -R` 无法在这里执行**，需要你在真实终端跑一遍脚本。未验证项：

- `pacman -U` 多包事务中两处 `provides/conflicts`（polkit 代理、门户）的实际替换过程（机制与上游配方既有做法一致）
- 门户（`xdg-desktop-portal-forma`）在真实会话里的文件选择器、录屏、打印流程
- polkit 认证弹窗、截图快捷键迁移的实际观感

## 七、已知边界

- **门户仍是开发中版本**：跨进程父窗口绑定、Wayland 严格模态、真实录屏 / 远程桌面 / USB / 打印未完成验收。介意的话用 `--without-portal`，只换应用不换门户（此时 `xdg-desktop-portal-kde` 保留）。
- **"打开终端"传参**：`forma-terminal` 接受一个目录参数（`argv[1]`），与 KIO 的常规调用兼容；若某个入口传的是 `--workdir` 这类 KDE 专有参数，会落到默认目录。
- **卸载不删配置**：Dolphin/Kate/Konsole 的 `~/.config/*rc`、`~/.local/share/dolphin` 等会留下，需要手清。
- **系统设置是双入口**：KDE「系统设置」和「Forma 系统设置」同时在菜单里，两者互不影响；Forma 那份覆盖面窄一些（取决于已装的 KDE 服务）。
- **ark、filelight 等保留**：Forma 没有压缩包管理、磁盘占用分析等对应功能。

## 八、回滚

```sh
./forma-migrate.sh rollback
```

会依次：恢复 Spectacle 快捷键 → 重装 `dolphin kate konsole plasma-systemmonitor gwenview haruna kcalc spectacle polkit-kde-agent xdg-desktop-portal-kde`（同一事务里移除冲突的 Forma 包）→ 清掉剩余 Forma 包 → 重启 polkit / 门户服务。之后注销重登。

## 九、排障记录

### `arch-portal` 在 prepare() 报 “patch does not apply”

现象：

```
error: patch failed: src/xdg-desktop-portal-kde.cpp:7
KDE checkout has incompatible changes; refusing to overwrite.
==> ERROR: A failure occurred in prepare().
```

原因：子模块固定在 `489f05eb`（提交信息 *Integrate Forma styling, file picker and previews into KDE portal*），**该提交本身已经包含 Forma 集成**；`integrations/kde/forma-portal.patch` 是给更早的 KDE 检出准备的，重复打补丁必然失败，反向校验也过不去（有两处 hunk 与提交内容不完全一致），于是落到 “refusing to overwrite” 分支。

修复：`integrations/kde/prepare.sh` 先判断是否已集成（存在 `src/formastyle.cpp` 且 `src/CMakeLists.txt` 引用了它），已集成就跳过 `git apply`，后面的文件拷贝本来就是 `cmp` 判重、天然幂等。现在 `prepare.sh` 可重复执行。

其它常见情况：

| 现象 | 处理 |
| --- | --- |
| `makepkg` 提示 “Running makepkg as root is not allowed” | 用桌面用户跑脚本，不要 `sudo ./forma-migrate.sh` |
| 链接阶段 `lto1: fatal error: write: No space left on device` | `/tmp` 太小（tmpfs）。`TMPDIR=/var/tmp ./forma-migrate.sh build` 或扩大 `/tmp` |
| 仓库文件属主不是自己，makepkg 报权限错误 | `sudo chown -R "$USER":"$USER" ~/文档/Forma`（脚本也会提示） |
