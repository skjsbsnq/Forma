#include "ui.hpp"
#include <spawn.h>
#include <sys/wait.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <cstdlib>

extern char** environ;
#include "../../src/render/font.hpp"
#include "editor.hpp"
#include "remote.hpp"

using namespace mayo;
namespace forma::mail {
namespace {
std::string lower(std::string s) {
    for (auto& c : s)
        if (static_cast<unsigned char>(c) < 128) c = static_cast<char>(std::tolower(c));
    return s;
}
bool validRecipients(const std::string& input, size_t& count) {
    if (input.find_first_of("\r\n") != std::string::npos) return false;
    std::vector<std::string> addresses;
    std::string token;
    bool quoted = false;
    int angled = 0;
    for (char c : input) {
        if (c == '"') quoted = !quoted;
        if (!quoted && c == '<') ++angled;
        if (!quoted && c == '>') --angled;
        if (angled < 0 || angled > 1) return false;
        if (!quoted && !angled && (c == ',' || c == ';')) {
            addresses.push_back(token);
            token.clear();
        } else
            token += c;
    }
    if (quoted || angled) return false;
    addresses.push_back(token);
    for (auto address : addresses) {
        auto first = address.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        address = address.substr(first, address.find_last_not_of(" \t") - first + 1);
        auto left = address.find('<');
        if (left != std::string::npos) {
            auto right = address.find('>', left);
            if (right == std::string::npos || address.find_first_not_of(" \t", right + 1) != std::string::npos)
                return false;
            address = address.substr(left + 1, right - left - 1);
        }
        auto at = address.find('@');
        if (at == std::string::npos || at == 0 || at + 1 == address.size() ||
            address.find('@', at + 1) != std::string::npos ||
            address.find_first_of(" \t<>(),;\r\n") != std::string::npos)
            return false;
        ++count;
    }
    return true;
}
Account providerDefaults(Account account, int provider) {
    account.provider = provider == 0 ? "google" : provider == 1 ? "microsoft" : "password";
    if (provider == 0) {
        account.imapHost = "imap.gmail.com";
        account.smtpHost = "smtp.gmail.com";
        account.imapPort = 993;
        account.smtpPort = 465;
        account.imapImplicitTLS = true;
        account.smtpImplicitTLS = true;
    }
    if (provider == 1) {
        account.imapHost = "outlook.office365.com";
        account.smtpHost = "smtp.office365.com";
        account.imapPort = 993;
        account.smtpPort = 587;
        account.imapImplicitTLS = true;
        account.smtpImplicitTLS = false;
    }
    return account;
}
std::string sizeText(size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string((bytes + 1023) / 1024) + " KB";
    return std::to_string((bytes + 1024 * 1024 - 1) / (1024 * 1024)) + " MB";
}
bool openAuthURL(const std::string& url) {
    if (url.rfind("https://", 0) != 0) return false;
    pid_t pid = 0;
    const char* args[] = {"xdg-open", url.c_str(), nullptr};
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(args), environ) != 0) return false;
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return true;
}
std::string firstGlyph(const std::string& s) {
    if (s.empty()) return "?";
    unsigned char c = static_cast<unsigned char>(s[0]);
    return s.substr(0, c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4);
}
void place(const std::shared_ptr<Widget>& widget, const Rect& rect) {
    widget->measure(rect.size());
    widget->arrange(rect);
}

class CenteredPanel final : public PopupView {
public:
    explicit CenteredPanel(const Element& content) : PopupView(Material::Popover) {
        setPadding(Insets(22));
        addChild(content.widget());
    }
    void present(WidgetHost& h) {
        m_shown = true;
        m_shownHost = &h;
        m_appear.set(0);
        m_appear.animateTo(1, h.time(), Transition{.15f, Curve::EaseOut});
        h.showOverlay(shared_from_this());
    }
    Rect placePopup(Size window, Size measured) override {
        return {(window.width - measured.width) * .5f, (window.height - measured.height) * .45f, measured.width,
                measured.height};
    }
};

// An actual independently interactive row in a standard framework ScrollView.
class MessageRow final : public Widget {
public:
    Message message;
    bool selected = false, draft = false;
    std::function<void()> activate, flag;
    MessageRow(Message value, bool chosen, bool isDraft = false)
        : message(std::move(value)), selected(chosen), draft(isDraft) {
        setFixedHeight(95);
        setCursor(CursorShape::PointingHand);
        setTooltip(message.subject);
    }
    bool acceptsFocus() const override { return true; }
    bool focusOnClick() const override { return true; }
    bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    Size measureContent(Size available) override { return {available.width, 95}; }
    void draw(Painter& p) override {
        const auto& t = theme();
        const auto& c = t.colors;
        Rect r = frame().inset(Insets(2, 6));
        if (selected)
            p.fillRoundedRect(r, 7, c.accent);
        else if (isHovered())
            p.fillRoundedRect(r, 7, c.hover);
        Color main = selected ? c.onAccent : c.label,
              sub = selected ? c.onAccent.multipliedAlpha(.78f) : c.secondaryLabel;
        const Font sender = t.font(TextStyle::Body).withWeight(FontWeight::Semibold);
        const Font subject =
            t.font(TextStyle::Body).withWeight(message.unread ? FontWeight::Medium : FontWeight::Regular);
        const Font preview = t.font(TextStyle::Subheadline);
        const float left = r.x + 19;
        if (message.unread) p.fillCircle({r.x + 8, r.y + 18}, 3, selected ? c.onAccent : c.accent);
        if (draft) p.drawIcon(Icon::Pencil, {r.x + 5, r.y + 13, 10, 12}, selected ? c.onAccent : c.accent);
        p.pushClip(r.inset(4));
        p.drawText(message.sender, Rect{left, r.y + 6, r.width - 100, 20}, sender, main);
        p.drawText(message.date, Rect{r.right() - 76, r.y + 7, 64, 18}, t.font(TextStyle::Caption), sub,
                   TextAlign::Trailing);
        p.pushClip({left, r.y + 28, r.width - 31, 20});
        p.drawText(message.subject.empty() ? "（无主题）" : message.subject, Rect{left, r.y + 28, r.width - 31, 20},
                   subject, main);
        p.popClip();
        auto lines = render::wrapText(message.preview, preview, r.width - 33);
        float y = r.y + 49;
        for (size_t i = 0; i < std::min(size_t(2), lines.size()); ++i) {
            auto& l = lines[i];
            p.drawText(std::string_view(message.preview).substr(l.begin, l.end - l.begin),
                       Rect{left, y, r.width - 33, 18}, preview, sub);
            y += 17;
        }
        if (message.star)
            p.drawIcon(Icon::Star, {r.right() - 20, r.y + 72, 12, 12}, selected ? c.onAccent : Color::hex(0xE8A53C));
        p.popClip();
        if (!selected) p.fillRect({left, r.bottom(), r.right() - left - 10, .5f}, c.separator.multipliedAlpha(.45f));
    }
    bool onPointerDown(const PointerEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        return true;
    }
    void onPointerUp(const PointerEvent& e) override {
        if (!frame().contains(e.position)) return;
        if (e.position.x > frame().right() - 30 && e.position.y > frame().bottom() - 30 && flag)
            flag();
        else if (activate)
            activate();
    }
    bool onKeyDown(const KeyEvent& e) override {
        if (e.key == Key::Enter || e.key == Key::Space) {
            if (activate) activate();
            return true;
        }
        return false;
    }
    void onHoverChanged(bool) override { invalidate(); }
};
class Avatar final : public Widget {
public:
    std::string initial;
    explicit Avatar(std::string value) : initial(firstGlyph(value)) {
        setFixedWidth(36);
        setFixedHeight(36);
    }
    void draw(Painter& p) override {
        auto r = frame();
        p.fillCircle(r.center(), 17, theme().colors.accent.multipliedAlpha(.12f));
        p.drawText(initial, r, theme().font(TextStyle::Headline), theme().colors.accent, TextAlign::Center);
    }
};

// A plain adjustable split for list and reading pane. The outer split remains
// the real framework SplitView and supplies the floating glass sidebar.
class MailColumns final : public Widget {
    class Handle final : public Widget {
        MailColumns& owner;
        float start = -1, width = 0;

