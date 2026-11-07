#pragma once

// UI-free Linux process identity, rate calculation and safe signal delivery.

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string_view>

struct ProcessIdentity {
    pid_t pid = 0;
    std::uint64_t startTicks = 0;  // exact /proc/<pid>/stat field 22, never converted through double
    bool operator==(const ProcessIdentity&) const = default;
};

std::optional<ProcessIdentity> parseProcessIdentity(std::string_view stat);
std::optional<ProcessIdentity> readProcessIdentity(pid_t pid);

struct ProcessCounters {
    ProcessIdentity identity;
    std::uint64_t ticks = 0;
    std::uint64_t read = 0;
    std::uint64_t written = 0;
    bool ioKnown = false;
};

struct ProcessRates {
    double cpuPercent = 0;
    double readRate = 0;
    double writeRate = 0;
};

ProcessRates processRates(const ProcessCounters& now, const ProcessCounters* previous, double dt,
                          double ticksPerSecond);

enum class ProcessStatus { Ready, Exited, IdentityChanged, Unsupported, PermissionDenied, Unavailable, NotBound };

struct ProcessResult {
    ProcessStatus status = ProcessStatus::NotBound;
    int error = 0;
    explicit operator bool() const { return status == ProcessStatus::Ready; }
};

/// A confirmation owns one pidfd. Opening first, then verifying stat and liveness prevents PID reuse
/// between sampling and confirmation from targeting a different process. There is no kill(pid) fallback.
class ProcessTarget {
public:
    ProcessTarget() = default;
    ~ProcessTarget();
    ProcessTarget(const ProcessTarget&) = delete;
    ProcessTarget& operator=(const ProcessTarget&) = delete;
    ProcessTarget(ProcessTarget&& other) noexcept;
    ProcessTarget& operator=(ProcessTarget&& other) noexcept;

    ProcessResult bind(ProcessIdentity expected);
    [[nodiscard]] ProcessResult check() const;
    /// Consumes the binding on success or failure; another attempt needs a new confirmation.
    ProcessResult sendSignal(int signal);
    void reset();
    [[nodiscard]] bool isBound() const { return m_fd >= 0; }
    [[nodiscard]] ProcessIdentity identity() const { return m_identity; }

private:
    int m_fd = -1;
    ProcessIdentity m_identity;
};
