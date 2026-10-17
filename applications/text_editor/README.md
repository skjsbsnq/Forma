# Forma Text

A native C++20 text editor built with this repository's Mayonnaise framework (SDL3 and the existing GPU renderer). The application does not embed a browser.

## Build and run

```sh
cmake -S . -B build -DMAYONNAISE_BUILD_APPS=ON
cmake --build build --target texteditor -j4
./build/applications/text_editor/texteditor
./build/applications/text_editor/texteditor /path/to/document.txt
```

The binary is `texteditor`. Its desktop entry advertises plain text, Markdown, C++ and SVG. Install with `cmake --install build --component texteditor`.

Open, new, save and save as use Ctrl+O, Ctrl+N, Ctrl+S and Ctrl+Shift+S. Ctrl+F opens Find & Replace; Ctrl+G / Ctrl+Shift+G move between literal matches. Ctrl+, opens Settings. The editor supports UTF-8 code point navigation, mouse selection, shift selection, clipboard, home/end, up/down, word navigation/deletion, select all, undo and redo. Tab inserts four spaces. File drops open a document after checking unsaved changes. CRLF and CR files normalize to LF. New, open and close protect unsaved changes with Save / Don't Save / Cancel.

Save As currently uses a native framework dialog with an editable full destination path. An error appears in the document navigation strip if opening or writing fails. This first version edits one document per window; the tab strip identifies that document and its plus button starts a new document with unsaved-change protection.

## Visual reference and alignment

