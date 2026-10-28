#include "mayonnaise/widgets/image.hpp"

#include <algorithm>

#include "../render/image_io.hpp"
#include "../render/renderer2d.hpp"
#include "../render/rhi/rhi.hpp"
#include "mayonnaise/painter.hpp"

namespace mayo {

struct ImageData::Impl {
    render::DecodedImage pixels;
    int originalW = 0;
    int originalH = 0;
    bool retain = false;   // static images drop their CPU copy once uploaded
    bool dynamic = false;  // updated every frame: no mipmaps, sub-image uploads
    bool dirty = false;

    void fit(int maxDim) {
        originalW = pixels.width;
        originalH = pixels.height;
        if (maxDim <= 0 || (pixels.width <= maxDim && pixels.height <= maxDim)) return;
        float k = static_cast<float>(maxDim) / static_cast<float>(std::max(pixels.width, pixels.height));
        render::resizeBox(pixels, std::max(1, static_cast<int>(static_cast<float>(pixels.width) * k + 0.5f)),
                          std::max(1, static_cast<int>(static_cast<float>(pixels.height) * k + 0.5f)));
    }
    std::unique_ptr<rhi::Texture> texture;  // may be destroyed on any thread; the RHI defers the GPU side

    rhi::Texture* gpuTexture() {
        rhi::Device* device = rhi::device();
        if (!device) return nullptr;
        if (texture && dirty && !pixels.pixels().empty()) {
            device->upload(*texture, 0, 0, pixels.width, pixels.height, pixels.pixels().data());
            dirty = false;
            return texture.get();
        }
        if (texture || pixels.pixels().empty()) return texture.get();
        dirty = false;
        // Clamp to what the GPU can sample (rare: > 16k photos).
        const int maxSize = device->maxTextureSize();
        if (pixels.width > maxSize || pixels.height > maxSize) {
            float k = static_cast<float>(maxSize) / static_cast<float>(std::max(pixels.width, pixels.height));
            render::resizeBox(pixels, static_cast<int>(static_cast<float>(pixels.width) * k),
                              static_cast<int>(static_cast<float>(pixels.height) * k));
        }
        texture = device->createTexture({.width = pixels.width, .height = pixels.height, .mipmaps = !dynamic});
        device->upload(*texture, 0, 0, pixels.width, pixels.height, pixels.pixels().data());
        if (!retain) {
            pixels.clearPixels();
        }
        return texture.get();
    }
};

ImageData::ImageData() : m_impl(std::make_unique<Impl>()) {}

ImageData::~ImageData() = default;

std::shared_ptr<ImageData> ImageData::load(const std::string& path, int maxDimension) {
    std::shared_ptr<ImageData> img(new ImageData());
    if (!render::loadImageFile(path, img->m_impl->pixels)) return nullptr;
    img->m_impl->fit(maxDimension);
    return img;
}

void ImageData::setRetainPixels(bool retain) { m_impl->retain = retain; }

std::shared_ptr<ImageData> ImageData::create(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    std::shared_ptr<ImageData> img(new ImageData());
    auto& d = img->m_impl->pixels;
    d.width = width;
    d.height = height;
    d.setPixels(std::vector<std::uint8_t>(static_cast<size_t>(width) * height * 4, 0));
    auto pixels = d.pixels();
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    img->m_impl->dynamic = true;
    img->m_impl->retain = true;
    img->m_impl->fit(0);
    return img;
}

void ImageData::update(std::vector<std::uint8_t> rgba, bool premultiplied) {
    auto& d = m_impl->pixels;
    if (rgba.size() != static_cast<size_t>(d.width) * d.height * 4) return;
    if (!premultiplied) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            unsigned a = rgba[i + 3];
            for (size_t c = 0; c < 3; ++c) rgba[i + c] = static_cast<std::uint8_t>((rgba[i + c] * a + 127) / 255);
        }
    }
    d.setPixels(std::move(rgba));
    m_impl->dynamic = true;
    m_impl->retain = true;
    m_impl->dirty = true;
}

