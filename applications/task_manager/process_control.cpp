#include "process_control.hpp"

#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <fstream>
#include <string>
#include <utility>

namespace {

std::string_view nextField(std::string_view& text) {
    const size_t first = text.find_first_not_of(" \t\n");
    if (first == std::string_view::npos) {
        text = {};
        return {};
    }
    text.remove_prefix(first);
    const size_t end = text.find_first_of(" \t\n");
    const auto field = text.substr(0, end);
    text.remove_prefix(field.size());
    return field;
}

template <typename T>
bool number(std::string_view field, T& value) {
    if (field.empty()) return false;
    const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
    return error == std::errc{} && end == field.data() + field.size();
}

ProcessResult failure(int error) {
    switch (error) {
        case ESRCH:
        case ENOENT: return {ProcessStatus::Exited, error};
        case ENOSYS: return {ProcessStatus::Unsupported, error};
        case EPERM:
        case EACCES: return {ProcessStatus::PermissionDenied, error};
        default: return {ProcessStatus::Unavailable, error};
    }
}

int openPidfd(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

int signalPidfd(int fd, int signal) {
#ifdef SYS_pidfd_send_signal
    return static_cast<int>(::syscall(SYS_pidfd_send_signal, fd, signal, nullptr, 0));
#else
    (void)fd;
    (void)signal;
    errno = ENOSYS;
    return -1;
#endif
}

}  // namespace

std::optional<ProcessIdentity> parseProcessIdentity(std::string_view stat) {
    const size_t open = stat.find('(');
    const size_t close = stat.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) return std::nullopt;
    auto prefix = stat.substr(0, open);
    ProcessIdentity identity;
    if (!number(nextField(prefix), identity.pid) || identity.pid <= 0 || !nextField(prefix).empty())
        return std::nullopt;
    stat.remove_prefix(close + 1);
    for (int field = 3; field < 22; ++field) {
        if (nextField(stat).empty()) return std::nullopt;
    }
    if (!number(nextField(stat), identity.startTicks)) return std::nullopt;
    return identity;
}

std::optional<ProcessIdentity> readProcessIdentity(pid_t pid) {
    if (pid <= 0) {
        errno = EINVAL;
        return std::nullopt;
    }
    errno = 0;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in) {
        if (!errno) errno = EIO;
        return std::nullopt;
    }
    std::string stat;
    // comm can contain newlines and ')' as well as spaces.
    stat.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        errno = EIO;
        return std::nullopt;
    }
    const auto identity = parseProcessIdentity(stat);
    if (!identity || identity->pid != pid) {
        errno = EIO;
        return std::nullopt;
    }
    return identity;
}

ProcessRates processRates(const ProcessCounters& now, const ProcessCounters* previous, double dt,
                          double ticksPerSecond) {
    ProcessRates rates;
    if (!previous || now.identity != previous->identity || !std::isfinite(dt) || dt <= 0 ||
        !std::isfinite(ticksPerSecond) || ticksPerSecond <= 0)
        return rates;
    rates.cpuPercent =
        static_cast<double>(now.ticks - std::min(now.ticks, previous->ticks)) / ticksPerSecond / dt * 100;
    if (now.ioKnown && previous->ioKnown) {
        rates.readRate = static_cast<double>(now.read - std::min(now.read, previous->read)) / dt;
        rates.writeRate = static_cast<double>(now.written - std::min(now.written, previous->written)) / dt;
    }
    return rates;
}

ProcessTarget::~ProcessTarget() { reset(); }

ProcessTarget::ProcessTarget(ProcessTarget&& other) noexcept
    : m_fd(std::exchange(other.m_fd, -1)), m_identity(std::exchange(other.m_identity, {})) {}

ProcessTarget& ProcessTarget::operator=(ProcessTarget&& other) noexcept {
    if (this != &other) {
        reset();
        m_fd = std::exchange(other.m_fd, -1);
        m_identity = std::exchange(other.m_identity, {});
    }
    return *this;
}

void ProcessTarget::reset() {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
    m_identity = {};
}

ProcessResult ProcessTarget::check() const {
    if (m_fd < 0) return {};
    pollfd descriptor{m_fd, POLLIN, 0};
    int result;
    do {
        result = ::poll(&descriptor, 1, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return failure(errno);
    if (descriptor.revents & (POLLIN | POLLHUP)) return {ProcessStatus::Exited, ESRCH};
    if (descriptor.revents != 0) return {ProcessStatus::Unavailable, EIO};
    return {ProcessStatus::Ready};
}

ProcessResult ProcessTarget::bind(ProcessIdentity expected) {
    reset();
    if (expected.pid <= 0) return {ProcessStatus::Unavailable, EINVAL};
    m_fd = openPidfd(expected.pid);
    if (m_fd < 0) return failure(errno);
    const auto actual = readProcessIdentity(expected.pid);
    ProcessResult result = actual ? ProcessResult{ProcessStatus::Ready} : failure(errno);
    if (actual && *actual != expected) result = {ProcessStatus::IdentityChanged};
    // The pidfd may refer to a process that exited before stat was read, even if its PID was reused.
    if (const auto live = check(); !live) result = live;
    if (result && signalPidfd(m_fd, 0) != 0) result = failure(errno);  // also require safe-send support
    if (!result)
        reset();
    else
        m_identity = expected;
    return result;
}

ProcessResult ProcessTarget::sendSignal(int signal) {
    ProcessResult result = check();
    if (result && signalPidfd(m_fd, signal) != 0) result = failure(errno);
    reset();
    return result;
}