The primary reference is the current official [CotEditor](https://coteditor.com/) light screenshot, inspected directly at full resolution:

- [Main light screenshot](https://coteditor.com/img/screenshots/screenshot%402x.png)
- [Dark appearance](https://coteditor.com/img/screenshots/darkmode%402x.png)
- [File browser](https://coteditor.com/img/screenshots/filebrowser%402x.png)
- [Tools](https://coteditor.com/img/screenshots/tools%402x.png)
- [Settings](https://coteditor.com/img/screenshots/settings%402x.png)

The light image's application window occupies approximately 1,246 × 1,076 pixels at 2×, corresponding to 623 × 538 points. This implementation keeps the reference's chrome metrics while offering a larger default 860 × 700 point window: 52 point unified title bar, 36 point document strip, 25 point navigation strip and 32 point status bar. The editor has a 40 point left inset, a narrow trailing-aligned line-number gutter, a 13 point monospace face and 19.5 point line spacing. Selection and active-line backgrounds follow the same editor grid. SVG highlighting distinguishes tag names, attributes, quoted values, DOCTYPE and CSS selectors. Both appearances use framework semantic colors.

This is a close visual adaptation, not a claim of pixel-identical macOS rendering. Framework window corners, glass, traffic lights, icons and control bezels are retained. The typefaces are Inter plus the installed monospace family, not proprietary Apple fonts. The toolbar adds working open/save/find affordances. There is no file browser, real multi-tab session, regex engine, split editor or full language grammar yet. Find & Replace is literal, case-sensitive and wraps through the document. UTF-8 movement operates on code points, not extended grapheme clusters. The original screenshots are references only and are not bundled or displayed inside the application.

## Verification

```sh
cmake -S . -B build -DMAYONNAISE_BUILD_TESTS=ON -DMAYONNAISE_USE_SYSTEM_DEPS=OFF
cmake --build build --target texteditor text_editor_tests -j4
ctest --test-dir build -R text_editor_tests --output-on-failure
```

Five regression cases with 40 assertions cover UTF-8 cursor/deletion/selection boundaries, literal find wraparound, replacement undo/redo, Unicode line/column reporting true unwrapped document width, Chinese UI translation, verbatim filenames and document text that coincides with UI message keys. These tests run without a display server.

The save-confirmation lifecycle also has an actual GUI smoke path. Supply a **nonexistent scratch file**; the smoke run refuses an existing destination and exits 0 only after the newly opened Save dialog survives the old confirmation's deferred destruction, the same Save button callback runs, and the UTF-8 document is read back successfully:

```sh
MAYO_DEMO=save-flow MAYO_TEST_SAVE_PATH=/tmp/forma-text-save-test.txt \
  XDG_CONFIG_HOME=/tmp/forma-text-test ./build/applications/text_editor/texteditor
```

For reproducible visual checks:

```sh
MAYO_DEMO=sample MAYO_APPEARANCE=light MAYO_SCREENSHOT=/tmp/editor.png \
  XDG_CONFIG_HOME=/tmp/forma-text-test ./build/applications/text_editor/texteditor
# MAYO_DEMO=find|settings and MAYO_SCREENSHOT_WINDOW=last capture auxiliary windows.
```

Light, dark, find and settings screenshots were rendered and visually inspected for alignment, overlap and clipping. The verified images are under [`docs/screenshots`](../../docs/screenshots/): `text-editor-light.png`, `text-editor-dark.png`, `text-editor-find.png` and `text-editor-settings.png`.


## 简体中文与第二轮视觉打磨

应用现在通过专用 `i18n.hpp` 注册框架的 `UiTranslator`，并复用全项目 `forma::zhCN` 基础词库。标题、工具提示、查找与替换、匹配数量、保存与未保存确认、错误信息、编辑器设置和自绘状态栏均为简体中文。文稿内容、用户文件名、完整路径及语法标识不会参与 UI 翻译；`LabelView` 新增可关闭自动翻译的能力，编辑器与文件名标签明确使用此模式。回归测试验证正文为 `Settings`、`Save`、`Plain Text` 时仍逐字保留原文，撤销也不会改写正文。

查找窗从 285 磅收紧为 202 磅，两行保留固定的“查找”和“替换为”标签，输入框、结果与按钮使用统一间距；在最窄 480 磅宽度核验。输入时显示真实匹配数量，上一个/下一个操作同步反馈结果。工具栏语法按钮现在打开真实菜单，可切换自动识别、纯文本、SVG、语法高亮、行号与自动换行。导航与状态栏没有菜单的假箭头已移除，重复的新建按钮也已合并为文稿栏右侧的单一加号。设置入口使用齿轮图标，保存窗显示完整目标路径并在窗口内报告失败。

自绘状态栏按实际文字宽度选择完整或紧凑中文格式，并分区裁切，编码信息始终留有独立空间。最窄 620 磅主窗口使用长中文文件名、中文正文及中英混排进行验证；文件名、行号、文本基线、标点和状态信息没有重叠或截断。浅色和深色主图都使用中文写作示例，SVG 配色对照单独保留。

本轮继续采用当前 CotEditor 官方图片，不混用搜索结果中的旧 macOS 版本。版本依据补充为 [CotEditor 6.0 / macOS 26 Liquid Glass 发布说明](https://coteditor.com/releasenotes/6.0.1.en)，中文操作术语参考 [Apple 文本编辑：查找和替换文本](https://support.apple.com/zh-cn/guide/textedit/txtef6cfde1a/mac)。

最终截图均由实际应用渲染并逐张查看：

| 截图 | 检查内容 |
| --- | --- |
| `text-editor-light.png` / `text-editor-dark.png` | 中文正文、混排、明暗外观与状态栏 |
| `text-editor-svg.png` | 原始 CotEditor SVG 基准的配色及几何对齐 |
| `text-editor-find.png` / `text-editor-find-narrow.png` | 永久行标签、匹配数量、202 磅窗高、480 磅最窄宽度 |
| `text-editor-settings.png` | 全中文设置与表单左右对齐 |
| `text-editor-save.png` / `text-editor-unsaved.png` | 中文保存、完整路径、未保存确认及按钮文案 |
| `text-editor-narrow.png` | 620 磅主窗口、长中文文件名与中文状态栏 |
| `text-editor-display.png` | 可操作的语法与显示菜单 |

截图位于 `docs/screenshots/`。可用 `MAYO_DEMO=chinese|sample|find|find-narrow|settings|save|unsaved|narrow|display` 重现；辅助窗口需加 `MAYO_SCREENSHOT_WINDOW=last`。GUI 保存生命周期回归在本轮中文环境中再次运行成功。