std::shared_ptr<ImageData> ImageData::scaledToFit(int maxDimension) const {
    if (m_impl->pixels.pixels().empty()) return nullptr;
    std::shared_ptr<ImageData> img(new ImageData());
    img->m_impl->pixels = m_impl->pixels;
    img->m_impl->fit(maxDimension);
    img->m_impl->originalW = m_impl->originalW;
    img->m_impl->originalH = m_impl->originalH;
    return img;
}

int ImageData::originalWidth() const { return m_impl->originalW; }
int ImageData::originalHeight() const { return m_impl->originalH; }

std::shared_ptr<ImageData> ImageData::decode(std::span<const std::uint8_t> encoded) {
    std::shared_ptr<ImageData> img(new ImageData());
    if (!render::decodeImage(encoded, img->m_impl->pixels)) return nullptr;
    img->m_impl->fit(0);
    return img;
}

std::shared_ptr<ImageData> ImageData::fromPixels(int width, int height, std::span<const std::uint8_t> rgba) {
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4)
        return nullptr;
    std::shared_ptr<ImageData> img(new ImageData());
    auto& d = img->m_impl->pixels;
    d.width = width;
    d.height = height;
    d.setPixels({rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(width) * height * 4});
    auto pixels = d.pixels();
    for (size_t i = 0; i < pixels.size(); i += 4) {
        unsigned a = pixels[i + 3];
        for (size_t c = 0; c < 3; ++c) pixels[i + c] = static_cast<std::uint8_t>((pixels[i + c] * a + 127) / 255);
    }
    img->m_impl->fit(0);
    return img;
}

int ImageData::width() const { return m_impl->pixels.width; }
int ImageData::height() const { return m_impl->pixels.height; }

void Painter::drawImage(const Rect& r, const ImageData& image, const CornerRadii& radii, float opacity,
                        const Rect& source) {
    rhi::Texture* tex = image.impl().gpuTexture();
    if (!tex) return;
    m_renderer.image(r, tex, source, radii, opacity);
}

ImageView::ImageView(std::shared_ptr<ImageData> data) : m_data(std::move(data)) {}

void ImageView::setData(std::shared_ptr<ImageData> data) {
    m_data = std::move(data);
    invalidateLayout();
}

void ImageView::setContentMode(ContentMode mode) {
    m_mode = mode;
    invalidate();
}

void ImageView::setSourceScale(float scale) {
    m_sourceScale = std::max(0.01f, scale);
    invalidateLayout();
}

Size ImageView::measureContent(Size available) {
    if (!m_data) return {};
    Size natural{static_cast<float>(m_data->width()) / m_sourceScale,
                 static_cast<float>(m_data->height()) / m_sourceScale};
    // Shrink to fit the offered width, keeping the aspect ratio.
    if (std::isfinite(available.width) && natural.width > available.width && natural.width > 0) {
        float k = available.width / natural.width;
        natural = {natural.width * k, natural.height * k};
    }
    return natural;
}

void ImageView::draw(Painter& p) {
    if (!m_data || m_data->width() <= 0) return;
    const Rect box = contentRect();
    const float iw = static_cast<float>(m_data->width());
    const float ih = static_cast<float>(m_data->height());
    Rect target = box;
    Rect source{0, 0, 1, 1};
    if (m_mode == ContentMode::Fit) {
        float k = std::min(box.width / iw, box.height / ih);
        target = {box.center().x - iw * k * 0.5f, box.center().y - ih * k * 0.5f, iw * k, ih * k};
    } else if (m_mode == ContentMode::Fill) {
        // Crop in texture space so rounded corners stay on the visible rect.
        float k = std::max(box.width / iw, box.height / ih);
        float sw = box.width / (iw * k);
        float sh = box.height / (ih * k);
        source = {(1 - sw) * 0.5f, (1 - sh) * 0.5f, sw, sh};
    }
    p.drawImage(target, *m_data, cornerRadius(), 1.0f, source);
}

}  // namespace mayo
