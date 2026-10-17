# Forma 邮件 0.2.0 候选版

基于 Mayonnaise C++20 / SDL3 的中文邮件客户端。第二版使用框架原生 Sidebar、SplitView、Toolbar、ToolbarGroup、Button、Menu、SearchField、Popover、ScrollView 和 Label；不再用整窗自绘卡片模拟控件。主题颜色、玻璃侧栏、按钮反馈与浅深色外观沿用框架。

## 已实现

- 邮箱侧栏、可拖动三栏布局、列表搜索、未读与星标筛选、正文选择和复制。
- 单个活动 IMAP/SMTP 账户，隐式 TLS 或强制 STARTTLS，验证服务器证书。
- 读取服务器文件夹，默认加载当前文件夹最新 40 封，可继续加载至 200 封邮件，读取正文使用 BODY.PEEK，不因下载擅自标已读。
- 中文 MIME 主题、发件人、Base64 / quoted-printable 正文和常见字符集解码；multipart 邮件和附件读取、附件保存。HTML 邮件转为不执行脚本、不加载远程内容的纯文本。
- 服务器已读/未读、星标、归档及移至废纸篓；邮件移动要求服务器支持 IMAP MOVE，失败会保留状态并显示错误。
- SMTP 发送 UTF-8 正文和附件。支持多收件人、抄送和密送，服务端接受后才显示提交成功；发送失败保留草稿。
- 多份本地草稿、自动保存、重新打开、回复；正文支持 UTF-8 光标、拖选、Shift 扩选、剪切复制粘贴、撤销重做。
- 可选系统 Secret Service 钥匙环保存应用密码，普通配置不包含密码；钥匙环保存失败会明确提示。应用配置目录和文件权限分别收紧为 0700 / 0600。

应用初次打开显示明确标注的示例邮箱。使用“账户与连接”填写邮箱服务商提供的服务器、登录名及应用专用密码；连接动作由用户触发。没有配置真实账户时发送按钮禁用。不要向聊天提供邮箱密码。

## 本轮完善

- Gmail 浏览器 PKCE / loopback OAuth、Outlook 设备代码 OAuth、令牌刷新及 XOAUTH2 邮件认证；提供注册入口与清楚的未配置提示。
- 多收件人、抄送/密送、UTF-8 附件发送；密送地址仅用于 SMTP envelope，不进入邮件头。
- 完整 MIME 接收解析、RFC2047 / RFC2231 中文主题与附件名、multipart/alternative 和内联附件。
- 账号隔离的离线邮件缓存，连接期间每 120 秒收信，加载更多（40 → 200 封）。
- 多份附件草稿改为原子 JSON 写入；正文编辑延迟保存，保存失败提示且阻止关闭丢失内容。
- 新的 OAuth、存储、附件往返、真实本机 TLS/XOAUTH2 协议及 UI 回归测试。

## 发布状态与限制

**尚不能标为正式版**：Google/Microsoft OAuth 应用尚未注册，也没有执行真实服务商的登录、刷新及收发验收。本机模拟协议测试验证实现流程，但不能证明服务商许可/账号策略已经满足。详见 [OAuth 配置与正式发布验收](OAUTH_SETUP.md)。

目前仅支持单个活动账户，无多账户同时在线、富文本撰写、HTML 原样排版或服务端搜索。每文件夹最多加载最近 200 封，单封接收最多 16 MB，发送附件合计最多 25 MB。SMTP 成功表示服务器确认接收；当前未实现向“已发送”文件夹 IMAP APPEND 副本，服务商是否自动保存需实际验证。邮件仅在应用运行且已连接时定时收取，不是独立系统后台服务。

## 构建与安装

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMAYONNAISE_USE_SYSTEM_DEPS=ON -DMAYONNAISE_BUILD_TESTS=OFF
cmake --build build --target forma-mail -j12
./build/applications/mail/forma-mail
```

除框架依赖外，需 libcurl、libsecret、json-glib 开发文件。Arch 中进入 `packaging/arch-mail` 执行 `makepkg` 只构建邮件组件；可设置 `FORMA_MAIL_BUILD_DIR` 复用已配置构建目录。

配置与草稿位于 `$XDG_CONFIG_HOME/mayonnaise/forma-mail/`，默认 `~/.config/mayonnaise/forma-mail/`。密码或 OAuth 刷新令牌存在系统钥匙环，或仅用于当前会话。草稿位于 drafts.json，缓存文件按账号和文件夹隔离。截图模式不会写配置或自动连接账户。

## 验证

- `forma-mail-storage-tests`：原子持久化、隔离、权限、密送隐私、中文附件发送/接收往返及 MIME 边界测试。
- `python3 applications/mail/test_oauth.py`：本机 HTTPS 模拟 OAuth 提供方，验证 PKCE、拒绝错误 state、交换/刷新与令牌轮换、设备代码 pending、取消；不连接真实账户。
- `forma-mail-tests`：MIME 解码、附件内容与文件名、HTML 纯文本转换、SMTP MIME 和邮件头注入拒绝。
- `python3 applications/mail/test_protocol.py`：建立临时本机 TLS IMAP/SMTP 服务器，验证拒绝未受信任证书、PEEK 收信、远端标记、MOVE、SMTP 数据及草稿重新加载；所有网络流量均限定在 localhost，使用临时配置和测试凭据。脚本默认二进制位于 `build/applications/mail/forma-mail-tests`。
- `FORMA_MAIL_UI_TEST=1 forma-mail`：UI 搜索、中文编辑、选择替换、拖选、撤销重做、只读正文事件测试。请使用临时 XDG_CONFIG_HOME。
- 实际运行检查浅色、深色、撰写和账户窗口截图。

## 视觉参考

- [Apple Mail / macOS Tahoe 26 用户指南](https://support.apple.com/guide/mail/welcome/16.0/mac/26)
- [官方 Mail 界面截图](https://help.apple.com/assets/6940590395B73B67500DF914/69405904627DAC39E4013406/en_US/87d606d0d0525dabe98de029012af1cd.png)
- [Apple Liquid Glass 介绍](https://www.apple.com/newsroom/2025/06/apple-introduces-a-delightful-and-elegant-new-software-design/)

参考图片仅用于设计核对，未随安装包分发。
