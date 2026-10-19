#include "dialog.hpp"
#include <mayonnaise/font.hpp>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <string_view>

namespace forma::auth {
using namespace mayo;
namespace {
constexpr float kTextWidth = 252;
Font body() { return {14, FontWeight::Regular, 18}; }
Font heading() { return {15, FontWeight::Bold, 19}; }
// Wrap on Unicode scalar boundaries, retaining explicit PAM line breaks. Long words are split.
std::vector<std::string> lines(std::string_view text, const Font& font, float width) {
    std::vector<std::string> out;
    std::string line;
    float used = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t start = i;
        unsigned char c = static_cast<unsigned char>(text[i++]);
        char32_t cp = c;
        int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
        if (n) cp = c & ((1 << (6 - n)) - 1);
        while (n-- && i < text.size()) cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 63);
        if (cp == '\r') continue;
        if (cp == '\n') { out.push_back(line); line.clear(); used = 0; continue; }
        const float advance = glyphAdvance(cp, font);
        if (used + advance > width && !line.empty()) {
            // Prefer the final space; recompute only the short carried tail.
            const auto space = line.find_last_of(' ');
            if (space != std::string::npos && space > line.size() / 2) {
                out.push_back(line.substr(0, space));
                line = line.substr(space + 1);
                used = 0;
                for (unsigned char ch : line) if (ch < 128) used += glyphAdvance(ch, font);
                // For non-ASCII tails, conservatively start a fresh line.
                if (std::any_of(line.begin(), line.end(), [](unsigned char ch) { return ch >= 128; })) {
                    out.push_back(line); line.clear(); used = 0;
                }
            } else { out.push_back(line); line.clear(); used = 0; }
        }
        if (line.empty() && cp == ' ') continue;
        line.append(text.substr(start, i - start)); used += advance;
    }
    if (!line.empty() || out.empty()) out.push_back(line);
    return out;
}
float textHeight(const std::string& text, const Font& f) {
    return static_cast<float>(lines(text, f, kTextWidth).size()) * f.lineHeight;
}
void drawLines(Painter& p, const std::string& text, float x, float y, const Font& f, Color c) {
    for (const auto& line : lines(text, f, kTextWidth)) {
        p.drawText(line, {x + 24, y, kTextWidth, f.lineHeight}, f, c);
        y += f.lineHeight;
    }
}
std::string instruction(const DialogState& s) {
    if (s.retry) return s.preview ? "Authentication failed. Try again." : "验证失败，请重试。";
    if (!s.waiting) return s.preview ? "Authenticating…" : "正在验证…";
    if (s.prompt == "Password:" || s.prompt == "Password: " || s.prompt.empty())
        return s.preview ? "Enter your password to allow this." : "输入密码以允许此次操作。";
    return s.prompt;
}
class AuthField final : public TextFieldView {
public:
    AuthField() : TextFieldView("Password") { setStyle(TextFieldStyle::Plain); setSecure(true); }
    void draw(Painter& p) override {
        const auto& t = theme();
        p.fillRoundedRect(frame(), 7, t.isDark() ? Color::white(.10f) : Color::black(.075f));
        TextFieldView::draw(p);
        if (isFocused() && host() && host()->isKeyWindow())
            p.strokeRoundedRect(frame().outset(2), 9, Color::hex(0x80aafa), 3.5f);
    }
};
class AuthButton final : public ButtonView {
public:
    AuthButton(std::string title, bool primary) : ButtonView(std::move(title)), primary_(primary) {}
    void draw(Painter& p) override {
        const bool dark = theme().isDark();
        Color fill = primary_ ? Color::hex(0x007aff) : dark ? Color::white(.13f) : Color::black(.075f);
        fill = fill.lighter(isPressed() ? -.12f : isHovered() ? .04f : 0);
        p.fillRoundedRect(frame(), frame().height / 2, fill.multipliedAlpha(isEnabled() ? 1 : .45f));
        p.drawText(title(), frame(), body(), primary_ ? Color::white(isEnabled() ? 1 : .55f) : theme().colors.label,
                   TextAlign::Center);
    }
private:
    bool primary_;
};
}
Dialog::Dialog() {
    input_ = std::make_shared<AuthField>();
    identity_ = std::make_shared<PickerView>(std::vector<std::string>{"Administrator"}, 0);
    cancel_ = std::make_shared<AuthButton>("Cancel", false);
    confirm_ = std::make_shared<AuthButton>("OK", true);
    addChild(identity_); addChild(input_); addChild(cancel_); addChild(confirm_);
    cancel_->setOnClick([this] { clearInput(); if (onCancel) onCancel(); });
    confirm_->setOnClick([this] { submit(); });
    input_->setOnSubmit([this](const std::string&) { submit(); });
    identity_->setOnChange([this](int i) { clearInput(); if (onIdentity) onIdentity(i); });
}
void Dialog::update(DialogState state) {
    const bool changedPrompt = state.prompt != state_.prompt || state.echo != state_.echo || state.selected != state_.selected;
    if (state.iconName != state_.iconName) {
        badge_.reset();
        if (!state.iconName.empty() && std::all_of(state.iconName.begin(), state.iconName.end(),
                [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; })) {
            for (const char* theme : {"hicolor", "breeze", "Adwaita"}) {
                for (const char* size : {"64x64", "48x48", "32x32", "256x256"}) {
                    const std::string path = std::string("/usr/share/icons/") + theme + "/" + size + "/apps/" + state.iconName + ".png";
                    if (access(path.c_str(), R_OK) == 0) badge_ = ImageData::load(path, 64);
                    if (badge_) break;
                }
                if (badge_) break;
            }
        }
    }
    state_ = std::move(state);
    if (changedPrompt || !state_.waiting) clearInput();
    input_->setSecure(!state_.echo);
    input_->setPlaceholder(state_.preview ? (state_.echo ? "Response" : "Password") : (state_.echo ? "回答" : "密码"));
    input_->setDisabled(!state_.waiting || state_.retry);
    identity_->setOptions(state_.identities.empty() ? std::vector<std::string>{"Administrator"} : state_.identities);
    identity_->setSelected(state_.selected);
    identity_->setDisabled(state_.identities.size() < 2);
    cancel_->setTitle(state_.preview ? "Cancel" : "取消");
    confirm_->setTitle(state_.preview ? (state_.retry ? "Try Again" : "OK") : (state_.retry ? "重试" : "好"));
    confirm_->setDisabled(!state_.waiting && !state_.retry);
    invalidateLayout();
}
float Dialog::preferredHeight() const {
    return 116 + textHeight(state_.title, heading()) + 20 + textHeight(state_.message, body()) + 18 +
           textHeight(instruction(state_), body()) + (state_.notice.empty() ? 0 : textHeight(state_.notice, body()) + 10) +
           18 + 30 + 8 + 30 + 18 + 32 + 18;
}
void Dialog::arrangeContent(const Rect& r) {
    const float y = r.y + preferredHeight() - 18 - 32 - 18 - 30 - 8 - 30;
    identity_->arrange({r.x + 18, y, r.width - 36, 30});
    input_->arrange({r.x + 18, y + 38, r.width - 36, 30});
    const float width = (r.width - 44) / 2;
    cancel_->arrange({r.x + 18, r.y + preferredHeight() - 50, width, 32});
    confirm_->arrange({r.x + 26 + width, r.y + preferredHeight() - 50, width, 32});
}
void Dialog::clearInput() { input_->clearSensitive(); }
void Dialog::focusInput() { if (host() && state_.waiting && !state_.retry) host()->setFocus(input_.get(), false); }
void Dialog::submit() {
    if (state_.retry) { clearInput(); if (onRetry) onRetry(); return; }
    if (!state_.waiting) return;
    std::string response = input_->text();
    clearInput();
    if (onResponse) onResponse(std::move(response));
    volatile char* data = response.data();
    for (size_t i = 0; i < response.size(); ++i) data[i] = 0;
}
void Dialog::draw(Painter& p) {
    const auto& t = theme();
    const float x = frame().x, top = frame().y;
    // Lock is locally drawn original artwork; no Apple asset or SF font is distributed.
    p.drawShadow({x + 35, top + 53, 51, 38}, 3, Color::black(.15f), 3, {0, 1});
    p.drawArc({x + 61, top + 46}, 17, 3.1415926f, 3.1415926f, Color::hex(0xa2acb2), 8);
    p.drawArc({x + 61, top + 46}, 17, 3.1415926f, 3.1415926f, Color::hex(0xe1e6e8), 4);
    p.drawLine({x + 44, top + 46}, {x + 44, top + 58}, Color::hex(0xc6cdd1), 8);
    p.drawLine({x + 78, top + 46}, {x + 78, top + 58}, Color::hex(0xc6cdd1), 8);
    p.fillVerticalGradient({x + 34, top + 54, 54, 39}, 2, Color::hex(0xffcd57), Color::hex(0xe99820));
    for (int i = 0; i < 12; ++i)
        p.drawLine({x + 36, top + 57.f + 2.8f * static_cast<float>(i)}, {x + 86, top + 57.f + 2.8f * static_cast<float>(i)}, Color::white(.08f), .5f);
    p.fillRoundedRect({x + 64, top + 66, 32, 32}, 9, Color::hex(0xf2f2f2), Color::black(.12f), .6f);
    if (badge_) p.drawImage({x + 66, top + 68, 28, 28}, *badge_, 7);
    else {
        p.fillCircle({x + 80, top + 82}, 11, Color::hex(0x858a8e));
        p.drawIcon(state_.preview ? Icon::ArrowRight : Icon::Shield, {x + 72, top + 74, 16, 16}, Color::white(), 1.5f);
    }
    float y = top + 116;
    drawLines(p, state_.title, x, y, heading(), t.colors.label);
    y += textHeight(state_.title, heading()) + 20;
    drawLines(p, state_.message, x, y, body(), t.colors.label);
    y += textHeight(state_.message, body()) + 18;
    drawLines(p, instruction(state_), x, y, body(), t.colors.label);
    y += textHeight(instruction(state_), body()) + 10;
    if (!state_.notice.empty()) drawLines(p, state_.notice, x, y, body(), state_.noticeError ? t.colors.destructive : t.colors.secondaryLabel);
    // Single identity is a read-only filled field, matching the reference's username row.
    if (state_.identities.size() < 2) {
        p.fillRoundedRect(identity_->frame(), 7, t.isDark() ? Color::white(.10f) : Color::black(.075f));
        p.pushClip(identity_->frame().inset(6));
        p.drawText(state_.identities.empty() ? "Administrator" : state_.identities.front(), identity_->frame().inset(7), body(), t.colors.label);
        p.popClip();
        input_->paint(p); cancel_->paint(p); confirm_->paint(p);
    } else paintChildren(p);
}
} // namespace forma::auth
