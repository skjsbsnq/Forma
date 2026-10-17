#include "remote.hpp"

#include <curl/curl.h>
#include <glib.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <cstring>
#include <fstream>
#include <mutex>
#include <span>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace forma::mail::remote {
namespace {
constexpr size_t maxImage = 8 * 1024 * 1024;
std::mutex& curlMutex() {
    static std::mutex m;
    return m;
}
size_t write(char* p, size_t size, size_t n, void* user) {
    auto& out = *static_cast<std::vector<unsigned char>*>(user);
    size_t count = size * n;
    if (out.size() + count > maxImage) return 0;
    out.insert(out.end(), p, p + count);
    return count;
}
bool looksLikeImage(const std::vector<unsigned char>& bytes) {
    // Sniffing is authoritative: a server claiming image/* must still produce real
    // bitmap magic. SVG and HTML are never rendered (scriptable / phishing).
    if (bytes.size() < 8) return false;
    static const unsigned char png[] = {0x89, 'P', 'N', 'G'}, gif[] = {'G', 'I', 'F'}, bmp[] = {'B', 'M'},
                               webp1[] = {'R', 'I', 'F', 'F'};
    if (!std::memcmp(bytes.data(), png, 4) || !std::memcmp(bytes.data(), gif, 3) || !std::memcmp(bytes.data(), bmp, 2))
        return true;
    if (bytes[0] == 0xFF && bytes[1] == 0xD8) return true;  // JPEG
    if (!std::memcmp(bytes.data(), webp1, 4) && bytes.size() > 12 && !std::memcmp(bytes.data() + 8, "WEBP", 4))
        return true;
    return false;
}
}  // namespace

std::string cachePath(const std::string& configDirectory, const std::string& url) {
    auto* digest = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(digest, reinterpret_cast<const guchar*>(url.data()), url.size());
    const gchar* hex = g_checksum_get_string(digest);
    std::string name = hex;
    g_checksum_free(digest);
    return configDirectory + "/remote-images/" + name + ".img";
}

std::uint64_t cacheSize(const std::string& configDirectory) {
    std::error_code ec;
    std::uint64_t total = 0;
    std::filesystem::directory_iterator it(configDirectory + "/remote-images", ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) total += static_cast<std::uint64_t>(it->file_size(ec));
    }
    return total;
}

void clearCache(const std::string& configDirectory) {
    std::error_code ec;
    std::filesystem::remove_all(configDirectory + "/remote-images", ec);
}

void fetch(const std::string& configDirectory, const std::string& url, const std::function<void(std::function<void()>)>& post,
           Fetched done) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        post([done] { done({}, "不支持的图片地址"); });
        return;
    }
    std::thread([configDirectory, url, post = post, done = std::move(done)] {
        // Cache first: the reader can call fetch for every draw without cost.
        auto path = cachePath(configDirectory, url);
        std::error_code ec;
        if (std::filesystem::file_size(path, ec)) {
            std::ifstream file(path, std::ios::binary);
            std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(file), {});
            if (!bytes.empty()) {
                post([bytes = std::move(bytes), done] { done(std::move(bytes), {}); });
                return;
            }
        }
        static std::once_flag init;
        std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        CURL* c = curl_easy_init();
        if (!c) {
            post([done] { done({}, "无法初始化网络"); });
            return;
        }
        std::vector<unsigned char> bytes;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &bytes);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Forma-Mail/0.2");
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 4L);
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024);
        auto code = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        std::string error;
        if (code != CURLE_OK)
            error = "图片下载失败";
        else if (status != 200)
            error = "图片服务器返回 " + std::to_string(status);
        else if (!looksLikeImage(bytes))
            error = "服务器返回的不是图片";
        if (error.empty()) {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        post([bytes = std::move(bytes), error, done] { done(std::move(bytes), error); });
    }).detach();
}
}  // namespace forma::mail::remote
