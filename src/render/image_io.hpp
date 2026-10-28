#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mayo::render {

struct DecodedImage {
    int width = 0;
    int height = 0;

    DecodedImage() = default;
    DecodedImage(const DecodedImage& other);
    DecodedImage& operator=(const DecodedImage& other);
    DecodedImage(DecodedImage&&) noexcept = default;
    DecodedImage& operator=(DecodedImage&&) noexcept = default;

    /// RGBA8, premultiplied. Decoded pixels retain the decoder's allocation until replaced or released.
    [[nodiscard]] std::span<std::uint8_t> pixels();
    [[nodiscard]] std::span<const std::uint8_t> pixels() const;
    void setPixels(std::vector<std::uint8_t> pixels);
    void clearPixels();
    /// Takes ownership of an allocation returned by stb_image (dimensions must already be set).
    void adoptPixels(std::uint8_t* pixels);

private:
    struct DecoderFree {
        void operator()(std::uint8_t* pixels) const;
    };
    std::unique_ptr<std::uint8_t, DecoderFree> m_decoded;
    std::vector<std::uint8_t> m_owned;
};

/// Decodes PNG/JPEG/GIF/BMP/TGA and applies the JPEG EXIF orientation, so photos come out upright.
bool decodeImage(std::span<const std::uint8_t> bytes, DecodedImage& out);
bool loadImageFile(const std::string& path, DecodedImage& out);
/// Area-averaging downscale (premultiplied pixels average correctly). No-op if already that small.
void resizeBox(DecodedImage& image, int width, int height);
/// EXIF orientation tag (1..8) of a JPEG, 1 if absent.
int exifOrientation(std::span<const std::uint8_t> bytes);
/// Writes premultiplied RGBA8 pixels as a straight-alpha PNG.
bool writePng(const std::string& path, int width, int height, std::vector<std::uint8_t> premultiplied);

}  // namespace mayo::render