    public:
        explicit Handle(MailColumns& o) : owner(o) { setCursor(CursorShape::ResizeHorizontal); }
        bool isInteractive() const override { return true; }
        bool onPointerDown(const PointerEvent& e) override {
            if (e.button != MouseButton::Left) return false;
            start = e.position.x;
            width = owner.listWidth;
            return true;
        }
        void onPointerMove(const PointerEvent& e) override {
            if (start >= 0) {
                owner.listWidth =
                    std::clamp(width + e.position.x - start, 265.0f, std::max(265.0f, owner.frame().width - 330));
                owner.invalidateLayout();
                if (owner.resized) owner.resized(owner.listWidth);
            }
        }
        void onPointerUp(const PointerEvent&) override { start = -1; }
    };

public:
    float listWidth = 305;
    std::function<void(float)> resized;
    std::shared_ptr<Widget> list, reading;
    std::shared_ptr<Handle> handle;
    MailColumns(std::shared_ptr<Widget> l, std::shared_ptr<Widget> r)
        : list(std::move(l)), reading(std::move(r)), handle(std::make_shared<Handle>(*this)) {
        addChild(list);
        addChild(reading);
        addChild(handle);
    }
    bool expandsContent(Axis) const override { return true; }
    bool extendsIntoTitleBar() const override { return true; }
    Size measureContent(Size a) override { return a; }
    void arrangeContent(const Rect& r) override {
        const float bar = host() ? host()->titleBarHeight() : 52;
        float lw = std::clamp(listWidth, 265.0f, std::max(265.0f, r.width - 330));
        place(list, {r.x, r.y + bar, lw, r.height - bar});
        place(reading, {r.x + lw + 1, r.y + bar, r.width - lw - 1, r.height - bar});
        place(handle, {r.x + lw - 3, r.y + bar, 7, r.height - bar});
    }
    void draw(Painter& p) override {
        p.fillRect(frame(), theme().colors.contentBackground);
        paintChildren(p);
        const auto r = frame();
        p.fillRect({r.x + list->frame().width, r.y, 1, r.height}, theme().colors.separator.multipliedAlpha(.65f));
    }
};
Button tool(Icon icon, std::string tip, std::function<void()> action) {
    return Button(icon).style(ButtonStyle::Borderless).tooltip(std::move(tip)).onClick(std::move(action));
}
struct ComposeSession {
    Draft draft;
    Window* window = nullptr;
    bool alive = true, sent = false, sending = false;
    std::string lastSaveError;
    bool dirty = false;
    std::uint64_t revision = 0;
    std::function<void()> refreshAttachments;
    Label status;
    Label from;
    VStack attachmentRows;
    ScrollView attachmentScroll;
    std::vector<std::weak_ptr<Widget>> editable;
    Button send;
    std::shared_ptr<TextEditor> editor;
    ComposeSession()
        : status(""), from(""), attachmentRows({}), attachmentScroll(attachmentRows), send(Icon::Share, "发送") {}
};

struct AccountSession {
    bool alive = true, authorizing = false;
    Window* window = nullptr;
    Account previous;
    Picker provider, imapTLS, smtpTLS;
    TextField name, address, username, password, clientId, clientSecret, tenant, imap, smtp, imapPort, smtpPort;
    Checkbox remember;
    Label status, hint, authMessage, authCode;
    VStack content, fields, authSection;
    ScrollView scroll;
    ProgressBar spinner;
    Button connect, cancel;
    std::shared_ptr<TextEditor> authURL;
    Backend::AuthPrompt prompt;
    AccountSession(const Account& a)
        : previous(a),
          provider({"Gmail · Google", "Outlook · Microsoft", "其他邮箱 · 密码"}, a.provider == "google"      ? 0
                                                                                 : a.provider == "microsoft" ? 1
                                                                                                             : 2),
          imapTLS({"TLS", "STARTTLS"}, a.imapImplicitTLS ? 0 : 1),
          smtpTLS({"TLS", "STARTTLS"}, a.smtpImplicitTLS ? 0 : 1),
          name("例如：个人邮箱"),
          address("you@example.com"),
          username("通常与邮箱地址相同"),
          password("应用专用密码"),
          clientId("已注册的 OAuth Client ID"),
          clientSecret("可选，Google 桌面客户端密钥"),
          tenant("common"),
          imap("imap.example.com"),
          smtp("smtp.example.com"),
          imapPort("993"),
          smtpPort("465"),
          remember("保留登录状态（存入系统钥匙环）", a.rememberPassword),
          status(""),
          hint(""),
          authMessage(""),
          authCode(""),
          content({}),
          fields({}),
          authSection({}),
          scroll(content),
          spinner(ProgressSpinner()),
          connect("连接账户"),
          cancel("取消授权"),
          authURL(std::make_shared<TextEditor>("", false)) {
        name.setText(a.name);
        address.setText(a.address);
        username.setText(a.username);
        clientId.setText(a.clientId);
        tenant.setText(a.tenant.empty() ? "common" : a.tenant);
        imap.setText(a.imapHost);
        smtp.setText(a.smtpHost);
        imapPort.setText(std::to_string(a.imapPort));
        smtpPort.setText(std::to_string(a.smtpPort));
        password.secure();
        clientSecret.secure();
        for (auto field : {name, address, username, password, clientId, clientSecret, tenant, imap, smtp})
            field.width(320);
        provider.width(320);
        imapPort.width(80);
        smtpPort.width(80);
        imapTLS.width(215);
        smtpTLS.width(215);
        authURL->setLineLimit(3);
        authURL->setFont(Font{11, FontWeight::Regular, 17});
    }
};
}  // namespace

