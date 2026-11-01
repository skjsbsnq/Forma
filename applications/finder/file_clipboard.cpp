#include "file_clipboard.hpp"
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_video.h>
#include <glib.h>
#include <cstring>

#include <map>
#include <memory>
#include <sstream>
namespace finder {
namespace {
using Payload = std::map<std::string, std::string>;
Payload* ownedPayload = nullptr;
const void* SDLCALL provide(void* userdata, const char* mime, size_t* size) {
    *size = 0;
    if (!mime) return nullptr;
    const auto& payload = *static_cast<Payload*>(userdata);
    const auto it = payload.find(mime);
    if (it == payload.end()) return nullptr;
    *size = it->second.size();
    return it->second.data();
}
void SDLCALL cleanup(void* userdata) {
    if (ownedPayload == userdata) ownedPayload = nullptr;
    delete static_cast<Payload*>(userdata);
}
// SDL may discard an offer restored by a Wayland clipboard manager as recursive after
// its source has been cancelled. Read that current system selection via data-control.
// Never substitute a stale file list when another application has replaced it.
std::string waylandClipboard(const char* type) {
    if (!g_getenv("WAYLAND_DISPLAY")) return {};
    gchar* output = nullptr;
    gchar* errors = nullptr;
    gint status = 0;
    gchar* args[] = {const_cast<gchar*>("wl-paste"), const_cast<gchar*>("--no-newline"),
                     const_cast<gchar*>("--type"), const_cast<gchar*>(type), nullptr};
    const bool ok = g_spawn_sync(nullptr, args, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                                 &output, &errors, &status, nullptr) && status == 0;
    std::string result = ok && output ? output : "";
    g_free(output);
    g_free(errors);
    return result;
}
std::string getData(const char* mime) {
    size_t size = 0;
    void* data = SDL_GetClipboardData(mime, &size);
    std::string result = data ? std::string(static_cast<char*>(data), size) : std::string();
    SDL_free(data);
    return result;
}
}
std::string fileClipboardUris(const std::vector<std::string>& paths) {
    std::string result;
    for (const auto& path : paths) {
        if (char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr)) {
            result += uri;
            result += "\r\n";
            g_free(uri);
        }
    }
    return result;
}
FileClipboard parseFileClipboard(const std::string& data, bool copiedFilesFormat) {
    FileClipboard result;
    std::istringstream lines(data);
    std::string line;
    if (copiedFilesFormat) {
        std::getline(lines, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != "cut" && line != "copy") return result;
        result.cut = line == "cut";
    }
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line.find('\0') != std::string::npos) continue;
        if (line.rfind("file:", 0) == 0) {
            char* hostname = nullptr;
            char* path = g_filename_from_uri(line.c_str(), &hostname, nullptr);
            if (path && (!hostname || !*hostname || std::strcmp(hostname, "localhost") == 0)) result.paths.emplace_back(path);
            g_free(path);
            g_free(hostname);
        } else if (line[0] == '/') result.paths.push_back(line);
    }
    return result;
}
bool writeFileClipboard(const FileClipboard& files) {
    if (files.paths.empty() || !SDL_GetCurrentVideoDriver()) return false;
    auto payload = std::make_unique<Payload>();
    const auto uris = fileClipboardUris(files.paths);
    (*payload)["text/uri-list"] = uris;
    (*payload)["x-special/gnome-copied-files"] = std::string(files.cut ? "cut\n" : "copy\n") + uris;
    (*payload)["application/x-kde-cutselection"] = files.cut ? "1" : "0";
    std::string plain;
    for (const auto& path : files.paths) plain += (plain.empty() ? "" : "\n") + path;
    (*payload)["text/plain;charset=utf-8"] = plain;
    (*payload)["text/plain"] = plain;
    const char* types[] = {"x-special/gnome-copied-files", "text/uri-list", "application/x-kde-cutselection",
                          "text/plain;charset=utf-8", "text/plain"};
    auto* data = payload.release();
    if (!SDL_SetClipboardData(provide, cleanup, data, types, 5)) {
        // SDL registers the cleanup before calling the backend, including on backend failure.
        SDL_ClearClipboardData();
        return false;
    }
    ownedPayload = data;
    return true;
}
FileClipboard readFileClipboard() {
    // Use the registered provider while we own it; directory navigation doesn't change ownership.
    if (ownedPayload) return parseFileClipboard(ownedPayload->at("x-special/gnome-copied-files"), true);
    if (SDL_HasClipboardData("x-special/gnome-copied-files")) {
        auto files = parseFileClipboard(getData("x-special/gnome-copied-files"), true);
        if (!files.paths.empty()) return files;
    }
    if (SDL_HasClipboardData("text/uri-list")) {
        auto files = parseFileClipboard(getData("text/uri-list"));
        if (!files.paths.empty()) {
            if (SDL_HasClipboardData("application/x-kde-cutselection")) files.cut = getData("application/x-kde-cutselection") == "1";
            return files;
        }
    }
    if (g_getenv("WAYLAND_DISPLAY")) {
        auto files = parseFileClipboard(waylandClipboard("x-special/gnome-copied-files"), true);
        if (!files.paths.empty()) return files;
        files = parseFileClipboard(waylandClipboard("text/uri-list"));
        if (!files.paths.empty()) {
            files.cut = waylandClipboard("application/x-kde-cutselection") == "1";
            return files;
        }
        files = parseFileClipboard(waylandClipboard("text/plain;charset=utf-8"));
        if (!files.paths.empty()) return files;
    }
    char* text = SDL_GetClipboardText();
    auto files = parseFileClipboard(text ? text : "");
    SDL_free(text);
    return files;
}
}
