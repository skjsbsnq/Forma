#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include "../common/zh_cn.hpp"

namespace forma::text {
// Keep editable document contents, paths and grammar identifiers outside this catalogue.
inline std::string translate(std::string_view key) {
    static const std::unordered_map<std::string_view, std::string_view> messages{
        {"Untitled", "未命名"},
        {"Text Editor", "文本编辑器"},
        {"Forma Text", "Forma 文本编辑器"},
        {"Edited", "已修改"},
        {"Plain Text", "纯文本"},
        {"Find", "查找"},
        {"Replace with", "替换为"},
        {"Find & Replace", "查找与替换"},
        {"Find & Replace (Ctrl+F)", "查找与替换（Ctrl+F）"},
        {"Find text", "查找内容"},
        {"Replacement text", "替换内容"},
        {"Literal search, case-sensitive. Matches wrap around the document.",
         "按文字精确查找，区分大小写；到达文末后从头继续。"},
        {"Enter text to find", "输入要查找的文字"},
        {"Match selected in document", "已在文档中选中匹配内容"},
        {"No matches", "未找到匹配内容"},
        {"Replaced {count} matches", "已替换 {count} 处"},
        {"Found {count} matches", "找到 {count} 处匹配内容"},
        {"Replace All", "全部替换"},
        {"Replace", "替换"},
        {"Find Next", "查找下一个"},
        {"Previous match", "上一个匹配项"},
        {"Next match", "下一个匹配项"},
        {"Save Document", "保存文稿"},
        {"Save your document", "保存文稿"},
        {"Absolute file path", "文件完整路径"},
        {"Save location", "存储位置"},
        {"Enter the full path, including the file extension.", "请输入完整路径及文件扩展名。"},
        {"Save Changes?", "保存更改？"},
        {"Do you want to save your changes?", "要保存对文稿的更改吗？"},
        {"Your changes will be lost if you don’t save them.", "如果不保存，所做的更改将会丢失。"},
        {"Don’t Save", "不要保存"},
        {"Save…", "保存…"},
        {"Save", "保存"},
        {"Could not save: {path}", "无法保存：{path}"},
        {"Saved {name}", "已保存 {name}"},
        {"Could not open {path}", "无法打开：{path}"},
        {"Writing failed. Check available disk space.", "写入失败，请检查可用磁盘空间。"},
        {"Please enter a file path.", "请输入文件路径。"},
        {"This is a binary file. Open a UTF-8 text document.", "这是二进制文件，请打开 UTF-8 文本文稿。"},
        {"Text Editor Settings", "文本编辑器设置"},
        {"Editor", "编辑器"},
        {"Line numbers", "显示行号"},
        {"Wrap lines", "自动换行"},
        {"Font size", "字体大小"},
        {"Syntax highlighting", "语法高亮"},
        {"12 pt", "12 磅"},
        {"13 pt", "13 磅"},
        {"14 pt", "14 磅"},
        {"16 pt", "16 磅"},
        {"18 pt", "18 磅"},
        {"Open… (Ctrl+O)", "打开…（Ctrl+O）"},
        {"Save (Ctrl+S)", "保存（Ctrl+S）"},
        {"Syntax and display", "语法与显示"},
        {"Automatic", "自动识别"},
        {"Syntax", "语法"},
        {"Editor settings (Ctrl+,)", "编辑器设置（Ctrl+,）"},
        {"Document start", "跳到文稿开头"},
        {"Document end", "跳到文稿末尾"},
        {"New document (Ctrl+N)", "新建文稿（Ctrl+N）"},
        {"{lines} lines · {chars} characters · Ln {line}, Col {column}",
         "{lines} 行 · {chars} 个字符 · 第 {line} 行，第 {column} 列"},
        {"{lines} lines · {chars} chars · {line}:{column}", "{lines} 行 · {chars} 字符 · {line} 行 {column} 列"},
        {"Line ending: LF", "换行符：LF"},
        {"UTF-8 · LF", "UTF-8 · LF"},
    };
    if (auto found = messages.find(key); found != messages.end()) return std::string(found->second);
    return forma::zhCN(key);
}
inline std::string message(std::string_view key,
                           std::initializer_list<std::pair<std::string_view, std::string>> values) {
    auto result = translate(key);
    for (const auto& [name, value] : values) {
        const auto placeholder = "{" + std::string(name) + "}";
        size_t at = 0;
        while ((at = result.find(placeholder, at)) != std::string::npos) {
            result.replace(at, placeholder.size(), value);
            at += value.size();
        }
    }
    return result;
}
}  // namespace forma::text
