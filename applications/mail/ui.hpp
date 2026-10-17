#pragma once
#include <mayonnaise/mayonnaise.hpp>
#include <memory>
#include "backend.hpp"

namespace forma::mail {
class MailUI : public std::enable_shared_from_this<MailUI> {
public:
    MailUI(mayo::App& app, mayo::Window& window, Backend& backend);
    ~MailUI();
    void build();
    void refresh();
    void showAccount(int provider = -1);
    void compose(const Draft* draft = nullptr, bool reply = false);
    bool onKey(const mayo::KeyEvent& event);
    void setSearch(const std::string& query);
    void showDrafts();
    bool selfTest();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}  // namespace forma::mail
