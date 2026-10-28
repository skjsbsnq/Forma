#include <doctest/doctest.h>

#include <stb_image_write.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "render/image_io.hpp"

using namespace mayo;

namespace {
void sink(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + size);
}

/// 4x2 JPEG whose left half is red and right half blue, optionally tagged with an EXIF orientation.
std::vector<std::uint8_t> makeJpeg(int orientation) {
    const int w = 16;
    const int h = 8;
    std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 3];
            p[0] = x < w / 2 ? 255 : 0;
            p[1] = 0;
            p[2] = x < w / 2 ? 0 : 255;
        }
    }
    std::vector<std::uint8_t> out;
    stbi_write_jpg_to_func(sink, &out, w, h, 3, px.data(), 100);
    if (orientation) {
        std::vector<std::uint8_t> exif = {
            0xFF, 0xE1, 0, 34, 'E', 'x',  'i',  'f', 0, 0, 'M', 'M', 0, 42, 0,
            0,    0,    8, 0,  1,   0x01, 0x12, 0,   3, 0, 0,   0,   1, 0,  static_cast<std::uint8_t>(orientation),
            0,    0,    0, 0,  0,   0};
        out.insert(out.begin() + 2, exif.begin(), exif.end());
    }
    return out;
}

bool reddish(const render::DecodedImage& img, int x, int y) {
    const std::uint8_t* p = &img.pixels()[(static_cast<size_t>(y) * img.width + x) * 4];
    return p[0] > 200 && p[2] < 60;
}
}  // namespace

TEST_CASE("EXIF orientation is read and applied") {
    auto plain = makeJpeg(0);
    CHECK(render::exifOrientation(plain) == 1);
    render::DecodedImage img;
    REQUIRE(render::decodeImage(plain, img));
    CHECK(img.width == 16);
    CHECK(reddish(img, 1, 1));

    auto rotated = makeJpeg(6);  // "rotate 90° clockwise to display"
    CHECK(render::exifOrientation(rotated) == 6);
    REQUIRE(render::decodeImage(rotated, img));
    CHECK(img.width == 8);
    CHECK(img.height == 16);
    // The left (red) half of the stored image ends up on top.
    CHECK(reddish(img, 4, 1));
    CHECK_FALSE(reddish(img, 4, 14));

    auto flipped = makeJpeg(3);  // 180°
    REQUIRE(render::decodeImage(flipped, img));
    CHECK(img.width == 16);
    CHECK_FALSE(reddish(img, 1, 1));
    CHECK(reddish(img, 14, 1));
}

TEST_CASE("Box downscale averages and keeps aspect") {
    render::DecodedImage img;
    img.width = 4;
    img.height = 2;
    img.setPixels({255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255,
                   255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255});
    render::resizeBox(img, 2, 1);
    REQUIRE(img.pixels().size() == 8);
    CHECK(img.pixels()[0] == 255);
    CHECK(img.pixels()[2] == 0);
    CHECK(img.pixels()[4] == 0);
    CHECK(img.pixels()[6] == 255);
    render::resizeBox(img, 10, 10);  // never upscales
    CHECK(img.width == 2);
}

TEST_CASE("Decoded image copies stay independent across moves and pixel release") {
    render::DecodedImage source;
    REQUIRE(render::decodeImage(makeJpeg(0), source));
    const auto* decoded = source.pixels().data();
    const auto original = source.pixels()[0];

    render::DecodedImage copy(source);
    REQUIRE(copy.pixels().size() == source.pixels().size());
    CHECK(copy.pixels().data() != decoded);
    copy.pixels()[0] = static_cast<std::uint8_t>(original ^ 255);
    CHECK(source.pixels()[0] == original);

    render::DecodedImage assigned;
    assigned = source;
    CHECK(assigned.pixels().data() != decoded);
    CHECK(assigned.pixels()[0] == original);

    render::DecodedImage moved(std::move(source));
    CHECK(moved.pixels().data() == decoded);
    CHECK(source.pixels().empty());
    source = std::move(moved);
    CHECK(source.pixels().data() == decoded);
    CHECK(moved.pixels().empty());
    source.clearPixels();
    CHECK(source.pixels().empty());
    CHECK(assigned.pixels()[0] == original);
    REQUIRE(render::decodeImage(makeJpeg(6), source));
    render::resizeBox(source, 4, 8);
    CHECK(source.pixels().size() == 4 * 8 * 4);
    CHECK(reddish(source, 1, 1));
}

TEST_CASE("PNG decode premultiplies alpha and failed decode preserves previous pixels") {
    const std::vector<std::uint8_t> rgba{200, 100, 50, 128, 80, 40, 20, 0, 12, 34, 56, 255};
    std::vector<std::uint8_t> png;
    REQUIRE(stbi_write_png_to_func(sink, &png, 3, 1, 4, rgba.data(), 12));
    render::DecodedImage img;
    REQUIRE(render::decodeImage(png, img));
    CHECK(img.width == 3);
    CHECK(img.height == 1);
    const std::vector<std::uint8_t> expected{100, 50, 25, 128, 0, 0, 0, 0, 12, 34, 56, 255};
    CHECK(std::vector<std::uint8_t>(img.pixels().begin(), img.pixels().end()) == expected);
    const auto* decoded = img.pixels().data();
    const std::vector<std::uint8_t> invalid{1, 2, 3};
    CHECK_FALSE(render::decodeImage(invalid, img));
    CHECK(img.pixels().data() == decoded);
    CHECK(img.width == 3);
    CHECK(std::vector<std::uint8_t>(img.pixels().begin(), img.pixels().end()) == expected);
}
