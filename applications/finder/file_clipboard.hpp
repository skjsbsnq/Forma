#pragma once
#include <string>
#include <vector>
namespace finder {
struct FileClipboard {
    std::vector<std::string> paths;
    bool cut = false;
};
// File-manager clipboard formats preserve URI escaping and move intent across processes.
std::string fileClipboardUris(const std::vector<std::string>& paths);
FileClipboard parseFileClipboard(const std::string& data, bool copiedFilesFormat = false);
bool writeFileClipboard(const FileClipboard& files);
FileClipboard readFileClipboard();
}
