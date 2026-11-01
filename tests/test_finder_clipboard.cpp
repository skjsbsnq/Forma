#include "file_clipboard.hpp"
#include <cstdlib>
#include <SDL3/SDL.h>
#include <glib.h>
#include <iostream>
void testFinderClipboard() {
    auto require = [](bool ok, const char* message) {
        if (!ok) { std::cerr << "clipboard: " << message << '\n'; std::exit(1); }
    };
    const std::vector<std::string> paths = {"/tmp/中文 文件.txt", "/tmp/name#100%.txt", "/tmp/trailing ", "/tmp/line\nbreak"};
    const auto uris = finder::fileClipboardUris(paths);
    require(finder::parseFileClipboard(uris).paths == paths, "URI round trip preserves filenames");
    const auto cut = finder::parseFileClipboard("cut\n" + uris, true);
    require(cut.cut && cut.paths == paths, "move intent survives a cross-process payload");
    const auto copy = finder::parseFileClipboard("copy\r\n" + uris, true);
    require(!copy.cut && copy.paths == paths, "copy intent is explicit");
    require(finder::parseFileClipboard("cut\n" + uris, false).paths == paths, "plain URI fallback does not treat a marker as a path");
    require(finder::parseFileClipboard("file://elsewhere/tmp/a\r\n").paths.empty(), "remote URI cannot become a local path");
    require(finder::parseFileClipboard("file://localhost/tmp/a%20b\r\n").paths == std::vector<std::string>{"/tmp/a b"}, "localhost URI is decoded");
    require(finder::parseFileClipboard("/tmp/trailing \n").paths == std::vector<std::string>{"/tmp/trailing "}, "plain filename spaces are preserved");
    // An owning process reads its provider directly; replacing it must discard the old files.
    const std::string oldWayland = g_getenv("WAYLAND_DISPLAY") ? g_getenv("WAYLAND_DISPLAY") : "";
    const bool hadWayland = g_getenv("WAYLAND_DISPLAY") != nullptr;
    g_unsetenv("WAYLAND_DISPLAY");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    require(SDL_Init(SDL_INIT_VIDEO), "headless clipboard backend initializes");
    require(finder::writeFileClipboard({paths, true}), "publish cut files");
    const auto own = finder::readFileClipboard();
    require(own.cut && own.paths == paths, "same-process read preserves files and cut intent");
    require(SDL_SetClipboardText("replacement text"), "replace file clipboard with ordinary text");
    require(finder::readFileClipboard().paths.empty(), "replacement invalidates the owning-process files");
    require(finder::writeFileClipboard({paths, false}), "publish a second file selection");
    require(!finder::readFileClipboard().cut, "replacement can change cut to copy");
    require(SDL_ClearClipboardData(), "explicitly clear the clipboard");
    require(finder::readFileClipboard().paths.empty(), "clearing clipboard removes cached files");
    SDL_Quit();
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    if (hadWayland) g_setenv("WAYLAND_DISPLAY", oldWayland.c_str(), true);

}
