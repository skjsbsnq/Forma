#include <mayonnaise/mayonnaise.hpp>

#include <array>
#include <cstdlib>
#include <string_view>

using namespace mayo;

namespace {

void paintGlass(Painter& p, const Rect& r) {
    const Theme& t = theme();
    const std::array colors{t.colors.blue, t.colors.mint, t.colors.orange, t.colors.pink};
    p.pushClip(r, t.metrics.sidebarRadius);
    for (size_t i = 0; i < colors.size(); ++i) {
        float x = r.x + r.width * static_cast<float>(i) / 4;
        p.fillVerticalGradient({x, r.y, r.width / 4 + 1, r.height}, {}, colors[i], colors[(i + 1) % 4]);
    }
    for (int i = 1; i < 10; ++i) {
        const float x = r.x + r.width * static_cast<float>(i) / 10;
        p.drawLine({x, r.y}, {x, r.bottom()}, Color::white(0.32f), 1);
    }
    const float diameter = std::min(r.height - 32, r.width * 0.30f);
    for (int i = 0; i < 2; ++i) {
        Rect lens{r.x + r.width * (i == 0 ? 0.25f : 0.75f) - diameter / 2, r.center().y - diameter / 2, diameter,
                  diameter};
        p.drawShadow(lens, diameter / 2, t.colors.shadow, 18, {0, 5});
        p.drawMaterial(lens, diameter / 2, i == 0 ? Material::Glass : Material::GlassClear);
        p.drawIcon(i == 0 ? Icon::Sparkles : Icon::Photo, {lens.center().x - 12, lens.center().y - 26, 24, 24},
                   t.colors.label, 1.2f);
        p.drawText(i == 0 ? "柔和玻璃" : "清透玻璃", {lens.x, lens.center().y + 5, lens.width, 24},
                   t.font(TextStyle::Headline), t.colors.label, TextAlign::Center);
    }
    p.popClip();
}

Canvas sampleRows() {
    return Canvas([](Painter& p, const Rect& r) {
               const Theme& t = theme();
               p.fillRect(r, t.colors.contentBackground);
               for (int i = 0; i < 20; ++i) {
                   Rect row{r.x, r.y + static_cast<float>(i) * 38, r.width, 38};
                   if (i % 2 == 0) p.fillRect(row, t.colors.secondaryFill);
                   p.drawIcon(Icon::Document, {row.x + 12, row.y + 11, 16, 16}, t.colors.accent);
                   p.drawText("设计文稿 " + std::to_string(i + 1), {row.x + 40, row.y, row.width - 52, row.height},
                              t.font(TextStyle::Body), t.colors.label);
               }
           })
        .height(760)
        .fillWidth();
}

}  // namespace

int main(int argc, char** argv) {
    const bool narrow = argc > 1 && std::string_view(argv[1]) == "--narrow";
    App app("forma-components");
    auto& window = app.createWindow(
        {.title = "Forma 组件预览", .width = narrow ? 480 : 1040, .height = 850, .minWidth = 480, .minHeight = 620});
    window.setTitleVisible(false);
    auto status = Label("缩窄窗口，右侧操作会自动收纳到更多菜单。").secondary().lineLimit(2);
    auto action = [status](const std::string& title) { status.node()->setText("已执行：" + title); };
    auto toolbar = Toolbar(
        {Button(Icon::Sidebar).style(ButtonStyle::Borderless).tooltip("显示侧边栏").onClick([action] {
             action("显示侧边栏");
         }),
         Label("组件预览").font(TextStyle::Headline), Spacer(),
         ToolbarGroup(
             {Button(Icon::Import, "导入").style(ButtonStyle::Borderless).onClick([action] { action("导入"); }),
              Button(Icon::Share, "分享").style(ButtonStyle::Borderless).onClick([action] { action("分享"); })}),
         ToolbarGroup({Button(Icon::Star, "收藏").style(ButtonStyle::Borderless).onClick([action] { action("收藏"); }),
                       Button(Icon::Trash, "删除").style(ButtonStyle::Borderless).disabled()}),
         SegmentedControl({"浏览", "检查"}).onChange([action](int i) { action(i == 0 ? "浏览" : "检查"); })});
    window.setToolbar(toolbar);
    auto soft = ScrollView(sampleRows()).scrollEdge(ScrollEdgeStyle::Soft, 38).height(180).fillWidth();
    auto hard = ScrollView(sampleRows()).scrollEdge(ScrollEdgeStyle::Hard, 38).height(180).fillWidth();
    auto edges = HStack({VStack({Label("柔和渐隐").font(TextStyle::Headline), soft}).fillWidth(),
                         VStack({Label("磨砂边缘").font(TextStyle::Headline), hard}).fillWidth()})
                     .spacing(20);
    auto content =
        VStack({Label("让材质与交互一起成形").font(TextStyle::Title1),
                Label("沿用 Forma 字体与主题，增强材质和交互。").secondary().lineLimit(2),
                Canvas(paintGlass).height(190).fillWidth(), Label("滚动边缘").font(TextStyle::Title2),
                Label("列表可独立滚动，边缘效果随内容位置变化。").secondary().lineLimit(2), edges,
                Label("工具栏收纳").font(TextStyle::Title2), status,
                HStack({Button("浅色外观").onClick([&app] { app.setAppearanceMode(AppearanceMode::Light); }),
                        Button("深色外观").onClick([&app] { app.setAppearanceMode(AppearanceMode::Dark); })})
                    .spacing(12)})
            .spacing(14)
            .padding(Insets(24, 28))
            .fillWidth();
    window.setContent(ScrollView(content));
    app.after(0.05, [soft, hard] {
        soft.node()->scrollTo({0, 76}, false);
        hard.node()->scrollTo({0, 76}, false);
    });
    if (const char* demo = std::getenv("MAYO_DEMO"); demo && std::string_view(demo) == "overflow") {
        app.after(0.15, [toolbar] {
            for (const auto& child : toolbar.node()->children()) {
                if (auto button = std::dynamic_pointer_cast<ButtonView>(child);
                    button && button->icon() == Icon::Ellipsis && button->isVisible())
                    button->click();
            }
        });
    }
    return app.run();
}
