#include "capture.hpp"
#include <png.h>
#include <iostream>
#include <stdexcept>
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        shot::Image src{8, 6, 1.5, std::vector<std::uint8_t>(8 * 6 * 4)};
        for (size_t i = 0; i < src.rgba.size(); ++i) src.rgba[i] = static_cast<std::uint8_t>(i);
        auto a = shot::crop(src, 6, 4, 20, 20);
        check(a.width == 2 && a.height == 2, "crop boundary clamp");
        check(a.rgba[0] == src.rgba[(4 * 8 + 6) * 4], "crop origin pixel");
        auto b = shot::cropLogical(src, 1, 1, 2, 2, 8 / 1.5, 6 / 1.5);
        check(b.width == 3 && b.height == 3, "150% selection dimensions");
        check(b.rgba[0] == src.rgba[(2 * 8 + 2) * 4], "150% selection origin");
        auto bytes = shot::png(b);
        png_image p{};
        p.version = PNG_IMAGE_VERSION;
        check(png_image_begin_read_from_memory(&p, bytes.data(), bytes.size()), "PNG header");
        p.format = PNG_FORMAT_RGBA;
        std::vector<std::uint8_t> decoded(PNG_IMAGE_SIZE(p));
        check(png_image_finish_read(&p, nullptr, decoded.data(), 0, nullptr), "PNG decode");
        check(p.width == 3 && p.height == 3 && decoded == b.rgba, "PNG lossless roundtrip");
        png_image_free(&p);
        std::cout << "Screenshot crop, scale mapping, PNG roundtrip passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
