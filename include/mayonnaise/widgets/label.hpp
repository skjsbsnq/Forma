#pragma once

#include <string>
#include <vector>

#include "../painter.hpp"
#include "../widget.hpp"

namespace mayo {

/// Semantic text colours that follow the appearance.
enum class LabelColor : std::uint8_t { Primary, Secondary, Tertiary, Accent, Destructive, OnAccent };

class LabelView : public Widget {
public:
    explicit LabelView(std::string text);

    void setText(std::string text);
    /// Editable/user-provided content must bypass display-only UI translation.
    void setTextTranslationEnabled(bool enabled) { m_translateText = enabled; }
    [[nodiscard]] const std::string& text() const { return m_text; }
    void setTextStyle(TextStyle style);
    void setFont(const Font& font);
    void setWeight(FontWeight weight);
    void setTextAlign(TextAlign align);
    void setLineLimit(int lines);
    void setColorRole(LabelColor role);
    [[nodiscard]] Font font() const;
    [[nodiscard]] Color textColor() const;
    /// Width of the whole text on one line, without needing a Painter (menus, tooltips, panel sizing).
    [[nodiscard]] float naturalWidth() const;

    Size measureContent(Size available) override;
    void draw(Painter& painter) override;

private:
    struct Line {
        size_t begin, end;
        float width;
    };
    void wrap(float width);

    bool m_translateText = true;
    std::string m_text;
    TextStyle m_style = TextStyle::Body;
    std::optional<Font> m_font;
    std::optional<FontWeight> m_weight;
    TextAlign m_align = TextAlign::Leading;
    int m_lineLimit = 0;
    LabelColor m_role = LabelColor::Primary;
    std::vector<Line> m_lines;
    float m_wrapWidth = -1;
    bool m_linesDirty = true;
};

/// Static text. Wraps to the available width unless `.lineLimit(1)`.
class Label : public WidgetHandle<Label, LabelView> {
public:
    explicit Label(std::string text = {}) : WidgetHandle(std::make_shared<LabelView>(std::move(text))) {}

    Label& verbatim() { return apply([](LabelView& n) { n.setTextTranslationEnabled(false); }); }
    Label& font(TextStyle style) { return apply([&](LabelView& n) { n.setTextStyle(style); }); }
    Label& font(const Font& f) { return apply([&](LabelView& n) { n.setFont(f); }); }
    Label& weight(FontWeight w) { return apply([&](LabelView& n) { n.setWeight(w); }); }
    Label& textAlign(TextAlign a) { return apply([&](LabelView& n) { n.setTextAlign(a); }); }
    Label& lineLimit(int lines) { return apply([&](LabelView& n) { n.setLineLimit(lines); }); }
    Label& color(LabelColor role) { return apply([&](LabelView& n) { n.setColorRole(role); }); }
    Label& secondary() { return color(LabelColor::Secondary); }
    Label& tertiary() { return color(LabelColor::Tertiary); }

    void setText(std::string text) { m_node->setText(std::move(text)); }
    [[nodiscard]] const std::string& text() const { return m_node->text(); }
};

}  // namespace mayo
