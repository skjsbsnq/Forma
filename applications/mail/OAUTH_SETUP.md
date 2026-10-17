# Gmail / Outlook OAuth 接入与正式发布验收

当前代码提供授权流程，**没有内置或借用其他应用的 Client ID**。首次真实登录之前，须为 Forma 注册 OAuth 应用。不要将邮箱密码、访问令牌、刷新令牌发到聊天或提交到仓库。

## Gmail

1. 在 [Google Cloud Console](https://console.cloud.google.com/auth/clients) 选择或创建自己的项目。
2. 配置 Google Auth Platform 的应用名称、受众、联系方式及测试用户；个人测试时把自己的 Google 账号加入测试用户。
3. 创建 **Desktop app / 桌面应用** 类型 OAuth 客户端。不要创建 Web、Android 或 iOS 客户端来替代。
4. 在权限配置中申请 IMAP/SMTP 使用的 `https://mail.google.com/` 邮件权限。面向公众发布时，应按 Google 对该权限的要求完成相应的应用验证；开发者测试不等于已获公开发布许可。
5. 打开 Forma「账户与连接」，选择 Gmail，填写邮箱地址和该客户端的 Client ID；如注册配置包含客户端密钥，填入相应字段。它是应用客户端配置，不是邮箱密码。
6. 点击「浏览器授权并连接」，在应用给出的系统浏览器页面登录，完成授权后返回 Forma。授权回调仅监听 `127.0.0.1` 随机端口，使用 PKCE 和随机 state。
7. 勾选保留登录状态时，刷新令牌通过系统钥匙环存储；钥匙环不可用会提示，下次可能需要重新授权。

Google 的测试用户、受众、授权期限与应用验证政策以控制台及官方文档为准。不要为了绕过验证使用其他软件的客户端 ID。

官方依据：[桌面应用 OAuth](https://developers.google.com/identity/protocols/oauth2/native-app)、[Gmail IMAP/SMTP OAuth](https://developers.google.com/workspace/gmail/imap/xoauth2-protocol)。

## Outlook / Microsoft 365

1. 打开 [Microsoft Entra 应用注册](https://entra.microsoft.com/#view/Microsoft_AAD_RegisteredApps/ApplicationsListBlade)，注册 Forma。
2. 按自己的账户类型选择支持的账户受众。需要支持个人 Outlook 与组织账号时，选择相应的多租户及个人 Microsoft 账户选项。
3. 为桌面/公共客户端启用公共客户端授权流程，以便使用设备代码登录。不要为此在桌面软件里嵌入机密客户端凭据。
4. 配置 Exchange Online 的委托权限 `IMAP.AccessAsUser.All`、`SMTP.Send`，并允许 `offline_access`。企业租户可能需要管理员同意或先启用邮箱 IMAP/SMTP AUTH。
5. 在 Forma 选择 Outlook，填写邮箱地址、Application (client) ID；Tenant 默认 `common`，仅组织账号可按注册配置填写租户 ID。
6. 点击浏览器授权，使用显示的设备代码在微软页面完成登录与同意。

官方依据：[IMAP/SMTP OAuth](https://learn.microsoft.com/en-us/exchange/client-developer/legacy-protocols/how-to-authenticate-an-imap-pop-smtp-application-by-using-oauth)、[设备授权流程](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code)。

## 标为正式版之前必须完成的真实验收

以下项目不能用本机模拟服务器测试替代：

- [ ] 已注册用于 Forma 的 Google 桌面客户端和 Microsoft 公共客户端，客户端配置有效。
- [ ] Gmail 浏览器授权、取消/拒绝授权、再次打开应用恢复登录均实测成功。
- [ ] Outlook 设备代码授权与刷新、个人/目标组织账号权限实测成功。
- [ ] 使用自己的测试邮箱确认真实收信、已读、星标、移动和加载更多。
- [ ] 由用户在应用中明确点击发送测试邮件；确认 To/Cc/Bcc、中文主题、附件到达且密送地址不泄露。
- [ ] 确认相应服务商“已发送”副本行为；当前版本未实现 IMAP APPEND 副本。
- [ ] 断网、令牌撤销/过期、钥匙环锁定、磁盘写入失败时错误明确，未丢草稿。
- [ ] 安装与升级包在干净 Arch 用户环境检查通过，未覆盖旧配置和草稿。

上述项目尚未完成前，发布产物标记为候选版，而不是宣称 Gmail/Outlook 正式支持已经验收。
