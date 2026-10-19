#pragma once
#include <mayonnaise/mayonnaise.hpp>
#include "capture.hpp"

namespace shot {
enum class Mode { Region, Fullscreen, Window };

// The canvas owns only image/selection drawing. All HUD controls are real Forma
// controls, with their normal layout, keyboard focus, hover and press animations.
class CaptureView final : public mayo::Widget {
public:
    CaptureView();
    void present(Image image, Mode mode);
    void setMode(Mode mode);
    void setBusy(bool busy);
    void setMessage(std::string message);
    void demoSelection();
    void disappear();
    void resume();
    [[nodiscard]] Image result() const;
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] Mode mode() const { return m_mode; }
    bool key(const mayo::KeyEvent& event);
    std::function<void(bool)> onOutput;
    std::function<void(Mode)> onMode;
    std::function<void()> onCancel;
    std::function<void()> onSelected;

    bool extendsIntoTitleBar() const override { return true; }
    bool isInteractive() const override { return true; }
    bool acceptsFocus() const override { return true; }
    bool expandsContent(mayo::Axis) const override { return true; }
    void arrangeContent(const mayo::Rect&) override;
    void draw(mayo::Painter&) override;
    bool onPointerDown(const mayo::PointerEvent&) override;
    void onPointerMove(const mayo::PointerEvent&) override;
    void onPointerUp(const mayo::PointerEvent&) override;

private:
    enum Drag { None, Create, Move, NW, N, NE, E, SE, S, SW, W };
    Image m_image;
    std::shared_ptr<mayo::ImageData> m_texture;
    Mode m_mode = Mode::Region;
    mayo::Rect m_selection, m_before, m_toolbar;
    mayo::Point m_anchor, m_pointer;
    Drag m_drag = None;
    bool m_pointerKnown = false, m_busy = false, m_exiting = false;
    mayo::Animated<float> m_reveal{0}, m_exit{1};
    mayo::Animated<mayo::Rect> m_hudPosition;
    std::shared_ptr<mayo::ToolbarGroupView> m_hud;
    std::shared_ptr<mayo::SegmentedControlView> m_modes;
    std::shared_ptr<mayo::LabelView> m_dimensions;
    std::shared_ptr<mayo::ButtonView> m_save, m_copy, m_cancel;
    std::string m_message;

    mayo::Rect imageRect() const;
    mayo::Rect hudRect() const;
    std::string dimensions() const;
    void updateControls();
    void updateCursor(mayo::Point);
    Drag hitHandle(mayo::Point) const;
    void requestOutput(bool copy);
    void drawSelection(mayo::Painter&, float reveal);
};
}  // namespace shot
