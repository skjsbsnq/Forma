#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include "backend.hpp"
using namespace forma::mail;
void check(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}
int main() {
    auto msg = Backend::parseMessage(
        "From: =?UTF-8?B?5rWL6K+V?= <sender@example.invalid>\r\nSubject: =?UTF-8?B?5Lit5paH?=\r\nContent-Type: "
        "multipart/mixed; boundary=xyz\r\n\r\n--xyz\r\nContent-Type: text/plain; "
        "charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n5L2g5aW9\r\n--xyz\r\nContent-Type: "
        "application/octet-stream\r\nContent-Disposition: attachment; "
        "filename=\"../a.txt\"\r\nContent-Transfer-Encoding: base64\r\n\r\naGVsbG8=\r\n--xyz--\r\n",
        "42");
    check(msg.sender == "测试" && msg.subject == "中文" && msg.body == "你好", "MIME Chinese decode");
    check(msg.attachments.size() == 1 && msg.attachments[0].name == "a.txt" && msg.attachments[0].data == "hello",
          "MIME safe attachment");
    auto html = Backend::parseMessage(
        "Subject: HTML\nContent-Type: text/html; charset=utf-8\n\n<p>Hello &amp; world</p><script>bad()</script>", "1");
    check(html.body == "Hello & world", "HTML inert text fallback");
    Account a;
    a.address = "sender@example.invalid";
    auto outgoing = Backend::makeOutgoing(a, {"to@example.invalid", "中文主题", "正文"});
    check(outgoing.find("Content-Transfer-Encoding: base64") != outgoing.npos, "outgoing MIME");
    bool rejected = false;
    try {
        Backend::makeOutgoing(a, {"to@example.invalid\r\nBcc: evil@example.invalid", "a", "b"});
    } catch (...) {
        rejected = true;
    }
    check(rejected, "header injection rejected");
    std::cout << "MIME / attachment / injection tests passed\n";
    if (!std::getenv("FORMA_TEST_IMAP_PORT")) return 0;
    mayo::App app("forma-mail-tests");
    auto& window = app.createWindow({.title = "Forma local protocol tests", .width = 400, .height = 180});
    window.setContent(mayo::Label("仅连接本机 TLS 测试服务器"));
    Backend backend(app, app.settings());
    Account test;
    test.imapHost = test.smtpHost = "localhost";
    test.imapPort = std::atoi(std::getenv("FORMA_TEST_IMAP_PORT"));
    test.smtpPort = std::atoi(std::getenv("FORMA_TEST_SMTP_PORT"));
    test.username = test.address = "test@example.invalid";
    test.password = "local-test-only";
    if (std::getenv("FORMA_TEST_XOAUTH")) {
        test.provider = "google";
        test.clientId = "local-test-client";
        test.accessToken = "local-access";
        test.tokenExpires = std::time(nullptr) + 3600;
        test.password.clear();
    }
    bool ok = false;
    std::string error;
    auto fail = [&](const std::string& e) {
        if (!e.empty()) {
            error = e;
            app.quit();
            return true;
        }
        return false;
    };
    backend.connect(test, [&](const std::string& e) {
        if (fail(e)) return;
        check(backend.connected(), "connected");
        check(backend.messages().size() == 1 && backend.messages()[0].subject == "中文", "network fetch MIME");
        backend.setRead("1", true, [&](const std::string& e) {
            if (fail(e)) return;
            check(!backend.messages()[0].unread, "remote seen applied");
            backend.setStar("1", true, [&](const std::string& e) {
                if (fail(e)) return;
                check(backend.messages()[0].star, "remote flag applied");
                backend.move("1", "Trash", [&](const std::string& e) {
                    if (fail(e)) return;
                    check(backend.messages().empty(), "remote move applied");
                    backend.send({"recipient@example.invalid", "本地测试", "不连接外部服务器"},
                                 [&](const std::string& e) {
                                     if (fail(e)) return;
                                     auto id = backend.saveDraft({{}, "a@example.invalid", "草稿", "多行\n中文"});
                                     check(!id.empty(), "save draft");
                                     mayo::Settings reload(app.settings().appName());
                                     Backend second(app, reload);
                                     check(second.drafts().size() == 1 && second.drafts()[0].body == "多行\n中文",
                                           "draft persistence");
                                     ok = true;
                                     app.quit();
                                 });
                });
            });
        });
    });
    app.after(30, [&] {
        error = "local test timeout";
        app.quit();
    });
    app.run();
    if (!ok) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "Local verified TLS IMAP/SMTP operations and draft reload passed\n";
}
