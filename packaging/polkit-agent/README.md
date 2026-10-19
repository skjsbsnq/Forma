# Arch Linux：Forma 身份验证代理

此配方从当前 Forma 源码构建，包含本地尚未提交的修改。当前版本为 `0.1.0-3`，包含 Forma 家族图标，并修复 NextKde 外轮廓与客户端圆角重叠造成的四角白边。

## 打包

```fish
cd /home/wwt/Projects/Forma/packaging/polkit-agent
makepkg -sf
```

产物：`forma-polkit-agent-0.1.0-3-x86_64.pkg.tar.zst`。`-s` 会通过 pacman 补齐缺少的构建依赖，`-f` 允许重新构建同版本安装包。

已经安装 Forma 代理时，直接升级并重启：

```fish
sudo pacman -U ./forma-polkit-agent-0.1.0-3-x86_64.pkg.tar.zst
systemctl --user restart forma-polkit-agent.service
```

## 移除 KDE 代理并安装 Forma

`polkit-kde-agent` 被 `plasma-desktop` 和 `plasma-meta` 依赖。新包同时声明 `provides=(polkit-kde-agent)` 和 `conflicts=(polkit-kde-agent)`，因此 `pacman -U` 会询问是否移除旧代理，并在同一个事务中安装新代理，保留 Plasma 的依赖关系。不要先单独执行 `pacman -Rdd`。

先完成上面的打包，再运行：

```fish
if sudo -v; and systemctl --user stop plasma-polkit-agent.service
    if sudo pacman -U ./forma-polkit-agent-0.1.0-3-x86_64.pkg.tar.zst
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

## 恢复 KDE 代理

```sh
systemctl --user disable --now forma-polkit-agent.service
sudo pacman -S polkit-kde-agent
systemctl --user daemon-reload
systemctl --user start plasma-polkit-agent.service
```

pacman 会询问是否移除冲突的 Forma 代理，选择 `y`。
