#include "image_io.hpp"

#include <fstream>
#include <iterator>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace mayo::render {

void DecodedImage::DecoderFree::operator()(std::uint8_t* pixels) const { stbi_image_free(pixels); }

DecodedImage::DecodedImage(const DecodedImage& other) : width(other.width), height(other.height) {
    const auto bytes = other.pixels();
    if (!bytes.empty()) m_owned.assign(bytes.begin(), bytes.end());
}

DecodedImage& DecodedImage::operator=(const DecodedImage& other) {
    if (this != &other) *this = DecodedImage(other);
    return *this;
}

std::span<std::uint8_t> DecodedImage::pixels() {
    if (m_decoded) return {m_decoded.get(), static_cast<size_t>(width) * height * 4};
    return m_owned;
}

std::span<const std::uint8_t> DecodedImage::pixels() const {
    if (m_decoded) return {m_decoded.get(), static_cast<size_t>(width) * height * 4};
    return m_owned;
}

void DecodedImage::setPixels(std::vector<std::uint8_t> pixels) {
    m_decoded.reset();
    m_owned = std::move(pixels);
}

void DecodedImage::clearPixels() {
    m_decoded.reset();
    std::vector<std::uint8_t>().swap(m_owned);
}

void DecodedImage::adoptPixels(std::uint8_t* pixels) {
    clearPixels();
    m_decoded.reset(pixels);
}

namespace {
bool finish(unsigned char* data, int w, int h, DecodedImage& out) {
    if (!data) return false;
    out.width = w;
    out.height = h;
    out.adoptPixels(data);
    // The renderer blends premultiplied colours.
    auto pixels = out.pixels();
    for (size_t i = 0; i < pixels.size(); i += 4) {
        unsigned a = pixels[i + 3];
        for (size_t c = 0; c < 3; ++c) pixels[i + c] = static_cast<std::uint8_t>((pixels[i + c] * a + 127) / 255);
    }
    return true;
}
}  // namespace

namespace {

std::uint32_t read(std::span<const std::uint8_t> b, size_t at, int n, bool little) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        std::uint32_t byte = b[at + static_cast<size_t>(little ? n - 1 - i : i)];
        v = (v << 8) | byte;
    }
    return v;
}

void applyOrientation(DecodedImage& img, int orientation) {
    if (orientation <= 1 || orientation > 8) return;
    const int w = img.width;
    const int h = img.height;
    const bool swap = orientation >= 5;
    const int ow = swap ? h : w;
    const int oh = swap ? w : h;
    const auto pixels = img.pixels();
    std::vector<std::uint8_t> out(pixels.size());
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            int sx = 0;
            int sy = 0;
            switch (orientation) {
                case 2: sx = w - 1 - x; sy = y; break;
                case 3: sx = w - 1 - x; sy = h - 1 - y; break;
                case 4: sx = x; sy = h - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = h - 1 - x; break;
                case 7: sx = w - 1 - y; sy = h - 1 - x; break;
                case 8: sx = w - 1 - y; sy = x; break;
                default: break;
            }
            const size_t d = (static_cast<size_t>(y) * ow + x) * 4;
            const size_t s = (static_cast<size_t>(sy) * w + sx) * 4;
            for (int c = 0; c < 4; ++c) out[d + c] = pixels[s + c];
        }
    }
    img.setPixels(std::move(out));
    img.width = ow;
    img.height = oh;
}

}  // namespace

int exifOrientation(std::span<const std::uint8_t> b) {
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return 1;
    size_t i = 2;
    while (i + 4 <= b.size() && b[i] == 0xFF) {
        const std::uint8_t marker = b[i + 1];
        const size_t len = read(b, i + 2, 2, false);
        if (marker == 0xDA || len < 2) break;  // start of scan: no more metadata
        const size_t seg = i + 4;
        if (marker == 0xE1 && seg + 14 <= b.size() && b[seg] == 'E' && b[seg + 1] == 'x' && b[seg + 2] == 'i' &&
            b[seg + 3] == 'f') {
            const size_t tiff = seg + 6;
            const bool little = b[tiff] == 'I';
            const size_t ifd = tiff + read(b, tiff + 4, 4, little);
            if (ifd + 2 > b.size()) return 1;
            const size_t count = read(b, ifd, 2, little);
            for (size_t k = 0; k < count; ++k) {
                const size_t e = ifd + 2 + k * 12;
                if (e + 12 > b.size()) break;
                if (read(b, e, 2, little) == 0x0112) {
                    auto v = static_cast<int>(read(b, e + 8, 2, little));
                    return v >= 1 && v <= 8 ? v : 1;
                }
            }
            return 1;
        }
        i += 2 + len;
    }
    return 1;
}

void resizeBox(DecodedImage& img, int tw, int th) {
    if (tw <= 0 || th <= 0 || (tw >= img.width && th >= img.height)) return;
    const int w = img.width;
    const int h = img.height;
    const auto pixels = img.pixels();
    std::vector<std::uint32_t> acc(static_cast<size_t>(tw) * th * 4, 0);
    std::vector<std::uint32_t> cnt(static_cast<size_t>(tw) * th, 0);
    for (int y = 0; y < h; ++y) {
        const int ty = static_cast<int>(static_cast<std::int64_t>(y) * th / h);
        for (int x = 0; x < w; ++x) {
            const int tx = static_cast<int>(static_cast<std::int64_t>(x) * tw / w);
            const size_t d = static_cast<size_t>(ty) * tw + tx;
            const size_t s = (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c) acc[d * 4 + c] += pixels[s + c];
            ++cnt[d];
        }
    }
    std::vector<std::uint8_t> out(static_cast<size_t>(tw) * th * 4);
    for (size_t d = 0; d < cnt.size(); ++d) {
        const std::uint32_t n = std::max<std::uint32_t>(1, cnt[d]);
        for (int c = 0; c < 4; ++c) out[d * 4 + c] = static_cast<std::uint8_t>((acc[d * 4 + c] + n / 2) / n);
    }
    img.setPixels(std::move(out));
    img.width = tw;
    img.height = th;
}

bool decodeImage(std::span<const std::uint8_t> bytes, DecodedImage& out) {
    int w = 0;
    int h = 0;
    int n = 0;
    unsigned char* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
    if (!finish(data, w, h, out)) return false;
    applyOrientation(out, exifOrientation(bytes));
    return true;
}

bool loadImageFile(const std::string& path, DecodedImage& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return decodeImage(bytes, out);
}

bool writePng(const std::string& path, int width, int height, std::vector<std::uint8_t> px) {
    for (size_t i = 0; i < px.size(); i += 4) {
        unsigned a = px[i + 3];
        if (a == 0 || a == 255) continue;
        for (size_t c = 0; c < 3; ++c) {
            unsigned v = (px[i + c] * 255u + a / 2) / a;
            px[i + c] = static_cast<std::uint8_t>(v > 255 ? 255 : v);
        }
    }
    return stbi_write_png(path.c_str(), width, height, 4, px.data(), width * 4) != 0;
}

}  // namespace mayo::render
