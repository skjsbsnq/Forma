// Icons and screenshots for the store, loaded off the UI thread.
//
// AppStream ships 128×128 PNGs next to its metadata, so most icons appear instantly. Remote screenshots
// and the rare remote icon are downloaded with libcurl, cached under
// `$XDG_CACHE_HOME/mayonnaise/app_store/`, decoded on the worker and handed to the UI thread through
// App::post. Requests are deduplicated and failures are remembered so a broken URL is not retried on
// every frame.
#pragma once

#include <mayonnaise/mayonnaise.hpp>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace forma::store {

class ImageCache {
public:
    ImageCache();
    ~ImageCache();
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    /// Called on the UI thread after an image became available (invalidates the window).
    void setOnLoaded(std::function<void()> fn);

    /// Icon for an app: local PNG first, remote URL otherwise. Null while loading.
    std::shared_ptr<mayo::ImageData> icon(const std::string& id, const std::string& localPath,
                                         const std::string& url);
    /// A screenshot thumbnail (`maxDimension` downscales it). Null while loading.
    std::shared_ptr<mayo::ImageData> screenshot(const std::string& url, int maxDimension = 1000);

    /// Forgets every image and pending request (catalogue reloads).
    void clear();

    [[nodiscard]] bool offline() const { return m_offline; }

private:
    struct Request {
        std::string key;      ///< cache key ("icon:<id>" or "shot:<url>")
        std::string localPath;
        std::string url;
        int maxDimension = 0;
    };

    [[nodiscard]] std::shared_ptr<mayo::ImageData> lookup(const std::string& key);
    void enqueue(Request request);
    void workerLoop();
    [[nodiscard]] std::string download(const std::string& url, bool& ok) const;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Request> m_queue;
    std::map<std::string, std::shared_ptr<mayo::ImageData>> m_images;
    std::set<std::string> m_pending;
    std::set<std::string> m_failed;
    std::thread m_worker;
    bool m_stop = false;
    bool m_offline = false;
    std::string m_cacheDir;
    std::function<void()> m_onLoaded;
};

}  // namespace forma::store
