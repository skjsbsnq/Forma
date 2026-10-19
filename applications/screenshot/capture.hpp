#pragma once
#include <cstdint>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>
namespace shot {
struct Image {
    int width = 0, height = 0;
    double scale = 1;
    std::vector<std::uint8_t> rgba;
};
/// A cancelled KWin picker / capture is a normal user dismissal.
class CaptureCancelled : public std::runtime_error {
public:
    CaptureCancelled() : std::runtime_error("截图已取消") {}
};
/// Blocking operation: run on a worker, then post the image to the Forma UI thread.
Image capture(bool window, std::stop_token stop = {});
Image crop(const Image&, int x, int y, int width, int height);
Image cropLogical(const Image&, double x, double y, double width, double height, double viewWidth, double viewHeight);
/// Thread-safe CPU encoding; call off the UI thread. Input remains unmodified.
std::vector<std::uint8_t> png(const Image&);
/// Blocking output operations, safe on a worker. copy returns after the clipboard owner is ready.
void save(const std::vector<std::uint8_t>&, const std::string&);
void copy(const std::vector<std::uint8_t>&);
std::string defaultDirectory();
}  // namespace shot
