#include "session.hpp"
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace shot {
namespace {
std::string socketPath() {
    const char* r = std::getenv("XDG_RUNTIME_DIR");
    return std::string(r ? r : "/tmp") + "/forma-capture-" + std::to_string(getuid()) + ".sock";
}
sockaddr_un address(const std::string& path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() < sizeof(a.sun_path)) std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return a;
}
}  // namespace
bool Session::send(Request request) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    auto a = address(socketPath());
    char message[3] = {'F', static_cast<char>('0' + request.mode),
                       static_cast<char>(request.quit         ? 'Q'
                                         : request.background ? 'B'
                                                              : 'S')};
    std::string packet(message, sizeof(message));
    packet += request.activationToken;
    bool sent = sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) ==
                static_cast<ssize_t>(packet.size());
    close(fd);
    return sent;
}
Session::Session(std::function<void(Request)> receive, int inheritedSocket, int inheritedLock)
    : m_path(socketPath()), m_receive(std::move(receive)) {
    if (inheritedSocket >= 0 && inheritedLock >= 0) {
        sockaddr_un a{};
        socklen_t length = sizeof(a);
        struct stat lockStat{}, pathStat{};
        // Only adopt our own still-locked endpoint; never close arbitrary caller-supplied FDs.
        if (getsockname(inheritedSocket, reinterpret_cast<sockaddr*>(&a), &length) == 0 && a.sun_family == AF_UNIX &&
            std::string(a.sun_path, strnlen(a.sun_path, sizeof(a.sun_path))) == m_path &&
            fstat(inheritedLock, &lockStat) == 0 && stat((m_path + ".lock").c_str(), &pathStat) == 0 &&
            lockStat.st_dev == pathStat.st_dev && lockStat.st_ino == pathStat.st_ino && lockStat.st_uid == getuid() &&
            flock(inheritedLock, LOCK_EX | LOCK_NB) == 0) {
            m_fd = inheritedSocket;
            m_lock = inheritedLock;
            fcntl(m_fd, F_SETFD, FD_CLOEXEC);
            fcntl(m_lock, F_SETFD, FD_CLOEXEC);
            resume();
        }
        return;
    }
    m_lock = open((m_path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (m_lock < 0 || flock(m_lock, LOCK_EX | LOCK_NB) != 0) return;
    m_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0) return;
    unlink(m_path.c_str());
    auto a = address(m_path);
    if (bind(m_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        close(m_fd);
        m_fd = -1;
        return;
    }
    chmod(m_path.c_str(), 0600);
    resume();
}
void Session::pause() {
    m_thread.request_stop();
    if (m_thread.joinable()) m_thread.join();
}
void Session::resume() {
    if (m_fd < 0 || m_thread.joinable()) return;
    m_thread = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            pollfd p{m_fd, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            char message[4096]{};
            auto n = recv(m_fd, message, sizeof(message), 0);
            if (n < 3 || message[0] != 'F' || message[1] < '0' || message[1] > '2') continue;
            m_receive({message[1] - '0', message[2] == 'B', message[2] == 'Q',
                       std::string(message + 3, static_cast<size_t>(n - 3))});
        }
    });
}
void Session::restart() {
    // Graphics drivers, font libraries and allocator arenas can retain memory after
    // their UI objects are destroyed. exec releases all of it without changing PID
    // or dropping the public socket, its lock, or requests queued during the handoff.
    const auto socket = std::to_string(m_fd), lock = std::to_string(m_lock);
    char* args[] = {const_cast<char*>("forma-screenshot"), const_cast<char*>("--background"),
                    const_cast<char*>("--resume-session"), const_cast<char*>(socket.c_str()),
                    const_cast<char*>(lock.c_str()),       nullptr};
    if (fcntl(m_fd, F_SETFD, 0) == 0 && fcntl(m_lock, F_SETFD, 0) == 0) execv("/proc/self/exe", args);
    std::perror("Forma screenshot: resident restart failed");
    fcntl(m_fd, F_SETFD, FD_CLOEXEC);
    fcntl(m_lock, F_SETFD, FD_CLOEXEC);
    resume();
}
Session::~Session() {
    pause();
    if (m_fd >= 0) {
        close(m_fd);
        unlink(m_path.c_str());
    }
    if (m_lock >= 0) close(m_lock);
}
}  // namespace shot
