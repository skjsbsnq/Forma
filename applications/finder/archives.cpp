#include "archives.hpp"

#include "applications.hpp"
#include "file_model.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace finder {

bool canExtractArchives(const std::vector<std::string>& paths) {
    return !paths.empty() && std::all_of(paths.begin(), paths.end(), [](const std::string& path) {
        std::error_code ec;
        if (isInTrash(path) || !std::filesystem::is_regular_file(path, ec)) return false;
        const auto entry = statEntry(path);
        if (!entry || entry->isDir) return false;
        if (entry->kind == FileKind::Archive) return true;
        static const std::set<std::string> types{
            "application/zip",
            "application/x-zip",
            "application/x-zip-compressed",
            "application/vnd.rar",
            "application/x-rar",
            "application/x-rar-compressed",
            "application/x-7z-compressed",
            "application/x-tar",
            "application/x-compressed-tar",
            "application/gzip",
            "application/x-gzip",
            "application/x-bzip",
            "application/x-bzip2",
            "application/x-bzip-compressed-tar",
            "application/x-xz",
            "application/x-xz-compressed-tar",
            "application/zstd",
            "application/x-zstd-compressed-tar",
            "application/x-lzma",
            "application/x-lzma-compressed-tar",
            "application/x-lzip",
            "application/x-lzip-compressed-tar",
        };
        return types.contains(mimeTypeFor(path));
    });
}

std::string extractArchives(const std::vector<std::string>& paths, const std::string& destination) {
    if (!canExtractArchives(paths)) return "请选择回收站以外的压缩包。";
    if (destination.empty()) {
        // Search/Recents may select archives from different folders. Extract each group beside its files.
        std::map<std::string, std::vector<std::string>> groups;
        for (const auto& path : paths) groups[std::filesystem::absolute(path).parent_path().string()].push_back(path);
        for (const auto& [folder, archives] : groups) {
            if (const auto error = extractArchives(archives, folder); !error.empty()) return error;
        }
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(destination, ec)) return "解压目标文件夹不存在或无法访问。";
    std::vector<std::string> command{"ark", "--batch", "--destination",
                                     std::filesystem::absolute(destination).string()};
    // Pass arguments directly: spaces, quotes and shell metacharacters remain part of the file name.
    command.push_back("--");
    for (const auto& path : paths) command.push_back(std::filesystem::absolute(path).string());
    std::string error;
    if (!spawnDetachedProcess(command, &error)) return "无法启动 Ark，请确认已安装 ark。\n" + error;
    return {};
}

}  // namespace finder