struct MailUI::Impl {
    App& app;
    Window& window;
    Backend& backend;
    MailUI& owner;
    Sidebar sidebar;
    SplitView split;
    VStack listContent, readerContent;
    ScrollView listScroll, readerScroll;
    VStack listPane, readerPane;
    Label mailboxTitle, mailboxCount, statusLabel;
    TextField search;
    Button syncButton, composeButton, archiveButton, deleteButton, replyButton, flagButton, accountButton, filterButton;
    Menu moreMenu;
    std::shared_ptr<MailColumns> columns;
    std::string folder = "INBOX", selected, query, lastSyncError;
    bool starredOnly = false, unreadOnly = false, drafts = false;
    bool mainAlive = true;
    Window* accountWindow = nullptr;
    std::vector<std::weak_ptr<ComposeSession>> sessions;
    std::shared_ptr<CenteredPanel> panel;
    std::shared_ptr<Popover> infoPopover;
    Impl(App& a, Window& w, Backend& b, MailUI& o)
        : app(a),
          window(w),
          backend(b),
          owner(o),
          sidebar(std::vector<SidebarItem>{}),
          split(sidebar, VStack({})),
          listContent({}),
          readerContent({}),
          listScroll(listContent),
          readerScroll(readerContent),
          listPane({}),
          readerPane({}),
          mailboxTitle("收件箱"),
          mailboxCount(""),
          statusLabel(""),
          search(SearchField("搜索")),
          syncButton(Icon::Envelope),
          composeButton(Icon::Pencil),
          archiveButton(Icon::Folder),
          deleteButton(Icon::Trash),
          replyButton(Icon::ArrowLeft),
          flagButton(Icon::Star),
          accountButton(Icon::PersonCircle),
          filterButton(Icon::List),
          moreMenu("", {}) {}
    std::weak_ptr<MailUI> weak() { return owner.weak_from_this(); }
    const Message* current() const {
        for (const auto& msg : backend.messages())
            if (msg.id == selected) return &msg;
        return nullptr;
    }
    bool saveSession(const std::shared_ptr<ComposeSession>& session) {
        ++session->revision;
        session->draft.id = backend.saveDraft(session->draft);
        session->lastSaveError = backend.draftError();
        session->dirty = !session->lastSaveError.empty();
        session->status.node()->setTooltip(session->lastSaveError);
        session->status.setText(session->lastSaveError.empty() ? "草稿已保存"
                                                               : "草稿保存失败：" + session->lastSaveError);
        if (drafts) owner.refresh();
        return session->lastSaveError.empty();
    }
    void queueSave(const std::shared_ptr<ComposeSession>& session) {
        session->dirty = true;
        const auto revision = ++session->revision;
        session->status.setText("正在保存草稿…");
        auto w = weak();
        std::weak_ptr<ComposeSession> pending = session;
        app.after(.5, [w, pending, revision] {
            auto ui = w.lock();
            auto current = pending.lock();
            if (ui && current && current->alive && current->dirty && current->revision == revision)
                ui->m->saveSession(current);
        });
    }
    std::vector<Message> filtered() const {
        std::vector<Message> rows;
        if (backend.busy() && !drafts && backend.currentFolder() != folder) return rows;
        if (drafts) {
            for (auto& d : backend.drafts()) {
                Message m;
                m.id = d.id;
                m.sender = d.to.empty() ? "新邮件" : d.to;
                m.subject = d.subject;
                m.preview = d.body;
                m.body = d.body;
                m.date = "草稿";
                rows.push_back(std::move(m));
            }
        } else
            for (auto& m : backend.messages()) {
                if (backend.demo() && m.folder != backend.currentFolder()) continue;
                if (starredOnly && !m.star) continue;
                if (unreadOnly && !m.unread) continue;
                rows.push_back(m);
            }
        rows.erase(
            std::remove_if(rows.begin(), rows.end(),
                           [&](const Message& m) {
                               return !query.empty() &&
                                      lower(m.sender + m.address + m.subject + m.preview + m.body).find(lower(query)) ==
                                          std::string::npos;
                           }),
            rows.end());
        return rows;
    }
    void error(const std::string& text) {
        statusLabel.setText(text);
        showAlert("操作未完成", text);
    }
    void showAlert(const std::string& title, const std::string& text) {
        auto weakOwner = weak();
        auto close = Button("好").style(ButtonStyle::Prominent).onClick([weakOwner] {
            if (auto ui = weakOwner.lock())
                if (ui->m->panel) ui->m->panel->dismiss();
        });
        panel = std::make_shared<CenteredPanel>(
            VStack({Label(title).font(TextStyle::Headline), Label(text).secondary(), HStack({Spacer(), close})})
                .spacing(14)
                .width(400));
        if (auto* h = split.node()->host()) panel->present(*h);
    }
    Backend::Done done() {
        auto w = weak();
        return [w](const std::string& err) {
            if (auto ui = w.lock()) {
                ui->m->lastSyncError = err;
                if (!err.empty()) ui->m->folder = ui->m->backend.currentFolder();
                ui->refresh();
                if (!err.empty()) ui->m->error(err);
            }
        };
    }
    void select(const std::string& id, bool markRead = true) {
        selected = id;
        readerScroll.scrollTo({0, 0}, false);
        refreshReader();
        refreshList();
        if (markRead && !drafts) {
            if (auto* msg = current();
                msg && msg->unread && !backend.busy() && (backend.connected() || backend.demo())) {
                backend.setRead(id, true, done());
                owner.refresh();
            }
        }
    }
    void refreshList() {
        listContent.clear();
        auto rows = filtered();
        mailboxCount.setText(std::to_string(rows.size()) + (drafts ? " 份草稿" : " 封邮件"));
        for (auto& msg : rows) {
            auto row = std::make_shared<MessageRow>(msg, msg.id == selected, drafts);
            auto w = weak();
            std::string id = msg.id;
            row->activate = [w, id] {
                if (auto ui = w.lock()) {
                    if (ui->m->drafts) {
                        for (auto& d : ui->m->backend.drafts())
                            if (d.id == id) {
                                ui->compose(&d);
                                break;
                            }
                    } else
                        ui->m->select(id);
                }
            };
            row->flag = [w, id] {
                if (auto ui = w.lock()) {
                    auto& b = ui->m->backend;
                    if (b.busy()) return;
                    for (auto& m : b.messages())
                        if (m.id == id) {
                            b.setStar(id, !m.star, ui->m->done());
                            ui->refresh();
                            break;
                        }
                }
            };
            listContent.add(row);
        }
        if (!drafts && backend.connected() &&
            backend.messages().size() >= static_cast<size_t>(backend.messageLimit())) {
            auto w = weak();
            listContent.add(Button(backend.busy() ? "正在加载…" : "加载更多邮件")
                                .style(ButtonStyle::Borderless)
                                .disabled(backend.busy())
                                .fillWidth()
                                .padding(Insets(10, 12))
                                .onClick([w] {
                                    if (auto ui = w.lock()) {
                                        if (ui->m->backend.busy()) return;
                                        ui->m->backend.loadMore(ui->m->done());
                                        ui->refresh();
                                    }
                                }));
        }
        if (rows.empty())
            listContent.add(VStack({Label(query.empty() ? "没有邮件" : "未找到邮件").font(TextStyle::Headline),
                                    Label(query.empty() ? "此邮箱目前为空" : "尝试其他发件人或关键词")
                                        .secondary()
                                        .font(TextStyle::Subheadline)})
                                .spacing(8)
                                .padding(Insets(32, 22))
                                .fillWidth());
        listContent.node()->invalidateLayout();
    }
    void refreshReader() {
        readerContent.clear();
        const auto* msg = current();
        if (drafts || !msg) {
            readerContent.add(VStack({Label(drafts ? "草稿" : "未选择邮件").font(TextStyle::Title2).secondary(),
                                      Label(drafts ? "双击或点击草稿继续编辑。\n每封草稿自动保存在此设备上。"
                                                   : "选择一封邮件以查看内容。")
                                          .secondary()})
                                  .spacing(10)
                                  .padding(Insets(50, 36)));
            return;
        }
        auto copied = *msg;
        auto w = weak();
        auto details = Button("详细信息").style(ButtonStyle::Borderless).controlSize(ControlSize::Small).onClick([w] {
            if (auto ui = w.lock()) ui->m->showDetails();
        });
        auto sender =
            HStack({std::make_shared<Avatar>(msg->sender),
                    VStack({Label(msg->sender).weight(FontWeight::Semibold),
                            Label("收件人：" +
                                  (backend.demo() ? std::string("我（示例邮箱）") : backend.account().address))
                                .secondary()
                                .font(TextStyle::Subheadline)})
                        .spacing(3),
                    Spacer(), VStack({Label(msg->date).secondary().font(TextStyle::Caption), details}).spacing(2)})
                .spacing(10);
        readerContent.add(VStack({Label(msg->subject.empty() ? "（无主题）" : msg->subject)
                                      .font(TextStyle::Title3)
                                      .weight(FontWeight::Semibold),
                                  sender})
                              .spacing(16));
        if (backend.demo())
            readerContent.add(Label("示例邮件 · 不会连接真实邮箱").font(TextStyle::Caption).secondary());
        readerContent.add(Divider());
        // Body rendering: text paragraphs and inline images alternate. The parser
        // marks inline image slots with U+2028-delimited "img:<cid>" / "imgurl:<url>"
        // records; each becomes an Image widget (decoded picture) or a caption.
        {
            auto reader = this->readerContent;  // handle copy shares the node; safe to capture
            auto& images = msg->inlineImages;
            auto findCid = [&images](const std::string& cid) -> const Attachment* {
                for (auto& im : images)
                    if (im.cid == cid || im.cid.find(cid) != std::string::npos) return &im;
                return nullptr;
            };
            std::string paragraph;
            auto flushText = [&] {
                while (!paragraph.empty() && (paragraph.back() == '\n' || paragraph.back() == ' ')) paragraph.pop_back();
                size_t start = 0;
                while (start < paragraph.size() && (paragraph[start] == '\n' || paragraph[start] == ' ')) start++;
                if (start) paragraph = paragraph.substr(start);
                if (paragraph.empty()) return;
                readerContent.add(std::make_shared<TextEditor>(paragraph, false));
                paragraph.clear();
            };
            const std::string mark = "\x1e";  // record separator emitted by the MIME converter
            size_t at = 0;
            while (true) {
                auto next = msg->body.find(mark, at);
                if (next == std::string::npos) {
                    paragraph += msg->body.substr(at);
                    break;
                }
                paragraph += msg->body.substr(at, next - at);
                at = next + mark.size();
                auto end = msg->body.find(mark, at);
                if (end == std::string::npos) break;
                auto record = msg->body.substr(at, end - at);
                at = end + mark.size();
                if (record.starts_with("img:")) {
                    auto cid = record.substr(4);
                    const Attachment* found = findCid(cid);
                    if (found) {
                        flushText();
                        auto data = std::span<const std::uint8_t>(
                            reinterpret_cast<const std::uint8_t*>(found->data.data()), found->data.size());
                        if (auto decoded = mayo::ImageData::decode(data)) {
                            // Large images fit the reader width; Mayo scales before upload.
                            auto scaled = decoded->scaledToFit(640);
                            readerContent.add(mayo::Image(scaled));
                            readerContent.node()->invalidateLayout();
                            continue;
                        }
                    }
                    flushText();
                    readerContent.add(Label("[内嵌图片：" + cid + "]").secondary());
                    continue;
                }
                if (record.starts_with("imgurl:")) {
                    // Remote image: download once, cache on disk, then show inline
                    // like an embedded one. A caption stands in while loading.
                    flushText();
                    auto pipe = record.find("|alt:");
                    auto url = pipe == record.npos ? record.substr(7) : record.substr(7, pipe - 7);
                    auto alt = pipe == record.npos ? std::string("图片") : record.substr(pipe + 5);
                    auto caption = std::make_shared<mayo::Label>("正在加载图片… " + alt);
                    reader.add(*caption);
                    auto& uiApp = this->app;
                    auto directory = this->backend.settingsDirectory();
                    remote::fetch(
                        directory, url,
                        [&uiApp](std::function<void()> work) { uiApp.post(std::move(work)); },
                        [reader, caption, alt](std::vector<unsigned char> bytes, const std::string& error) mutable {
                            if (bytes.empty()) {
                                caption->setText((error.empty() ? "图片加载失败" : error) + " " + alt);
                                return;
                            }
                            auto decoded =
                                mayo::ImageData::decode(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
                            if (!decoded) {
                                caption->setText("图片格式不受支持 " + alt);
                                return;
                            }
                            reader.remove(mayo::Element(*caption));
                            reader.add(mayo::Image(decoded->scaledToFit(640)));
                        });
                    continue;
                }
                paragraph += record;
            }
            flushText();
            if (msg->body.empty()) readerContent.add(Label("（此邮件没有正文）").secondary());
        }
        if (!msg->attachments.empty()) {
            readerContent.add(Divider());
            readerContent.add(
                Label(std::to_string(msg->attachments.size()) + " 个附件").secondary().font(TextStyle::Subheadline));
            for (auto& att : msg->attachments) {
                auto name = att.name.empty() ? "attachment" : att.name;
                auto save = Button(Icon::ArrowDownCircle, name).onClick([w, attachment = att] {
                    if (auto ui = w.lock()) ui->m->saveAttachment(attachment);
                });
                readerContent.add(save);
            }
        }
        auto reply = Button(Icon::ArrowLeft, "回复").style(ButtonStyle::Borderless).onClick([w] {
            if (auto ui = w.lock()) ui->compose(nullptr, true);
        });
        auto read = Button(msg->unread ? "标为已读" : "标为未读")
                        .style(ButtonStyle::Borderless)
                        .onClick([w, id = msg->id, unread = msg->unread] {
                            if (auto ui = w.lock()) {
                                if (ui->m->backend.busy()) return;
                                ui->m->backend.setRead(id, unread, ui->m->done());
                                ui->refresh();
                            }
                        });
        read.setDisabled(!backend.connected() && !backend.demo());
        readerContent.add(HStack({reply, read, Spacer()}).padding(Insets(12, 0, 0, 0)));
        readerContent.node()->invalidateLayout();
    }
    void showDetails() {
        auto* msg = current();
        if (!msg) return;
        auto content =
            VStack({Label("邮件详细信息").font(TextStyle::Headline), Label("发件人：" + msg->sender),
                    Label(msg->address).secondary(),
                    Label("收件人：" + (backend.demo() ? std::string("本地示例") : backend.account().address)),
                    Label("日期：" + msg->date).secondary(), Label("主题：" + msg->subject)})
                .spacing(8)
                .padding(Insets(14))
                .width(340);
        infoPopover = std::make_shared<Popover>(content);
        infoPopover->show(replyButton);
    }
    void saveAttachment(const Attachment& att) {
        auto w = weak();
        app.openFolder(
            [w, att](std::vector<std::string> paths) {
                auto ui = w.lock();
                if (!ui || paths.empty()) return;
                std::string safe = std::filesystem::path(att.name).filename().string();
                if (safe.empty() || safe == "." || safe == "..") safe = "attachment";
                auto target = std::filesystem::path(paths[0]) / safe;
                std::error_code ec;
                if (std::filesystem::exists(target, ec)) {
                    ui->m->showAlert("文件已存在", "所选文件夹已有同名文件。请选择其他文件夹，避免覆盖。");
                    return;
                }
                std::ofstream out(target, std::ios::binary);
                out.write(att.data.data(), static_cast<std::streamsize>(att.data.size()));
                if (!out)
                    ui->m->error("无法保存附件，请检查目录权限。");
                else
                    ui->m->statusLabel.setText("已保存附件：" + safe);
            },
            &window);
    }
    void load(const std::string& id) {
        if (backend.busy()) return;
        if (id == "__drafts") {
            owner.showDrafts();
            return;
        }
        if (!backend.connected() && !backend.demo() && id == "__starred") {
            drafts = false;
            starredOnly = true;
            folder = backend.currentFolder();
            owner.refresh();
            return;
        }
        drafts = false;
        starredOnly = id == "__starred";
        folder = starredOnly ? "INBOX" : id;
        selected.clear();
        listScroll.scrollTo({0, 0}, false);
        backend.loadFolder(folder, done());
        owner.refresh();
    }
    void flag() {
        if (auto* msg = current(); msg && !backend.busy() && (backend.connected() || backend.demo())) {
            backend.setStar(msg->id, !msg->star, done());
            owner.refresh();
        }
    }
    void moveMessage(bool trash) {
        if (backend.busy()) return;
        if (!backend.connected() && !backend.demo()) {
            showAlert("需要连接账户", "离线时可以阅读缓存与编辑草稿。连接账户后再移动邮件。");
            return;
        }
        auto* msg = current();
        if (!msg) return;
        std::string target;
        for (auto& f : backend.folders()) {
            auto name = lower(f.id + f.title);
            if ((trash && (name.find("trash") != std::string::npos || name.find("deleted") != std::string::npos ||
                           name.find("废纸") != std::string::npos)) ||
                (!trash && (name.find("archive") != std::string::npos || name.find("归档") != std::string::npos))) {
                target = f.id;
                break;
            }
        }
        if (target.empty()) {
            showAlert("找不到目标邮箱", trash ? "服务器没有提供废纸篓文件夹。可以在“更多”菜单选择移动目标。"
                                              : "服务器没有提供归档文件夹。可以在“更多”菜单选择移动目标。");
            return;
        }
        auto id = msg->id;
        selected.clear();
        backend.move(id, target, done());
        owner.refresh();
    }
    void scheduleSync() {
        auto w = weak();
        app.after(120, [w] {
            if (auto ui = w.lock()) {
                if (!ui->m->mainAlive) return;
                if (ui->m->backend.connected() && !ui->m->backend.busy()) {
                    ui->m->backend.sync([w](const std::string& error) {
                        if (auto current = w.lock()) {
                            current->m->lastSyncError = error;
                            current->refresh();
                        }
                    });
                    ui->refresh();
                }
                ui->m->scheduleSync();
            }
        });
    }
    void buildToolbar() {
        auto w = weak();
        auto toggle = tool(Icon::Sidebar, "显示或隐藏邮箱侧栏", [w] {
            if (auto ui = w.lock()) ui->m->split.toggleSidebar();
        });
        syncButton = tool(Icon::Envelope, "收取邮件 · Ctrl+Shift+M", [w] {
            if (auto ui = w.lock()) {
                if (!ui->m->backend.connected()) {
                    ui->showAccount();
                    return;
                }
                ui->m->backend.sync(ui->m->done());
                ui->refresh();
            }
        });
        composeButton = tool(Icon::Pencil, "撰写新邮件 · Ctrl+N", [w] {
            if (auto ui = w.lock()) ui->compose();
        });
        replyButton = tool(Icon::ArrowLeft, "回复 · Ctrl+R", [w] {
            if (auto ui = w.lock()) ui->compose(nullptr, true);
        });
        archiveButton = tool(Icon::Folder, "归档 · Ctrl+E", [w] {
            if (auto ui = w.lock()) ui->m->moveMessage(false);
        });
        deleteButton = tool(Icon::Trash, "移至废纸篓 · Delete", [w] {
            if (auto ui = w.lock()) ui->m->moveMessage(true);
        });
        flagButton = tool(Icon::Star, "切换星标 · Ctrl+L", [w] {
            if (auto ui = w.lock()) ui->m->flag();
        });
        accountButton = tool(Icon::PersonCircle, "账户与连接", [w] {
            if (auto ui = w.lock()) ui->showAccount();
        });
        moreMenu = Menu("", {}).icon(Icon::Ellipsis).borderless().tooltip("更多操作与外观");
        search = SearchField("搜索邮件");
        search.width(185);
        search.onChange([w](const std::string& q) {
            if (auto ui = w.lock()) {
                ui->m->query = q;
                ui->m->refreshList();
            }
        });
        auto title = VStack({mailboxTitle.font(TextStyle::Headline).lineLimit(1),
                             mailboxCount.font(TextStyle::Caption).secondary().lineLimit(1)})
                         .spacing(0)
                         .width(105);
        window.setToolbar(Toolbar({toggle, title, ToolbarGroup({syncButton, composeButton}),
                                   ToolbarGroup({replyButton, archiveButton, deleteButton}),
                                   ToolbarGroup({flagButton, moreMenu}), Spacer(), search, accountButton})
                              .spacing(8));
    }
};

MailUI::MailUI(App& app, Window& window, Backend& backend) : m(std::make_unique<Impl>(app, window, backend, *this)) {}
MailUI::~MailUI() = default;
void MailUI::build() {
    auto w = weak_from_this();
    m->window.setTitleVisible(false);
    m->buildToolbar();
    m->sidebar.selectionStyle(SidebarSelectionStyle::Neutral).onSelect([w](const std::string& id) {
        if (auto ui = w.lock()) ui->m->load(id);
    });
    auto account = Button(Icon::PersonCircle, "账户与连接").style(ButtonStyle::Borderless).onClick([w] {
        if (auto ui = w.lock()) ui->showAccount();
    });
    m->sidebar.header(VStack({account}).padding(Insets(6, 0, 6, 0)));
    m->listContent.spacing(0).padding(Insets(4, 0, 8, 0)).fillWidth();
    m->filterButton = tool(Icon::List, "仅显示未读邮件", [w] {
        if (auto ui = w.lock()) {
            ui->m->unreadOnly = !ui->m->unreadOnly;
            ui->refresh();
            ui->m->filterButton.node()->setForeground(ui->m->unreadOnly ? std::optional<Color>(theme().colors.accent)
                                                                        : std::nullopt);
        }
    });
    auto listHeader = HStack({Label("按日期排序").secondary().font(TextStyle::Subheadline), Spacer(), m->filterButton})
                          .padding(Insets(3, 17, 3, 20))
                          .height(33);
    m->listPane.add(listHeader);
    m->listPane.add(Divider());
    m->listPane.add(m->listScroll);
    m->listPane.add(Divider());
    m->listPane.add(
        m->statusLabel.font(TextStyle::Caption).secondary().lineLimit(2).padding(Insets(6, 17)).minHeight(30));
    m->listPane.spacing(0);
    m->readerContent.spacing(15).padding(Insets(24, 28, 30, 28)).fillWidth();
    m->readerPane.add(m->readerScroll);
    m->readerPane.spacing(0);
    m->columns = std::make_shared<MailColumns>(m->listPane.node(), m->readerPane.node());
    m->columns->listWidth = m->app.settings().getFloat("listWidth", 305);
    m->columns->resized = [w](float width) {
        if (auto ui = w.lock()) ui->m->app.settings().set("listWidth", width);
    };
    m->split = SplitView(m->sidebar, m->columns).sidebarWidth(190).sidebarLimits(155, 280);
    // Status belongs below the columns while preserving the SplitView's full-size titlebar contract.
    m->window.setContent(m->split);
    m->window.onClosed([w] {
        if (auto ui = w.lock()) ui->m->mainAlive = false;
    });
    m->window.onKey([w](const KeyEvent& e) {
        if (auto ui = w.lock()) return ui->onKey(e);
        return false;
    });
    m->scheduleSync();
    refresh();
}
void MailUI::refresh() {
    for (auto it = m->sessions.begin(); it != m->sessions.end();) {
        if (auto s = it->lock()) {
            s->send.setDisabled(!m->backend.connected() || m->backend.busy() || s->sending);
            for (auto& editable : s->editable)
                if (auto widget = editable.lock()) widget->setDisabled(s->sending);
            s->from.setText(m->backend.connected() ? "发件人：" + m->backend.account().address
                                                   : "发件人：尚未连接账户");
            if (m->backend.connected() && s->status.text().rfind("未连接账户", 0) == 0)
                s->status.setText("草稿自动保存在此设备");
            ++it;
        } else
            it = m->sessions.erase(it);
    }
    if (!m->mainAlive) return;
    std::vector<SidebarItem> items;
    items.push_back(SidebarItem::header("个人收藏"));
    int unread = 0;
    for (auto& msg : m->backend.messages())
        if (msg.unread) ++unread;
    items.push_back(SidebarItem("INBOX", Icon::Envelope, "收件箱").withBadge(unread ? std::to_string(unread) : ""));
    items.push_back(SidebarItem("__starred", Icon::Star, "星标邮件"));
    items.push_back(SidebarItem("__drafts", Icon::Pencil, "草稿")
                        .withBadge(m->backend.drafts().empty() ? "" : std::to_string(m->backend.drafts().size())));
    items.push_back(SidebarItem::header(m->backend.demo()                   ? "示例邮箱"
                                        : m->backend.account().name.empty() ? "邮箱"
                                                                            : m->backend.account().name));
    for (auto& f : m->backend.folders())
        if (f.id != "INBOX" && lower(f.id) != "starred") {
            Icon icon = Icon::Folder;
            auto id = lower(f.id);
            if (id.find("trash") != std::string::npos)
                icon = Icon::Trash;
            else if (id.find("sent") != std::string::npos)
                icon = Icon::Share;
            items.push_back(SidebarItem(f.id, icon, f.title).withBadge(f.unread ? std::to_string(f.unread) : ""));
        }
    items.push_back(SidebarItem::header(
        m->backend.demo() ? "示例模式 · 未连接账户"
        : m->backend.connected()
            ? (m->lastSyncError.empty() ? "账户已连接 · 每 2 分钟自动收信" : "同步失败 · 将自动重试")
            : "账户未连接"));
    m->sidebar.setItems(std::move(items));
    m->sidebar.selection(m->drafts ? "__drafts" : m->starredOnly ? "__starred" : m->folder);
    std::string title = m->drafts ? "草稿" : m->starredOnly ? "星标邮件" : "收件箱";
    for (auto& f : m->backend.folders())
        if (!m->drafts && !m->starredOnly && f.id == m->folder) title = f.title;
    m->mailboxTitle.setText(title);
    m->window.setTitle(title + " — Forma 邮件");
    auto rows = m->filtered();
    if (std::none_of(rows.begin(), rows.end(), [&](const Message& msg) { return msg.id == m->selected; }))
        m->selected = rows.empty() ? "" : rows.front().id;
    m->refreshList();
    m->refreshReader();
    bool available = m->current() && !m->drafts;
    bool canModify = available && (m->backend.connected() || m->backend.demo());
    bool busy = m->backend.busy();
    m->statusLabel.node()->setTooltip(m->lastSyncError);
    m->statusLabel.setText(busy                ? "正在与服务器通信…"
                           : m->backend.demo() ? "示例模式 · 未连接账户"
                           : m->backend.connected()
                               ? (m->lastSyncError.empty() ? "账户已连接 · 每 2 分钟自动收信" : "同步失败 · 将自动重试")
                               : (m->backend.messages().empty() ? "账户未连接" : "离线 · 显示本地缓存"));
    m->replyButton.setDisabled(!available);
    m->archiveButton.setDisabled(!canModify || busy);
    m->deleteButton.setDisabled(!canModify || busy);
    m->flagButton.setDisabled(!canModify || busy);
    m->syncButton.setDisabled(busy);
    m->filterButton.setDisabled(m->drafts);
    if (auto* msg = m->current())
        m->flagButton.node()->setForeground(msg->star ? std::optional<Color>(theme().colors.accent) : std::nullopt);
    auto w = weak_from_this();
    std::vector<MenuItem> menu;
    menu.push_back(MenuItem("标记为已读 / 未读",
                            [w] {
                                if (auto ui = w.lock()) {
                                    if (auto* msg = ui->m->current(); msg && !ui->m->backend.busy()) {
                                        ui->m->backend.setRead(msg->id, msg->unread, ui->m->done());
                                        ui->refresh();
                                    }
                                }
                            })
                       .withShortcut("Ctrl+U")
                       .disabled(!canModify || busy));
    menu.push_back(MenuItem::separator());
    for (auto& f : m->backend.folders())
        if (f.id != m->folder)
            menu.push_back(MenuItem("移至「" + f.title + "」",
                                    [w, id = f.id] {
                                        if (auto ui = w.lock())
                                            if (auto* msg = ui->m->current(); msg && !ui->m->backend.busy()) {
                                                ui->m->backend.move(msg->id, id, ui->m->done());
                                                ui->refresh();
                                            }
                                    })
                               .withIcon(Icon::Folder)
                               .disabled(!canModify || busy));
    menu.push_back(MenuItem::separator());
    menu.push_back(MenuItem("跟随系统外观", [w] {
                       if (auto ui = w.lock()) ui->m->app.setAppearanceMode(AppearanceMode::System);
                   }).setChecked(m->app.appearanceMode() == AppearanceMode::System));
    menu.push_back(MenuItem("浅色外观", [w] {
                       if (auto ui = w.lock()) ui->m->app.setAppearance(Appearance::Light);
                   }).setChecked(m->app.appearanceMode() == AppearanceMode::Light));
    menu.push_back(MenuItem("深色外观", [w] {
                       if (auto ui = w.lock()) ui->m->app.setAppearance(Appearance::Dark);
                   }).setChecked(m->app.appearanceMode() == AppearanceMode::Dark));
    menu.push_back(MenuItem::separator());
    menu.push_back(MenuItem("账户设置…", [w] {
                       if (auto ui = w.lock()) ui->showAccount();
                   }).withIcon(Icon::PersonCircle));
    if (m->drafts)
        menu.push_back(MenuItem("删除选中草稿",
                                [w] {
                                    if (auto ui = w.lock()) {
                                        ui->m->backend.removeDraft(ui->m->selected);
                                        ui->refresh();
                                    }
                                })
                           .asDestructive()
                           .disabled(m->selected.empty()));
    menu.push_back(MenuItem::separator());
    menu.push_back(MenuItem("关于 Forma 邮件", [w] {
        if (auto ui = w.lock())
            ui->m->showAlert("Forma 邮件 0.2.0 RC1",
                             "基于 Mayonnaise 的中文邮件客户端。\n当前为候选版，OAuth "
                             "服务商注册与真实账号收发尚待验收。\n源码许可：GPL-3.0。");
    }));
    m->moreMenu.setItems(std::move(menu));
}
void MailUI::setSearch(const std::string& q) {
    m->query = q;
    m->search.setText(q);
    refresh();
}
void MailUI::showDrafts() {
    m->drafts = true;
    m->selected.clear();
    m->unreadOnly = false;
    m->listScroll.scrollTo({0, 0}, false);
    refresh();
}
void MailUI::showAccount(int initialProvider) {
    if (m->accountWindow) {
        m->accountWindow->raise();
        return;
    }
    auto& window = m->app.createWindow(
        {.title = "邮件账户 — Forma 邮件", .width = 700, .height = 815, .minWidth = 625, .minHeight = 640});
    m->accountWindow = &window;
    auto w = weak_from_this();
    auto s = std::make_shared<AccountSession>(m->backend.account());
    s->window = &window;
    if (s->previous.address.empty() && s->previous.imapHost.empty()) s->provider.selected(0);
    if (initialProvider >= 0 && initialProvider <= 2) s->provider.selected(initialProvider);
    std::weak_ptr<AccountSession> ws = s;
    auto rebuild = [w, ws] {
        auto form = ws.lock();
        if (!form) return;
        form->fields.clear();
        const int provider = form->provider.selectedIndex();
        auto defaults = providerDefaults(form->previous, provider);
        if (provider < 2) {
            form->imap.setText(defaults.imapHost);
            form->smtp.setText(defaults.smtpHost);
            form->imapPort.setText(std::to_string(defaults.imapPort));
            form->smtpPort.setText(std::to_string(defaults.smtpPort));
            form->imapTLS.selected(defaults.imapImplicitTLS ? 0 : 1);
            form->smtpTLS.selected(defaults.smtpImplicitTLS ? 0 : 1);
        }
        form->fields.add(FormSection("账户", {FormRow("名称", form->name), FormRow("邮箱地址", form->address)}));
        if (provider < 2) {
            form->fields.add(FormSection(
                provider == 0 ? "Google OAuth" : "Microsoft OAuth",
                provider == 0
                    ? std::initializer_list<FormRow>{FormRow("Client ID", form->clientId),
                                                     FormRow("客户端密钥", form->clientSecret), FormRow(form->remember)}
                    : std::initializer_list<FormRow>{FormRow("Client ID", form->clientId),
                                                     FormRow("租户", form->tenant), FormRow(form->remember)},
                provider == 0 ? "需要在 Google Cloud 注册“桌面应用”OAuth 客户端。填写你的 Client "
                                "ID，客户端密钥按注册配置填写。Forma 不内置客户端 ID，也不会要求 Google 密码。"
                              : "需要在 Microsoft Entra 注册应用并启用公共客户端设备码登录。填写自己的 Client "
                                "ID；个人与组织账户通常使用 common 租户。Forma 不内置客户端 ID。"));
            form->fields.add(
                Label(provider == 0 ? "收件：imap.gmail.com · TLS 993\n发件：smtp.gmail.com · TLS 465"
                                    : "收件：outlook.office365.com · TLS 993\n发件：smtp.office365.com · STARTTLS 587")
                    .secondary()
                    .font(TextStyle::Subheadline));
            form->fields.add(Button(provider == 0 ? "打开 Google Cloud 注册入口" : "打开 Microsoft Entra 注册入口")
                                 .style(ButtonStyle::Borderless)
                                 .onClick([ws] {
                                     if (auto current = ws.lock()) {
                                         auto url = current->provider.selectedIndex() == 0
                                                        ? "https://console.cloud.google.com/apis/credentials"
                                                        : "https://entra.microsoft.com/#view/"
                                                          "Microsoft_AAD_RegisteredApps/ApplicationsListBlade";
                                         if (!openAuthURL(url))
                                             current->status.setText(
                                                 "无法打开浏览器，请手动访问云平台的应用注册页面。");
                                     }
                                 }));
            form->connect.setTitle("浏览器授权并连接");
        } else {
            form->fields.add(FormSection(
                "登录", {FormRow("用户名", form->username), FormRow("密码", form->password), FormRow(form->remember)},
                "使用邮箱服务商提供的应用专用密码。未保存时密码仅用于当前会话。"));
            form->fields.add(FormSection("收件服务器 · IMAP",
                                         {FormRow("服务器", form->imap),
                                          FormRow("加密与端口", HStack({form->imapTLS, form->imapPort}).spacing(12))}));
            form->fields.add(FormSection("发件服务器 · SMTP",
                                         {FormRow("服务器", form->smtp),
                                          FormRow("加密与端口", HStack({form->smtpTLS, form->smtpPort}).spacing(12))}));
            form->connect.setTitle("连接账户");
        }
        form->fields.node()->invalidateLayout();
    };
    s->provider.onChange([ws, rebuild](int) {
        if (auto form = ws.lock()) {
            if (form->authorizing) return;
            form->status.setText("");
            form->authSection.setHidden(true);
            rebuild();
        }
    });
    s->imapTLS.onChange([ws](int index) {
        if (auto form = ws.lock()) form->imapPort.setText(index == 0 ? "993" : "143");
    });
    s->smtpTLS.onChange([ws](int index) {
        if (auto form = ws.lock()) form->smtpPort.setText(index == 0 ? "465" : "587");
    });
    auto start = [w, ws](bool saved) {
        auto form = ws.lock();
        auto ui = w.lock();
        if (!form || !ui) return;
        if (ui->m->backend.busy()) {
            form->status.setText("正在处理其他操作，请稍后重试。");
            return;
        }
        Account a = providerDefaults(Account{}, form->provider.selectedIndex());
        a.name = form->name.value();
        a.address = form->address.value();
        a.username = form->username.value().empty() ? a.address : form->username.value();
        a.rememberPassword = form->remember.on();
        size_t count = 0;
        if (!validRecipients(a.address, count) || count != 1 ||
            a.address.find_first_of(" <>,;\t") != std::string::npos) {
            form->status.setText("请填写一个有效的邮箱地址。");
            return;
        }
        if (a.provider != "password") {
            a.username = a.address;
            a.clientId = form->clientId.value();
            a.clientSecret = form->clientSecret.value();
            a.tenant = form->tenant.value().empty() ? "common" : form->tenant.value();
            if (a.clientId.empty()) {
                form->status.setText("请先在 Google Cloud / Microsoft Entra 注册 OAuth 应用，并填写自己的 Client ID。");
                return;
            }
        } else {
            a.password = form->password.value();
            a.imapHost = form->imap.value();
            a.smtpHost = form->smtp.value();
            a.imapImplicitTLS = form->imapTLS.selectedIndex() == 0;
            a.smtpImplicitTLS = form->smtpTLS.selectedIndex() == 0;
            try {
                size_t used = 0;
                a.imapPort = std::stoi(form->imapPort.value(), &used);
                if (used != form->imapPort.value().size() || a.imapPort < 1 || a.imapPort > 65535)
                    throw std::invalid_argument("port");
                a.smtpPort = std::stoi(form->smtpPort.value(), &used);
                if (used != form->smtpPort.value().size() || a.smtpPort < 1 || a.smtpPort > 65535)
                    throw std::invalid_argument("port");
            } catch (...) {
                form->status.setText("端口必须是 1 到 65535 之间的整数。");
                return;
            }
            if (a.imapHost.empty() || a.smtpHost.empty()) {
                form->status.setText("请填写 IMAP 和 SMTP 服务器地址。");
                return;
            }
        }
        form->spinner.setHidden(false);
        form->connect.setDisabled(true);
        form->provider.setDisabled(true);
        form->status.setText(a.provider == "password" || saved ? "正在安全连接服务器…" : "正在准备安全授权…");
        form->authorizing = a.provider != "password" && !saved;
        form->cancel.setHidden(!form->authorizing);
        form->cancel.setDisabled(false);
        form->fields.setDisabled(true);
        auto complete = [w, ws](const std::string& error) {
            auto completed = ws.lock();
            auto owner = w.lock();
            if (!owner) return;
            if (completed) {
                completed->spinner.setHidden(true);
                completed->connect.setDisabled(false);
                completed->provider.setDisabled(false);
                completed->authorizing = false;
                completed->cancel.setHidden(true);
                completed->authSection.setHidden(true);
                completed->fields.setDisabled(false);
                completed->password.setText("");
                completed->clientSecret.setText("");
            }
            if (error.empty()) {
                owner->m->lastSyncError.clear();
                owner->m->folder = owner->m->backend.currentFolder();
                owner->m->drafts = false;
                owner->m->starredOnly = false;
                owner->m->selected.clear();
            }
            owner->refresh();
            if (!completed || !completed->alive) return;
            if (error.empty()) {
                if (owner->m->backend.warning().empty())
                    completed->window->close();
                else
                    completed->status.setText(owner->m->backend.warning());
            } else
                completed->status.setText(error);
        };
        if (a.provider == "password" || saved)
            ui->m->backend.connect(std::move(a), complete);
        else
            ui->m->backend.signIn(
                std::move(a),
                [w, ws](const Backend::AuthPrompt& prompt) {
                    auto authForm = ws.lock();
                    if (!authForm || !authForm->alive) return;
                    authForm->prompt = prompt;
                    authForm->authMessage.setText(prompt.message);
                    authForm->authURL->setValue(prompt.url);
                    authForm->authCode.setText(prompt.code.empty() ? "在浏览器中完成授权" : prompt.code);
                    authForm->authSection.setHidden(false);
                    authForm->status.setText("等待你完成授权。可复制链接和设备码，或点击打开浏览器。");
                    if (auto activeUI = w.lock())
                        activeUI->m->app.after(.05, [ws] {
                            if (auto current = ws.lock(); current && current->alive)
                                current->scroll.scrollTo({0, 10000}, true);
                        });
                },
                complete);
        ui->refresh();
    };
    s->connect.style(ButtonStyle::Prominent).onClick([start] { start(false); });
    auto saved =
        Button("使用已保存登录").onClick([start] { start(true); }).tooltip("从系统钥匙环读取现有密码或刷新令牌");
    auto open = Button(Icon::Globe, "打开授权网页").onClick([ws] {
        if (auto form = ws.lock()) {
            if (!openAuthURL(form->prompt.url)) form->status.setText("无法打开浏览器。请复制授权链接后手动打开。");
        }
    });
    auto copyURL = Button(Icon::DocOnDoc, "复制链接").onClick([ws] {
        if (auto form = ws.lock())
            if (auto* h = form->authURL->host()) {
                h->setClipboardText(form->prompt.url);
                form->status.setText("授权链接已复制。");
            }
    });
    auto copyCode = Button("复制设备码").onClick([ws] {
        if (auto form = ws.lock()) {
            if (form->prompt.code.empty()) {
                form->status.setText("此授权流程不需要设备码。");
                return;
            }
            if (auto* h = form->authURL->host()) {
                h->setClipboardText(form->prompt.code);
                form->status.setText("设备码已复制。");
            }
        }
    });
    s->cancel.onClick([w, ws] {
        if (auto ui = w.lock())
            if (auto form = ws.lock()) {
                ui->m->backend.cancel();
                form->status.setText("正在取消授权…");
                form->cancel.setDisabled(true);
            }
    });
    s->authSection.add(FormSection(
        "完成账户授权",
        {FormRow(VStack({s->authMessage.secondary(), s->authCode.font(TextStyle::Title2).weight(FontWeight::Semibold),
                         s->authURL, HStack({open, copyURL, copyCode}).spacing(7)})
                     .spacing(10))}));
    s->authSection.setHidden(true);
    s->cancel.setHidden(true);
    auto demo = Button("示例邮箱").style(ButtonStyle::Borderless).onClick([w, ws] {
        if (auto ui = w.lock()) {
            if (ui->m->backend.busy()) return;
            ui->m->backend.useDemo();
            ui->m->folder = "INBOX";
            ui->m->drafts = false;
            ui->m->selected.clear();
            ui->refresh();
            if (auto form = ws.lock(); form && form->alive) form->window->close();
        }
    });
    auto disconnect = Button("断开").onClick([w, ws] {
        if (auto ui = w.lock()) {
            if (ui->m->backend.busy()) return;
            ui->m->backend.disconnect();
            if (auto form = ws.lock()) form->status.setText("账户已断开；草稿仍保留在此设备。");
            ui->refresh();
        }
    });
    disconnect.setDisabled(!m->backend.connected());
    s->status.secondary().font(TextStyle::Subheadline);
    s->fields.spacing(20);
    s->content.spacing(17).padding(Insets(22, 28)).fillWidth();
    s->content.add(Label("邮件账户").font(TextStyle::Title2).weight(FontWeight::Semibold));
    s->content.add(Label("选择邮箱服务，以安全方式登录。").secondary());
    s->content.add(FormSection({FormRow("邮箱服务", s->provider)}));
    s->content.add(s->fields);
    s->content.add(s->authSection);
    s->spinner.frame(16, 16).hidden();
    s->content.add(HStack({s->spinner, s->status}).spacing(8));
    s->content.add(HStack({demo, disconnect, Spacer(), saved, s->connect}).spacing(7));
    s->content.add(HStack({Spacer(), s->cancel}));
    rebuild();
    window.setContent(s->scroll);
    window.onClosed([w, s] {
        s->alive = false;
        if (auto ui = w.lock()) {
            if (s->authorizing) ui->m->backend.cancel();
            ui->m->accountWindow = nullptr;
            ui->refresh();
        }
    });
    window.onKey([&window](const KeyEvent& e) {
        if (e.key == Key::Escape || (e.primary() && e.key == Key::W)) {
            window.close();
            return true;
        }
        return false;
    });
}
void MailUI::compose(const Draft* existing, bool reply) {
    if (reply && !m->current()) return;
    if (existing) {
        for (auto& weak : m->sessions) {
            if (auto open = weak.lock(); open && open->alive && open->draft.id == existing->id) {
                open->window->raise();
                return;
            }
        }
    }

    auto session = std::make_shared<ComposeSession>();
    if (existing)
        session->draft = *existing;
    else if (reply) {
        const auto& msg = *m->current();
        session->draft.to = msg.address;
        session->draft.subject = msg.subject.rfind("Re:", 0) == 0 ? msg.subject : "Re: " + msg.subject;
        session->draft.body = "\n\n—— " + msg.sender + " 写道 ——\n" + msg.body;
    }
    m->saveSession(session);
    auto& window = m->app.createWindow(
        {.title = session->draft.subject.empty() ? "新邮件 — Forma 邮件" : session->draft.subject + " — Forma 邮件",
         .width = 760,
         .height = 730,
         .minWidth = 560,
         .minHeight = 440});
    session->window = &window;
    auto w = weak_from_this();
    m->sessions.push_back(session);
    auto to = TextField("多个邮箱用逗号或分号分隔").text(session->draft.to).style(TextFieldStyle::Plain);
    auto cc = TextField("抄送地址（可选）").text(session->draft.cc).style(TextFieldStyle::Plain);
    auto bcc = TextField("密送地址（可选，对其他收件人隐藏）").text(session->draft.bcc).style(TextFieldStyle::Plain);
    auto subject = TextField("主题").text(session->draft.subject).style(TextFieldStyle::Plain);
    session->editor = std::make_shared<TextEditor>(session->draft.body, true);
    auto save = [w, session] {
        if (auto ui = w.lock()) {
            if (session->sent) return;
            ui->m->saveSession(session);
        }
    };
    to.onChange([session, w](const std::string& value) {
        session->draft.to = value;
        if (auto ui = w.lock()) ui->m->queueSave(session);
    });
    cc.onChange([session, w](const std::string& value) {
        session->draft.cc = value;
        if (auto ui = w.lock()) ui->m->queueSave(session);
    });
    bcc.onChange([session, w](const std::string& value) {
        session->draft.bcc = value;
        if (auto ui = w.lock()) ui->m->queueSave(session);
    });
    subject.onChange([session, w](const std::string& value) {
        session->draft.subject = value;
        if (session->alive) session->window->setTitle(value.empty() ? "新邮件 — Forma 邮件" : value + " — Forma 邮件");
        if (auto ui = w.lock()) ui->m->queueSave(session);
    });
    // The editor's change callback owns only a weak session, avoiding an editor-session cycle.
    std::weak_ptr<ComposeSession> weakSession = session;
    session->editor->setOnChange([w, weakSession](const std::string& value) {
        auto s = weakSession.lock();
        if (!s) return;
        s->draft.body = value;
        if (auto ui = w.lock()) {
            ui->m->queueSave(s);
        }
    });
    session->status.setText(!session->lastSaveError.empty() ? "草稿保存失败：" + session->lastSaveError
                            : m->backend.connected()        ? "草稿自动保存在此设备"
                                                            : "未连接账户 · 你仍可以编写和保存草稿");
    session->status.font(TextStyle::Caption).secondary().lineLimit(2);
    auto sendNow = [w, weakSession] {
        auto sendingSession = weakSession.lock();
        auto ui = w.lock();
        if (!ui || !sendingSession || sendingSession->sending) return;
        if (!ui->m->backend.connected()) {
            ui->showAccount();
            return;
        }
        if (ui->m->backend.busy()) {
            sendingSession->status.setText("服务器正在处理其他操作，请稍后重试。");
            return;
        }
        ui->m->saveSession(sendingSession);
        sendingSession->sending = true;
        sendingSession->send.setDisabled(true);
        sendingSession->status.setText("正在发送…");
        const auto& d = sendingSession->draft;
        Outgoing outgoing;
        outgoing.to = d.to;
        outgoing.subject = d.subject;
        outgoing.body = d.body;
        outgoing.cc = d.cc;
        outgoing.bcc = d.bcc;
        outgoing.attachments = d.attachments;
        ui->m->backend.send(std::move(outgoing), [w, sendingSession](const std::string& error) {
            sendingSession->sending = false;
            if (auto sentUI = w.lock()) {
                if (error.empty()) {
                    sendingSession->sent = true;
                    sentUI->m->backend.removeDraft(sendingSession->draft.id);
                    sentUI->refresh();
                    if (sendingSession->alive) sendingSession->window->close();
                } else {
                    sendingSession->status.setText("发送失败：" + error);
                    sendingSession->send.setDisabled(false);
                    sentUI->refresh();
                }
            }
        });
        ui->refresh();
    };
    auto sendAction = [w, weakSession, sendNow] {
        auto s = weakSession.lock();
        auto ui = w.lock();
        if (!s || !ui || s->sending) return;
        size_t count = 0;
        if (!validRecipients(s->draft.to, count) || !validRecipients(s->draft.cc, count) ||
            !validRecipients(s->draft.bcc, count)) {
            s->status.setText("收件地址格式有误。请使用完整邮箱地址，多个地址用逗号或分号分隔。");
            return;
        }
        if (!count) {
            s->status.setText("请至少填写一个收件人、抄送或密送地址。");
            return;
        }
        if (s->draft.subject.find_first_not_of(" \t\r\n") == std::string::npos) {
            auto cancel = Button("返回编辑").onClick([w] {
                if (auto owner = w.lock())
                    if (owner->m->panel) owner->m->panel->dismiss();
            });
            auto send = Button("仍然发送").style(ButtonStyle::Prominent).onClick([w, sendNow] {
                if (auto owner = w.lock())
                    if (owner->m->panel) owner->m->panel->dismiss();
                sendNow();
            });
            ui->m->panel = std::make_shared<CenteredPanel>(
                VStack({Label("不填写主题就发送？").font(TextStyle::Headline),
                        Label("这封邮件将发送给 " + std::to_string(count) + " 位收件人。空白主题可能让邮件不易辨认。")
                            .secondary(),
                        HStack({Spacer(), cancel, send}).spacing(8)})
                    .spacing(14)
                    .width(380));
            if (auto* host = s->editor->host()) ui->m->panel->present(*host);
            return;
        }
        sendNow();
    };
    session->refreshAttachments = [w, weakSession] {
        auto s = weakSession.lock();
        if (!s) return;
        s->attachmentRows.clear();
        for (size_t i = 0; i < s->draft.attachments.size(); ++i) {
            auto& attachment = s->draft.attachments[i];
            auto remove = tool(Icon::Xmark, "移除附件", [w, weakSession, i] {
                auto current = weakSession.lock();
                auto ui = w.lock();
                if (!current || !ui || i >= current->draft.attachments.size()) return;
                current->draft.attachments.erase(current->draft.attachments.begin() + static_cast<std::ptrdiff_t>(i));
                ui->m->saveSession(current);
                current->refreshAttachments();
                if (ui->m->drafts) ui->refresh();
            });
            s->attachmentRows.add(
                HStack({Label("附件").secondary().font(TextStyle::Caption).width(32),
                        Label(attachment.name).lineLimit(1), Spacer(),
                        Label(sizeText(attachment.data.size())).secondary().font(TextStyle::Caption), remove})
                    .spacing(8));
        }
        s->attachmentRows.setHidden(s->draft.attachments.empty());
        s->attachmentScroll.setHidden(s->draft.attachments.empty());
        s->attachmentScroll.height(std::min(132.0f, static_cast<float>(s->draft.attachments.size()) * 36 + 10));
        s->attachmentRows.node()->invalidateLayout();
    };
    auto attach = tool(Icon::Import, "添加附件", [w, weakSession] {
        auto s = weakSession.lock();
        auto ui = w.lock();
        if (!s || !ui || s->sending) return;
        ui->m->app.openFiles(
            {}, true,
            [w, weakSession](std::vector<std::string> paths) {
                auto current = weakSession.lock();
                auto owner = w.lock();
                if (!current || !owner || !current->alive || current->sending || paths.empty()) return;
                size_t total = 0;
                for (auto& a : current->draft.attachments) total += a.data.size();
                std::string error;
                for (auto& path : paths) {
                    std::error_code ec;
                    auto bytes = std::filesystem::file_size(path, ec);
                    if (ec || !std::filesystem::is_regular_file(path, ec)) {
                        error = "无法读取某个附件，请检查文件权限。";
                        continue;
                    }
                    if (bytes > 15 * 1024 * 1024 || total + bytes > 20 * 1024 * 1024) {
                        error = "附件上限为单个 15 MB、总计 20 MB。部分文件未添加。";
                        continue;
                    }
                    Attachment attachment;
                    attachment.name = std::filesystem::path(path).filename().string();
                    attachment.mime = "application/octet-stream";
                    attachment.data.resize(static_cast<size_t>(bytes));
                    std::ifstream file(path, std::ios::binary);
                    file.read(attachment.data.data(), static_cast<std::streamsize>(bytes));
                    if (!file) {
                        error = "无法完整读取某个附件。";
                        continue;
                    }
                    total += attachment.data.size();
                    current->draft.attachments.push_back(std::move(attachment));
                }
                bool saved = owner->m->saveSession(current);
                current->refreshAttachments();
                if (saved && !error.empty()) current->status.setText(error);
                if (owner->m->drafts) owner->refresh();
            },
            s->window);
    });
    session->editable = {to.node(),      cc.node(),       bcc.node(),
                         subject.node(), session->editor, session->attachmentRows.node(),
                         attach.node()};
    session->attachmentRows.spacing(3).padding(Insets(5, 24));
    session->refreshAttachments();
    session->send.style(ButtonStyle::Prominent).onClick(sendAction).tooltip("发送邮件 · Ctrl+Enter");
    session->send.setDisabled(!m->backend.connected() || m->backend.busy());
    auto connect = tool(Icon::PersonCircle, "邮件账户", [w] {
        if (auto ui = w.lock()) ui->showAccount();
    });
    auto saveButton = tool(Icon::Document, "保存草稿 · Ctrl+S", save);
    window.setTitleVisible(false);
    window.setToolbar(Toolbar({ToolbarGroup({saveButton, attach}), Label("新邮件").font(TextStyle::Headline), Spacer(),
                               connect, session->send}));
    session->from = Label(m->backend.connected() ? "发件人：" + m->backend.account().address : "发件人：尚未连接账户")
                        .secondary()
                        .font(TextStyle::Subheadline);
    auto editorContainer = VStack({session->editor}).padding(Insets(16, 23)).fillWidth();
    window.setContent(
        VStack({VStack({HStack({Label("收件人：").secondary().width(65), to}), Divider(),
                        HStack({Label("抄送：").secondary().width(65), cc}), Divider(),
                        HStack({Label("密送：").secondary().width(65), bcc}), Divider(),
                        HStack({Label("主题：").secondary().width(65), subject}), Divider(), session->from})
                    .spacing(9)
                    .padding(Insets(14, 24)),
                Divider(), session->attachmentScroll, ScrollView(editorContainer), Divider(),
                HStack({session->status, Spacer()}).padding(Insets(7, 24))})
            .spacing(0));
    auto canClose = [w, weakSession] {
        auto current = weakSession.lock();
        auto ui = w.lock();
        if (!current || !ui || current->sent) return true;
        if (ui->m->saveSession(current)) return true;
        auto dismiss = Button("继续编辑").style(ButtonStyle::Prominent).onClick([w] {
            if (auto owner = w.lock())
                if (owner->m->panel) owner->m->panel->dismiss();
        });
        ui->m->panel = std::make_shared<CenteredPanel>(
            VStack({Label("草稿尚未保存").font(TextStyle::Headline),
                    Label("关闭已取消，内容仍保留在当前窗口。请解决保存错误后按 Ctrl+S 重试。\n\n" +
                          current->lastSaveError)
                        .secondary(),
                    HStack({Spacer(), dismiss})})
                .spacing(14)
                .width(410));
        if (auto* h = current->editor->host()) ui->m->panel->present(*h);
        return false;
    };
    window.onCloseRequested(canClose);
    window.onClosed([w, session] {
        session->alive = false;
        if (auto ui = w.lock()) {
            ui->refresh();
        }
    });
    window.onKey([save, sendAction, canClose, &window](const KeyEvent& e) {
        if (e.primary() && e.key == Key::S) {
            save();
            return true;
        }
        if (e.primary() && e.key == Key::Enter) {
            sendAction();
            return true;
        }
        if (e.primary() && e.key == Key::W) {
            if (canClose()) window.close();
            return true;
        }
        return false;
    });
    refresh();
}
bool MailUI::onKey(const KeyEvent& e) {
    if (e.primary() && e.key == Key::N) {
        compose();
        return true;
    }
    if (e.primary() && e.key == Key::F) {
        if (auto* h = m->search.node()->host()) h->setFocus(m->search.node().get(), true);
        return true;
    }
    if (e.primary() && e.key == Key::Comma) {
        showAccount();
        return true;
    }
    if (e.primary() && e.shift() && e.key == Key::M) {
        if (!m->backend.connected())
            showAccount();
        else if (!m->backend.busy()) {
            m->backend.sync(m->done());
            refresh();
        }
        return true;
    }
    if (e.primary() && e.key == Key::R) {
        compose(nullptr, true);
        return true;
    }
    if (e.primary() && e.key == Key::E) {
        m->moveMessage(false);
        return true;
    }
    if (e.primary() && e.key == Key::L) {
        m->flag();
        return true;
    }
    if (e.primary() && e.key == Key::U) {
        if (auto* msg = m->current(); msg && !m->backend.busy() && (m->backend.connected() || m->backend.demo())) {
            m->backend.setRead(msg->id, msg->unread, m->done());
            refresh();
        }
        return true;
    }
    if (e.key == Key::Delete) {
        if (m->drafts) {
            m->backend.removeDraft(m->selected);
            refresh();
        } else
            m->moveMessage(true);
        return true;
    }
    if (e.key == Key::Down || e.key == Key::Up) {
        auto rows = m->filtered();
        auto it = std::find_if(rows.begin(), rows.end(), [&](const Message& msg) { return msg.id == m->selected; });
        if (!rows.empty()) {
            int index = it == rows.end() ? 0 : static_cast<int>(it - rows.begin());
            index = std::clamp(index + (e.key == Key::Down ? 1 : -1), 0, static_cast<int>(rows.size()) - 1);
            m->select(rows[static_cast<size_t>(index)].id);
            m->listScroll.scrollTo({0, std::max(0.0f, static_cast<float>(index) * 95 - 190)}, true);
        }
        return true;
    }
    return false;
}
bool MailUI::selfTest() {
    auto check = [](bool condition, const char* what) {
        if (!condition) throw std::runtime_error(what);
    };
    const auto original = m->filtered().size();
    check(original > 0, "demo inbox");
    m->query = "no-such-message-xx";
    check(m->filtered().empty(), "search excludes unmatched");
    m->query = "设计";
    check(!m->filtered().empty() && m->filtered().size() <= original, "Chinese search");
    m->query.clear();
    TextEditor editor("你好abc");
    editor.arrange({0, 0, 250, 240});
    editor.onKeyDown({Key::End, Modifiers::Ctrl});
    editor.onKeyDown({Key::Left});
    editor.onKeyDown({Key::Backspace});
    check(editor.value() == "你好ac", "UTF-8 midtext editing");
    editor.onKeyDown({Key::Home, Modifiers::Ctrl});
    editor.onKeyDown({Key::Delete});
    check(editor.value() == "好ac", "UTF-8 delete");
    editor.onKeyDown({Key::Right, Modifiers::Shift});
    editor.insert("新");
    check(editor.value() == "新ac", "Shift selection replacement");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    check(editor.value() == "好ac", "undo");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl | Modifiers::Shift});
    check(editor.value() == "新ac", "redo");
    editor.onKeyDown({Key::A, Modifiers::Ctrl});
    editor.insert("abcdef");
    editor.onPointerDown({{1, 11}, MouseButton::Left, 1});
    editor.onPointerMove({{33, 11}, MouseButton::Left});
    editor.onPointerUp({{33, 11}, MouseButton::Left});
    check(editor.caret() != editor.anchor(), "mouse drag selection");
    editor.insert("替换");
    check(editor.value().find("替换") != std::string::npos, "drag selection replacement");
    TextEditor readonly("可选择的正文", false);
    check(!readonly.onTextInput({"x"}) && readonly.value() == "可选择的正文", "reader stays read-only");
    size_t recipients = 0;
    check(validRecipients("Alice <alice@example.com>, bob@example.org; 中文姓名 <charlie@example.net>", recipients) &&
              recipients == 3,
          "multiple named recipients");
    recipients = 0;
    check(!validRecipients("invalid-address", recipients), "reject malformed recipient");
    recipients = 0;
    check(!validRecipients("a@example.com\r\nBcc:evil@example.com", recipients), "reject recipient header injection");
    recipients = 0;
    check(!validRecipients("Alice <alice@example.com", recipients), "reject unmatched address brackets");
    recipients = 0;
    check(validRecipients("\"Last, First\" <alice@example.com>", recipients) && recipients == 1, "quoted display name");
    auto google = providerDefaults(Account{}, 0);
    check(google.provider == "google" && google.imapHost == "imap.gmail.com" && google.smtpPort == 465 &&
              google.smtpImplicitTLS,
          "Google provider defaults");
    auto microsoft = providerDefaults(Account{}, 1);
    check(microsoft.provider == "microsoft" && microsoft.smtpHost == "smtp.office365.com" &&
              microsoft.smtpPort == 587 && !microsoft.smtpImplicitTLS,
          "Microsoft provider defaults");
    Account privateAccount;
    privateAccount.password = "DO-NOT-REVEAL";
    privateAccount.accessToken = "ACCESS-TOKEN";
    privateAccount.refreshToken = "REFRESH-TOKEN";
    AccountSession fields(privateAccount);
    check(fields.password.value().empty() && fields.clientSecret.value().empty(),
          "credentials never echoed in account fields");
    Draft multi;
    multi.to = "a@example.com,b@example.net";
    multi.cc = "c@example.com";
    multi.bcc = "d@example.org";
    multi.subject = "UI persistence test";
    multi.body = "本地草稿";
    multi.attachments.push_back({"example.txt", "text/plain", "附件内容"});
    multi.id = m->backend.saveDraft(multi);
    const auto found = std::find_if(m->backend.drafts().begin(), m->backend.drafts().end(),
                                    [&](const Draft& d) { return d.id == multi.id; });
    check(found != m->backend.drafts().end() && found->cc == multi.cc && found->bcc == multi.bcc &&
              found->attachments.size() == 1 && found->attachments[0].data == "附件内容",
          "Cc Bcc and attachment draft preservation");
    m->backend.removeDraft(multi.id);
    m->query.clear();
    refresh();
    return true;
}
}  // namespace forma::mail
