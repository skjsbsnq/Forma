#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <system_error>
#include <vector>

namespace forma::mail::remote {
// Fetches a remote image for the reader. Requests run on a worker thread; the
// callback receives the raw bytes, or an empty vector on failure. Repeated URLs
// are served from a private per-account disk cache (SHA-256 of the URL).
using Fetched = std::function<void(std::vector<unsigned char>, const std::string& error)>;

std::string cachePath(const std::string& configDirectory, const std::string& url);
// Serves from cache when present; else downloads (10 s timeout, 8 MB cap, https
// and http only, content must be an image). Thread-safe. Callback on UI thread
// via the provided post function.
void fetch(const std::string& configDirectory, const std::string& url, const std::function<void(std::function<void()>)>& post,
           Fetched done);
// Total bytes currently in the image cache.
std::uint64_t cacheSize(const std::string& configDirectory);
void clearCache(const std::string& configDirectory);
}  // namespace forma::mail::remote
