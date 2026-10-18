#include "images.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "mayonnaise/app.hpp"

namespace fs = std::filesystem;

namespace forma::store {

namespace {

constexpr std::size_t kMaxImage = 16u * 1024u * 1024u;


bool usable(const std::vector<unsigned char>& bytes) {
    if (bytes.size() < 8) return false;
    static const unsigned char png[] = {0x89, 'P', 'N', 'G'};
    const bool isPng = std::memcmp(bytes.data(), png, 4) == 0;
    const bool isJpeg = bytes[0] == 0xFF && bytes[1] == 0xD8;
    const bool isWebp = bytes.size() > 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' &&
                        bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' &&
                        bytes[11] == 'P';
    return isPng || isJpeg || isWebp;
}

std::size_t writeBytes(char* data, std::size_t size, std::size_t count, void* user) {
    auto& out = *static_cast<std::vector<unsigned char>*>(user);
    const std::size_t n = size * count;
    if (out.size() + n > kMaxImage) return 0;
    out.insert(out.end(), data, data + n);
    return n;
}

std::string cacheDirectory() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) base = xdg;
    else if (const char* home = std::getenv("HOME")) base = std::string(home) + "/.cache";
    else base = "/tmp";
    return base + "/mayonnaise/app_store";
}

std::string hashName(std::string_view text) {
    // FNV-1a 64-bit: stable across runs and short enough to read in a directory listing.
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(h));
    return buffer;
}

}  // namespace

ImageCache::ImageCache() : m_cacheDir(cacheDirectory()) {
    std::error_code ec;
    fs::create_directories(m_cacheDir, ec);
    m_worker = std::thread([this] { workerLoop(); });
}

ImageCache::~ImageCache() {
    {
        std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void ImageCache::setOnLoaded(std::function<void()> fn) {
    std::scoped_lock lock(m_mutex);
    m_onLoaded = std::move(fn);
}

void ImageCache::clear() {
    std::scoped_lock lock(m_mutex);
    m_images.clear();
    m_pending.clear();
    m_failed.clear();
    m_queue.clear();
}

std::shared_ptr<mayo::ImageData> ImageCache::lookup(const std::string& key) {
    std::scoped_lock lock(m_mutex);
    const auto it = m_images.find(key);
    return it == m_images.end() ? nullptr : it->second;
}

void ImageCache::enqueue(Request request) {
    {
        std::scoped_lock lock(m_mutex);
        if (m_pending.count(request.key) || m_failed.count(request.key)) return;
        m_pending.insert(request.key);
    }
    m_queue.push_back(std::move(request));
    m_cv.notify_one();
}

std::shared_ptr<mayo::ImageData> ImageCache::icon(const std::string& id, const std::string& localPath,
                                                  const std::string& url) {
    const std::string key = "icon:" + id;
    if (auto img = lookup(key)) return img;
    if (!localPath.empty()) {
        if (auto img = mayo::ImageData::load(localPath, 256)) {
            std::scoped_lock lock(m_mutex);
            m_images[key] = img;
            return img;
        }
    }
    if (url.empty()) return nullptr;
    const std::string file = m_cacheDir + "/icons/" + hashName(id) + ".img";
    enqueue({key, file, url, 0});
    return nullptr;
}

std::shared_ptr<mayo::ImageData> ImageCache::screenshot(const std::string& url, int maxDimension) {
    if (url.empty()) return nullptr;
    const std::string key = "shot:" + std::to_string(maxDimension) + ":" + url;
    if (auto img = lookup(key)) return img;
    const std::string file = m_cacheDir + "/shots/" + hashName(url) + ".img";
    enqueue({key, file, url, maxDimension});
    return nullptr;
}

std::string ImageCache::download(const std::string& url, bool& ok) const {
    ok = false;
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return {};
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::vector<unsigned char> bytes;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Forma-AppStore/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 64L * 1024);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status != 200 || !usable(bytes)) return {};
    ok = true;
    return std::string(bytes.begin(), bytes.end());
}

void ImageCache::workerLoop() {
    for (;;) {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            request = std::move(m_queue.front());
            m_queue.pop_front();
        }
        std::vector<unsigned char> bytes;
        bool fromDisk = false;
        {
            std::ifstream in(request.localPath, std::ios::binary);
            if (in) {
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                fromDisk = !bytes.empty();
            }
        }
        if (!fromDisk && !request.url.empty()) {
            bool ok = false;
            const std::string data = download(request.url, ok);
            if (ok) {
                bytes.assign(data.begin(), data.end());
                std::error_code ec;
                fs::create_directories(fs::path(request.localPath).parent_path(), ec);
                std::ofstream out(request.localPath, std::ios::binary | std::ios::trunc);
                if (out) out.write(data.data(), static_cast<std::streamsize>(data.size()));
            }
        }
        std::shared_ptr<mayo::ImageData> img;
        if (!bytes.empty()) {
            img = mayo::ImageData::decode(bytes);
            if (img && request.maxDimension > 0) {
                if (auto smaller = img->scaledToFit(request.maxDimension)) img = smaller;
            }
        }
        std::function<void()> notify;
        {
            std::scoped_lock lock(m_mutex);
            m_pending.erase(request.key);
            if (img) {
                m_images[request.key] = img;
                notify = m_onLoaded;
            } else if (!request.url.empty()) {
                m_failed.insert(request.key);
                m_offline = true;
            }
        }
        if (notify) {
            mayo::App::current().post([notify] { notify(); });
        }
    }
}

}  // namespace forma::store
