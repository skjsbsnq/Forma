#pragma once
#include <functional>
#include <string>
#include <thread>
namespace shot {
struct Request {
    int mode = 0;
    bool background = false, quit = false;
    std::string activationToken;
};
class Session {
public:
    static bool send(Request request);
    explicit Session(std::function<void(Request)> receive);
    ~Session();
    bool owns() const { return m_fd >= 0; }

private:
    int m_lock = -1, m_fd = -1;
    std::string m_path;
    std::jthread m_thread;
};
}  // namespace shot
