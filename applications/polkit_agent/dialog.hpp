#pragma once
#include <mayonnaise/mayonnaise.hpp>
#include <functional>

namespace forma::auth {
struct DialogState {
    std::string title = "身份验证";
    std::string iconName;
    bool preview = false;
    bool noticeError = false;
    std::string message;
    std::string prompt = "Password:";
    std::string notice;
    std::vector<std::string> identities;
    int selected = 0;
    bool echo = false;
    bool waiting = true;
    bool retry = false;
};

class Dialog final : public mayo::Widget {
public:
    Dialog();
    void update(DialogState state);
    float preferredHeight() const;
    void clearInput();
    void focusInput();
    void submit();
    std::function<void(std::string)> onResponse;
    std::function<void()> onCancel, onRetry;
    std::function<void(int)> onIdentity;
    mayo::Size measureContent(mayo::Size) override { return {300, preferredHeight()}; }
    void arrangeContent(const mayo::Rect& r) override;
    void draw(mayo::Painter& p) override;
private:
    DialogState state_;
    std::shared_ptr<mayo::ImageData> badge_;
    std::shared_ptr<mayo::TextFieldView> input_;
    std::shared_ptr<mayo::PickerView> identity_;
    std::shared_ptr<mayo::ButtonView> cancel_, confirm_;
};
} // namespace forma::auth
