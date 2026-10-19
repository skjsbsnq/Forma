#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mayonnaise/mayonnaise.hpp>
#include <sstream>
#include <thread>
#include "capture_view.hpp"
#include "session.hpp"
using namespace mayo;
namespace {
struct Options {
    shot::Mode mode = shot::Mode::Region;
    bool copy = false, save = false, background = false, quit = false, once = false;
    std::string directory = shot::defaultDirectory(), output;
};
std::string filename(const Options& options) {
    if (!options.output.empty()) return options.output;
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::ostringstream path;
    path << options.directory << "/Screenshot_" << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << '_'
         << std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000 << ".png";
    return path.str();
}
class CaptureController {
public:
    CaptureController(App& app, Options options, bool resident)
        : m_app(app), m_options(std::move(options)), m_resident(resident) {
        m_view = std::make_shared<shot::CaptureView>();
        m_window = &app.createWindow({.title = "Forma 截图",
                                      .width = 960,
                                      .height = 640,
                                      .minWidth = 320,
                                      .minHeight = 200,
                                      .initiallyHidden = true});
        m_window->setContent(m_view);
        m_window->setFullscreen(true);
        m_window->onKey([this](const KeyEvent& key) { return m_view->key(key); });
        m_window->onCloseRequested([this] {
            cancel();
            return false;
        });
        m_view->onOutput = [this](bool copy) { output(copy, m_options.copy && m_options.save); };
        m_view->onCancel = [this] { cancel(); };
        m_view->onMode = [this](shot::Mode mode) {
            if (m_finishing || m_capturing) return;
            if (mode == m_view->mode()) return;
            if (mode != shot::Mode::Window && m_view->mode() != shot::Mode::Window) {
                m_options.mode = mode;
                m_view->setMode(mode);
            } else
                activate(mode, true);
        };
        if (m_options.copy || m_options.save)
            m_view->onSelected = [this] { output(m_options.copy, m_options.copy && m_options.save); };
    }
    ~CaptureController() {
        m_capture.request_stop();
        if (m_capture.joinable()) m_capture.join();
        if (m_output.joinable()) m_output.join();
    }
    void activate(shot::Mode mode, bool force = false, bool launcherFocus = false) {
        if (m_finishing || m_capturing) return;
        if (!force && m_window->isVisible()) {
            if (mode != m_view->mode()) {
                if (mode == shot::Mode::Window || m_view->mode() == shot::Mode::Window) {
                    activate(mode, true);
                    return;
                }
                m_options.mode = mode;
                m_view->setMode(mode);
            }
            // SDL consumes the launcher's Wayland token while mapping a window.
            if (launcherFocus) {
                m_window->hide();
                m_window->show();
            }
            m_window->raise();
            return;
        }
        m_options.mode = mode;
        m_window->hide();
        m_capturing = true;
        auto generation = ++m_generation;
        // Keep the renderer, text atlas and window warm. Only capture pixels on a worker.
        m_capture = std::jthread([this, mode, generation](std::stop_token stop) {
            try {
                auto image = shot::capture(mode == shot::Mode::Window, stop);
                if (stop.stop_requested()) return;
                m_app.post([this, generation, mode, image = std::move(image)]() mutable {
                    if (generation != m_generation) return;
                    m_capturing = false;
                    m_view->present(std::move(image), mode);
                    m_window->show();
                    if (const char* demo = std::getenv("MAYO_DEMO"); demo && std::string(demo) == "selection")
                        m_app.after(.10, [this, generation] {
                            if (generation == m_generation) m_view->demoSelection();
                        });
                    if (const char* action = std::getenv("MAYO_TEST_ACTION")) {
                        bool copy = std::string(action) == "copy";
                        m_app.after(.6, [this, generation, copy] {
                            if (generation == m_generation) output(copy, false);
                        });
                    }
                });
            } catch (const shot::CaptureCancelled&) {
                m_app.post([this, generation] {
                    if (generation == m_generation) {
                        m_capturing = false;
                        if (!m_resident) m_app.quit();
                    }
                });
            } catch (const std::exception& error) {
                std::string message = error.what();
                m_app.post([this, generation, message] {
                    if (generation != m_generation) return;
                    m_capturing = false;
                    std::cerr << "Forma 截图：" << message << '\n';
                    if (!m_resident) m_app.quit(1);
                });
            }
        });
    }
    void cancel() {
        if (m_finishing) return;
        ++m_generation;
        m_capture.request_stop();
        m_capturing = false;
        if (!m_window->isVisible()) {
            if (!m_resident) m_app.quit();
            return;
        }
        m_view->disappear();
        m_finishing = true;
        m_app.after(.12, [this] {
            m_window->hide();
            m_finishing = false;
            if (!m_resident) m_app.quit();
        });
    }

private:
    void output(bool copy, bool both) {
        if (m_finishing || !m_view->hasSelection()) return;
        m_finishing = true;
        m_view->setBusy(true);
        m_view->disappear();
        auto image = m_view->result();
        auto path = filename(m_options);
        auto generation = m_generation;
        m_app.after(.12, [this, generation] {
            if (generation == m_generation && m_finishing) m_window->hide();
        });
        m_output = std::jthread([this, generation, image = std::move(image), path = std::move(path), copy, both] {
            try {
                auto bytes = shot::png(image);
                if (copy || both) shot::copy(bytes);
                if (!copy || both) {
                    shot::save(bytes, path);
                    std::cout << path << '\n';
                }
                m_app.post([this, generation] {
                    m_app.after(.12, [this, generation] {
                        if (generation != m_generation) return;
                        m_window->hide();
                        m_finishing = false;
                        if (!m_resident) m_app.quit();
                    });
                });
            } catch (const std::exception& error) {
                std::string message = error.what();
                m_app.post([this, generation, message] {
                    if (generation != m_generation) return;
                    m_finishing = false;
                    m_view->resume();
                    m_view->setMessage("操作未完成：" + message);
                    m_window->show();
                });
            }
        });
    }
    App& m_app;
    Options m_options;
    bool m_resident = false, m_capturing = false, m_finishing = false;
    unsigned m_generation = 0;
    Window* m_window = nullptr;
    std::shared_ptr<shot::CaptureView> m_view;
    std::jthread m_capture, m_output;
};
}  // namespace
int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--region")
            options.mode = shot::Mode::Region;
        else if (a == "--fullscreen")
            options.mode = shot::Mode::Fullscreen;
        else if (a == "--window")
            options.mode = shot::Mode::Window;
        else if (a == "--copy")
            options.copy = true;
        else if (a == "--save")
            options.save = true;
        else if (a == "--background")
            options.background = true;
        else if (a == "--quit")
            options.quit = true;
        else if (a == "--once")
            options.once = true;
        else if (a == "--output" && i + 1 < argc) {
            options.output = argv[++i];
            options.save = true;
        } else if (a == "--save-dir" && i + 1 < argc)
            options.directory = argv[++i];
        else if (a == "--help") {
            std::cout
                << "Forma 截图\n--region / --fullscreen / --window\n--copy / --save / --output FILE / --save-dir PATH\n"
                   "--background 常驻预热；--quit 退出常驻；--once 单次运行\n"
                   "选区内拖动移动，边缘缩放，Shift约束正方形，双击/Enter复制，S保存，Esc取消。\n";
            return 0;
        } else {
            std::cerr << "未知参数：" << a << '\n';
            return 2;
        }
    }
    if (options.background &&
        (options.once || options.copy || options.save || options.directory != shot::defaultDirectory())) {
        std::cerr << "--background 不能与单次截图或输出选项同时使用。\n";
        return 2;
    }
    bool resident = !options.once && !options.copy && !options.save && options.directory == shot::defaultDirectory() &&
                    !std::getenv("MAYO_SCREENSHOT") && !std::getenv("MAYO_TEST_ACTION");
    const char* activationToken = std::getenv("XDG_ACTIVATION_TOKEN");
    shot::Request request{static_cast<int>(options.mode), options.background, options.quit,
                          activationToken ? activationToken : ""};
    if ((resident || options.quit) && shot::Session::send(request)) return 0;
    if (options.quit) return 0;
    try {
        if (options.mode != shot::Mode::Region && (options.copy || options.save)) {
            auto bytes = shot::png(shot::capture(options.mode == shot::Mode::Window));
            if (options.copy) shot::copy(bytes);
            if (options.save) {
                auto path = filename(options);
                shot::save(bytes, path);
                std::cout << path << '\n';
            }
            return 0;
        }
        App app("forma-screenshot", MAYO_DESKTOP_ID);
        app.setDefaultAppearanceMode(AppearanceMode::Light);
        app.setKeepAlive(true);
        CaptureController controller(app, options, resident);
        std::unique_ptr<shot::Session> session;
        if (resident) {
            session = std::make_unique<shot::Session>([&](shot::Request r) {
                app.post([&, r] {
                    if (r.quit)
                        app.quit();
                    else if (!r.background) {
                        app.setActivationToken(r.activationToken);
                        controller.activate(static_cast<shot::Mode>(r.mode), false, !r.activationToken.empty());
                    }
                });
            });
            if (!session->owns()) {
                for (int retry = 0; retry < 20; ++retry) {
                    if (shot::Session::send(request)) return 0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                throw std::runtime_error("无法连接截图常驻进程");
            }
        }
        if (!options.background) app.post([&] { controller.activate(options.mode); });
        return app.run();
    } catch (const shot::CaptureCancelled&) {
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Forma 截图：" << error.what() << '\n';
        return 1;
    }
}
