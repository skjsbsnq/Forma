#include "capture.hpp"
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <png.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
namespace shot {
Image capture(bool window, std::stop_token stop) {
    if (stop.stop_requested()) throw CaptureCancelled();
    std::unique_ptr<GCancellable, decltype(&g_object_unref)> cancellable(g_cancellable_new(), g_object_unref);
    std::stop_callback cancelCallback(stop, [&] { g_cancellable_cancel(cancellable.get()); });
    int fd[2];
    if (pipe2(fd, O_CLOEXEC)) throw std::runtime_error("Cannot create screenshot pipe");
    auto reader = std::async(std::launch::async, [r = fd[0], stop] {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(16 * 1024 * 1024);
        std::uint8_t b[65536];
        ssize_t n;
        while (!stop.stop_requested()) {
            pollfd ready{r, POLLIN, 0};
            int result = poll(&ready, 1, 100);
            if (result < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (result == 0) continue;
            n = read(r, b, sizeof(b));
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            bytes.insert(bytes.end(), b, b + n);
        }
        close(r);
        return bytes;
    });
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable.get(), &error);
    if (!bus) {
        close(fd[1]);
        reader.get();
        bool cancelled = stop.stop_requested() || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        std::string msg = error->message;
        g_error_free(error);
        if (cancelled) throw CaptureCancelled();
        throw std::runtime_error(msg);
    }
    GUnixFDList* list = g_unix_fd_list_new();
    int index = g_unix_fd_list_append(list, fd[1], nullptr);
    close(fd[1]);
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "native-resolution", g_variant_new_boolean(true));
    g_variant_builder_add(&options, "{sv}", "include-decoration", g_variant_new_boolean(true));
    GVariant* args = window ? g_variant_new("(u@a{sv}h)", 0u, g_variant_builder_end(&options), index)
                            : g_variant_new("(@a{sv}h)", g_variant_builder_end(&options), index);
    GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, "org.kde.KWin.ScreenShot2", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2",
        window ? "CaptureInteractive" : "CaptureActiveScreen", args, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
        120000, list, nullptr, cancellable.get(), &error);
    g_object_unref(list);
    g_object_unref(bus);
    auto bytes = reader.get();
    if (stop.stop_requested()) {
        if (reply) g_variant_unref(reply);
        if (error) g_error_free(error);
        throw CaptureCancelled();
    }
    if (!reply) {
        gchar* remote = error ? g_dbus_error_get_remote_error(error) : nullptr;
        bool cancelled = stop.stop_requested() || (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) ||
                         (remote && (g_str_has_suffix(remote, ".Cancelled") || g_str_has_suffix(remote, ".Canceled")));
        g_free(remote);
        std::string msg = error ? error->message : "Screenshot failed";
        if (error) g_error_free(error);
        if (cancelled) throw CaptureCancelled();
        throw std::runtime_error(msg);
    }
    GVariant* dict = g_variant_get_child_value(reply, 0);
    guint32 width = 0, height = 0, stride = 0, format = 0;
    double scale = 1;
    g_variant_lookup(dict, "width", "u", &width);
    g_variant_lookup(dict, "height", "u", &height);
    g_variant_lookup(dict, "stride", "u", &stride);
    g_variant_lookup(dict, "format", "u", &format);
    g_variant_lookup(dict, "scale", "d", &scale);
    g_variant_unref(dict);
    g_variant_unref(reply);
    if (!width || !height || width > 32768 || height > 32768 || stride < width * 4 ||
        bytes.size() != static_cast<size_t>(stride) * height)
        throw std::runtime_error("Invalid screenshot buffer from KWin");
    if (format != 4 && format != 5 && format != 16 && format != 17 && format != 18)
        throw std::runtime_error("Unsupported KWin image format: " + std::to_string(format));
    if ((format == 16 || format == 17) && stride == width * 4) {
        if (format == 16)
            for (size_t alpha = 3; alpha < bytes.size(); alpha += 4) bytes[alpha] = 255;
        return {static_cast<int>(width), static_cast<int>(height), scale, std::move(bytes)};
    }
    Image image{static_cast<int>(width), static_cast<int>(height), scale,
                std::vector<std::uint8_t>(static_cast<size_t>(width) * height * 4)};
    for (guint32 y = 0; y < height; ++y)
        for (guint32 x = 0; x < width; ++x) {
            const auto* s = &bytes[static_cast<size_t>(y) * stride + x * 4];
            auto* d = &image.rgba[(static_cast<size_t>(y) * width + x) * 4];
            bool bgra = format == 4 || format == 5;
            d[0] = s[bgra ? 2 : 0];
            d[1] = s[1];
            d[2] = s[bgra ? 0 : 2];
            d[3] = format == 16 ? 255 : s[3];
            if ((format == 5 || format == 18) && d[3] > 0)
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<std::uint8_t>(std::min(255, int(d[c]) * 255 / int(d[3])));
        }
    return image;
}
Image crop(const Image& src, int x, int y, int w, int h) {
    x = std::clamp(x, 0, src.width - 1);
    y = std::clamp(y, 0, src.height - 1);
    w = std::clamp(w, 1, src.width - x);
    h = std::clamp(h, 1, src.height - y);
    Image out{w, h, src.scale, std::vector<std::uint8_t>(static_cast<size_t>(w) * h * 4)};
    for (int row = 0; row < h; ++row)
        std::copy_n(src.rgba.data() + (static_cast<size_t>(y + row) * src.width + x) * 4, static_cast<size_t>(w) * 4,
                    out.rgba.data() + static_cast<size_t>(row) * w * 4);
    return out;
}
Image cropLogical(const Image& image, double x, double y, double width, double height, double viewWidth,
                  double viewHeight) {
    if (viewWidth <= 0 || viewHeight <= 0) throw std::runtime_error("Invalid selection surface");
    int left = static_cast<int>(std::round(x * image.width / viewWidth));
    int top = static_cast<int>(std::round(y * image.height / viewHeight));
    int right = static_cast<int>(std::round((x + width) * image.width / viewWidth));
    int bottom = static_cast<int>(std::round((y + height) * image.height / viewHeight));
    return crop(image, left, top, right - left, bottom - top);
}
namespace {
struct PngOutput {
    std::vector<std::uint8_t> bytes;
    char error[256]{};
};
void pngFailure(png_structp writer, png_const_charp message) {
    auto* output = static_cast<PngOutput*>(png_get_error_ptr(writer));
    if (output) {
        std::strncpy(output->error, message, sizeof(output->error) - 1);
    }
    png_longjmp(writer, 1);
}
void pngWarning(png_structp, png_const_charp) {}
void pngWrite(png_structp writer, png_bytep bytes, png_size_t count) {
    auto* output = static_cast<PngOutput*>(png_get_io_ptr(writer));
    bool failed = false;
    try {
        output->bytes.insert(output->bytes.end(), bytes, bytes + count);
    } catch (...) {
        failed = true;
    }
    if (failed) png_error(writer, "Not enough memory to encode screenshot");
}
}  // namespace
std::vector<std::uint8_t> png(const Image& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4)
        throw std::runtime_error("Invalid screenshot image");
    auto output = std::make_unique<PngOutput>();
    output->bytes.reserve(image.rgba.size() / 4);
    png_structp writer = png_create_write_struct(PNG_LIBPNG_VER_STRING, output.get(), pngFailure, pngWarning);
    if (!writer) throw std::runtime_error("Cannot create PNG encoder");
    png_infop info = png_create_info_struct(writer);
    if (!info) {
        png_destroy_write_struct(&writer, nullptr);
        throw std::runtime_error("Cannot create PNG metadata");
    }
    if (setjmp(png_jmpbuf(writer))) {
        png_destroy_write_struct(&writer, &info);
        throw std::runtime_error(output->error[0] ? output->error : "Cannot encode screenshot");
    }
    png_set_write_fn(writer, output.get(), pngWrite, nullptr);
    png_set_IHDR(writer, info, static_cast<png_uint_32>(image.width), static_cast<png_uint_32>(image.height), 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    // Interactive screenshots favor latency. SUB handles flat UI regions without
    // testing five filters per row; zlib level 1 retains lossless pixels.
    png_set_compression_level(writer, 1);
    png_set_filter(writer, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
    png_write_info(writer, info);
    for (int y = 0; y < image.height; ++y)
        png_write_row(writer, const_cast<png_bytep>(image.rgba.data() + static_cast<size_t>(y) * image.width * 4));
    png_write_end(writer, info);
    png_destroy_write_struct(&writer, &info);
    return std::move(output->bytes);
}
void save(const std::vector<std::uint8_t>& bytes, const std::string& path) {
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("Cannot save " + path);
    out.close();
    if (!out) throw std::runtime_error("Cannot finish saving " + path);
    gchar* notify = g_find_program_in_path("notify-send");
    if (notify) {
        const gchar* args[] = {notify,
                               "--app-name=Forma Screenshot",
                               "--icon=io.github.skjsbsnq.Forma.Screenshot",
                               "截图已保存",
                               path.c_str(),
                               nullptr};
        GSubprocess* proc = g_subprocess_newv(
            args, static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
            nullptr);
        if (proc) g_object_unref(proc);
        g_free(notify);
    }
}
void copy(const std::vector<std::uint8_t>& bytes) {
    gchar* helper = g_find_program_in_path("wl-copy");
    if (!helper) throw std::runtime_error("Image clipboard requires wl-copy (wl-clipboard)");
    GError* err = nullptr;
    GSubprocess* child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE, &err, helper, "--type", "image/png", nullptr);
    g_free(helper);
    if (!child) {
        std::string msg = err->message;
        g_error_free(err);
        throw std::runtime_error(msg);
    }
    GBytes* input = g_bytes_new_static(bytes.data(), bytes.size());
    gboolean ok = g_subprocess_communicate(child, input, nullptr, nullptr, nullptr, &err);
    g_bytes_unref(input);
    if (ok) ok = g_subprocess_get_successful(child);
    g_object_unref(child);
    if (!ok) {
        std::string msg = err ? err->message : "wl-copy failed";
        if (err) g_error_free(err);
        throw std::runtime_error(msg);
    }
    gchar* notify = g_find_program_in_path("notify-send");
    if (notify) {
        const gchar* args[] = {notify,
                               "--app-name=Forma 截图",
                               "--icon=io.github.skjsbsnq.Forma.Screenshot",
                               "--expire-time=1800",
                               "截图已复制",
                               "可以直接粘贴到其他应用",
                               nullptr};
        GSubprocess* process = g_subprocess_newv(
            args, static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
            nullptr);
        if (process) g_object_unref(process);
        g_free(notify);
    }
}
std::string defaultDirectory() {
    const char* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    return std::string(pictures ? pictures : g_get_home_dir()) + "/Screenshots";
}
}  // namespace shot
