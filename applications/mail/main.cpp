#include <cstdlib>
#include <iostream>
#include <mayonnaise/mayonnaise.hpp>
#include <memory>
#include <string>
#include "backend.hpp"
#include "ui.hpp"

int main() {
    mayo::App app("forma-mail", MAYO_DESKTOP_ID);
    forma::mail::Backend backend(app, app.settings());
    auto& window = app.createWindow({.title = "Forma 邮件",
                                     .width = 1190,
                                     .height = 740,
                                     .minWidth = 1020,
                                     .minHeight = 540,
                                     .autosaveName = "main"});
    auto ui = std::make_shared<forma::mail::MailUI>(app, window, backend);
    ui->build();
    if (std::getenv("FORMA_MAIL_UI_TEST")) {
        try {
            ui->selfTest();
            std::cout << "Forma Mail UI: 20 checks passed\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << "\n";
            return 1;
        }
    }
    // Screenshots only: never connect to servers or send messages automatically.
    if (const char* demo = std::getenv("FORMA_MAIL_DEMO")) {
        std::string mode(demo);
        if (mode == "search")
            ui->setSearch("设计");
        else if (mode == "compose")
            ui->compose();
        else if (mode == "account")
            ui->showAccount();
        else if (mode == "gmail")
            ui->showAccount(0);
        else if (mode == "outlook")
            ui->showAccount(1);
        else if (mode == "attachments") {
            forma::mail::Draft example;
            example.to = "林予安 <yuan@example.invalid>, xinghe@example.invalid";
            example.cc = "design@example.invalid";
            example.bcc = "archive@example.invalid";
            example.subject = "周末的阅读与设计清单";
            example.body =
                "你好，\n\n整理了一份周末阅读清单，随信附上。\n\n这是一份本地撰写预览，没有发送给任何人。\n\n祝好";
            example.attachments = {{"秋日阅读清单.txt", "text/plain", "本地附件预览"},
                                   {"设计笔记.md", "text/markdown", "# 本周设计笔记"}};
            ui->compose(&example);
        } else if (mode == "drafts")
            ui->showDrafts();
    }
    return app.run();
}
