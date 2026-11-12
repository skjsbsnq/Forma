# Arch Linux：Forma 身份验证代理

此配方从当前 Forma 源码构建，包含本地尚未提交的修改。当前版本为 `0.1.0-6`，修复认证服务启动过早、缺少显示环境导致首次认证崩溃，以及 Plasma 启动依赖成环的问题；保留 Forma 家族图标和 NextKde 四角白边修复。

## 打包

```fish
# 从 Forma 仓库根目录开始
cd packaging/polkit-agent
makepkg -sf
```

产物：`forma-polkit-agent-0.1.0-6-x86_64.pkg.tar.zst`。`-s` 会通过 pacman 补齐缺少的构建依赖，`-f` 允许重新构建同版本安装包。

已经安装 Forma 代理时，升级、重载服务定义并重启。若有用户级完整服务覆盖，须先按下方「首次认证卡住」章节修正其 `After=` 行，否则它仍会覆盖包内修复：

```fish
sudo pacman -U ./forma-polkit-agent-0.1.0-6-x86_64.pkg.tar.zst
systemctl --user daemon-reload
systemctl --user restart forma-polkit-agent.service
```

## 移除 KDE 代理并安装 Forma

`polkit-kde-agent` 被 `plasma-desktop` 和 `plasma-meta` 依赖。新包同时声明 `provides=(polkit-kde-agent)` 和 `conflicts=(polkit-kde-agent)`，因此 `pacman -U` 会询问是否移除旧代理，并在同一个事务中安装新代理，保留 Plasma 的依赖关系。不要先单独执行 `pacman -Rdd`。

先完成上面的打包，再运行：

```fish
if sudo -v; and systemctl --user stop plasma-polkit-agent.service
    if sudo pacman -U ./forma-polkit-agent-0.1.0-6-x86_64.pkg.tar.zst
        systemctl --user daemon-reload
        systemctl --user enable --now forma-polkit-agent.service
        systemctl --user --no-pager status forma-polkit-agent.service
    else
        systemctl --user start plasma-polkit-agent.service
    end
end
```

pacman 询问移除 `polkit-kde-agent` 时选择 `y`。该操作卸载的是 KDE 的认证界面，不移除系统 `polkit` 认证服务。安装失败或取消时，上面的分支会重新启动原 KDE 代理。

启用 Forma 服务时，同时创建 `plasma-polkit-agent.service` 的兼容别名，满足 KDE `plasma-workspace.target` 的启动关系；后续登录会继续启动 Forma。不要使用 `sudo systemctl --user`。

检查注册成功：

```sh
journalctl --user -u forma-polkit-agent.service -n 30 --no-pager
```

日志应出现 `Authentication agent ready`。预览无需安装或替换服务：

```sh
/home/wwt/Projects/Forma/build-polkit/applications/polkit_agent/forma-polkit-agent --preview
```

## 首次认证卡住、反复触发才出现窗口

如果日志显示 `Authentication agent ready`，但第一次认证出现
`SDL video initialization failed: No available video device` 并崩溃重启，检查启动顺序：

```sh
systemctl --user cat forma-polkit-agent.service
systemctl --user show-environment
```

旧服务若只写 `After=graphical-session-pre.target`，可能在 KWin 导入
`WAYLAND_DISPLAY` / `DISPLAY` / `XAUTHORITY` 前就启动。常驻代理能注册，但进程
不会自动继承之后导入的显示变量，直到弹出密码窗口时才失败。自动重启后继承了
新的环境，所以第二次触发可能恢复。仅重启不能修复下次登录。

正确的启动顺序是：

```ini
After=graphical-session-pre.target plasma-core.target
```

不要改成 `After=graphical-session.target`：Plasma 的 workspace target 会通过
`plasma-polkit-agent.service` 别名拉起代理，并排在 graphical-session target 前面，
这样会形成循环依赖。

新版源码中的服务已修复此顺序。若 `systemctl --user cat` 显示加载的是
`~/.config/systemd/user/forma-polkit-agent.service`，这个完整的用户覆盖会遮住升级后的
包文件。先备份该文件，再用下列命令编辑完整服务，替换原有的 `After=` 行：

```sh
systemctl --user edit --full forma-polkit-agent.service
```

确认没有正在进行的认证，再重载并重启（不需要 sudo，也不用修改 PAM/polkit 规则）：

```sh
systemctl --user daemon-reload
systemctl --user restart forma-polkit-agent.service
```

重启后的日志应重新出现 `Authentication agent ready`。完整验证还需要下一次登录后
测试第一次认证；预览和离线依赖测试不等同于真实登录/PAM 测试。

## 恢复 KDE 代理

```sh
systemctl --user disable --now forma-polkit-agent.service
sudo pacman -S polkit-kde-agent
systemctl --user daemon-reload
systemctl --user start plasma-polkit-agent.service
```

pacman 会询问是否移除冲突的 Forma 代理，选择 `y`。
