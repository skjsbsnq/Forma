#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace forma::polkit {
struct State {
    bool active = false, waiting = false, echo = false, canRetry = false, registered = false;
    // Prompt text may legally be empty; only this flag determines whether to accept input.
    bool awaitingResponse = false;
    std::string message, prompt, error, iconName, actionId, status, info;
    std::vector<std::string> identities;
    int selected = 0;
};
// All calls and notifications run on the default GLib main context. Pump that
// context from the UI loop. Never retain responses in UI state after respond().
class Backend {
public:
    struct Impl;
    explicit Backend(std::function<void(const State&)> changed);
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    void start();
    void selectIdentity(int index);
    void respond(std::string response);
    void retry();
    void cancel();
    void shutdown();
    const State& state() const;

private:
    friend struct BackendTestAccess;
    std::shared_ptr<Impl> impl_;
};
}  // namespace forma::polkit
