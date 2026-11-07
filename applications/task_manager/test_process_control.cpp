#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "process_control.hpp"
#include "system_stats.hpp"

#include <dirent.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>

namespace {

// Every signal test owns a freshly forked child. Closing the pipe is enough to clean up, so tests
// never need a raw-PID signal even on kernels without pidfd support.
class Child {
public:
    Child() {
        int pipeFds[2];
        if (::pipe(pipeFds) != 0) return;
        m_pid = ::fork();
        if (m_pid == 0) {
            // doctest's fatal-signal handler belongs to the test runner, not its disposable child.
            ::signal(SIGTERM, SIG_DFL);
            ::close(pipeFds[1]);
            char value;
            while (::read(pipeFds[0], &value, 1) < 0 && errno == EINTR) {
            }
            ::close(pipeFds[0]);
            ::_exit(0);
        }
        ::close(pipeFds[0]);
        if (m_pid < 0)
            ::close(pipeFds[1]);
        else
            m_release = pipeFds[1];
    }
    ~Child() {
        release();
        wait();
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    pid_t pid() const { return m_pid; }
    void release() {
        if (m_release >= 0) ::close(m_release);
        m_release = -1;
    }
    int wait() {
        if (m_pid <= 0 || m_waited) return m_status;
        pid_t result;
        do {
            result = ::waitpid(m_pid, &m_status, 0);
        } while (result < 0 && errno == EINTR);
        m_waited = result == m_pid;
        return m_status;
    }
    bool alive() const {
        siginfo_t info{};
        return ::waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0;
    }

private:
    pid_t m_pid = -1;
    int m_release = -1;
    int m_status = -1;
    bool m_waited = false;
};

int openDescriptors() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    while (const dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    ::closedir(directory);
    return count;
}

bool requireBinding(ProcessTarget& target, ProcessIdentity identity) {
    const auto result = target.bind(identity);
    if (result.status == ProcessStatus::Unsupported || result.status == ProcessStatus::PermissionDenied) {
        // These systems must refuse, rather than falling back to kill(pid).
        CHECK_FALSE(target.isBound());
        CHECK(target.sendSignal(SIGTERM).status == ProcessStatus::NotBound);
        MESSAGE("Safe pidfd operations are unavailable; real-send assertions skipped.");
        return false;
    }
    CHECK(result.status == ProcessStatus::Ready);
    return static_cast<bool>(result);
}

#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
// Simulate an old kernel only inside a disposable fork. The filter never affects the runner or desktop.
int unsupportedOperations(bool blockOpen) {
    const pid_t isolated = ::fork();
    if (isolated < 0) return -1;
    if (isolated == 0) {
        const int result = [blockOpen] {
            Child child;
            if (child.pid() <= 0) return 1;
            const auto identity = readProcessIdentity(child.pid());
            if (!identity) return 2;
            ProcessTarget target;
            if (!blockOpen && !target.bind(*identity)) return 77;
            const auto blocked = static_cast<std::uint32_t>(blockOpen ? SYS_pidfd_open : SYS_pidfd_send_signal);
            sock_filter instructions[] = {
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),  // seccomp_data.nr
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, blocked, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            sock_fprog filter{static_cast<unsigned short>(sizeof instructions / sizeof instructions[0]), instructions};
            if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
                ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter) != 0)
                return 77;
            const auto refused = blockOpen ? target.bind(*identity) : target.sendSignal(SIGTERM);
            if (refused.status != ProcessStatus::Unsupported || target.isBound()) return 3;
            if (target.sendSignal(SIGKILL).status != ProcessStatus::NotBound) return 4;
            if (!child.alive()) return 5;  // detects any unsafe raw-PID fallback
            // Send-support failure must also refuse a newly opened confirmation and close its pidfd.
            if (!blockOpen && (target.bind(*identity).status != ProcessStatus::Unsupported || target.isBound()))
                return 6;
            return 0;
        }();  // pipe cleanup and wait happen before _exit
        ::_exit(result);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(isolated, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == isolated && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

std::string statText(std::string_view pid, std::string_view name, std::string_view ticks) {
    // Fields 3..21, followed by the raw start time at field 22.
    return std::string(pid) + " (" + std::string(name) + ") S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 " +
           std::string(ticks) + " 0 0\n";
}

constexpr std::string_view kMeminfo =
    "MemTotal: 1000 kB\nMemAvailable: 100 kB\nBuffers: 10 kB\nShmem: 5 kB\n"
    "Cached: 150 kB\nSReclaimable: 20 kB\nSwapTotal: 200 kB\nSwapFree: 150 kB\n";

}  // namespace

TEST_CASE("Process identity retains raw ticks and accepts complex comm") {
    const auto identity = parseProcessIdentity(statText("42", "worker ) with (newline\n", "9007199254740993"));
    REQUIRE(identity.has_value());
    CHECK(identity->pid == 42);
    CHECK(identity->startTicks == 9007199254740993ULL);  // cannot round through double
    CHECK_FALSE(parseProcessIdentity("42 (short) S 1"));
    CHECK_FALSE(parseProcessIdentity(statText("0", "worker", "123")));
    CHECK_FALSE(parseProcessIdentity(statText("42x", "worker", "123")));
    CHECK_FALSE(parseProcessIdentity(statText("42", "worker", "123x")));
    CHECK_FALSE(parseProcessIdentity(statText("42", "worker", "-1")));
    CHECK_FALSE(parseProcessIdentity(statText("42", "worker", "18446744073709551616")));
}

TEST_CASE("PID identity mismatch refuses all signals and closes the binding") {
    Child child;
    REQUIRE(child.pid() > 0);
    const auto identity = readProcessIdentity(child.pid());
    REQUIRE(identity.has_value());
    ProcessTarget target;
    if (!requireBinding(target, *identity)) return;
    target.reset();
    const int descriptors = openDescriptors();
    auto wrong = *identity;
    ++wrong.startTicks;
    CHECK(target.bind(wrong).status == ProcessStatus::IdentityChanged);
    CHECK_FALSE(target.isBound());
    CHECK(openDescriptors() == descriptors);
    CHECK(target.sendSignal(SIGKILL).status == ProcessStatus::NotBound);
    CHECK(child.alive());
}

TEST_CASE("Safe sending reaches only the bound child and consumes its pidfd") {
    for (const int signal : {SIGTERM, SIGKILL}) {
        Child child;
        Child other;
        REQUIRE(child.pid() > 0);
        REQUIRE(other.pid() > 0);
        const auto identity = readProcessIdentity(child.pid());
        REQUIRE(identity.has_value());
        ProcessTarget target;
        if (!requireBinding(target, *identity)) return;
        CHECK(target.sendSignal(signal).status == ProcessStatus::Ready);
        CHECK_FALSE(target.isBound());
        const int status = child.wait();
        REQUIRE(WIFSIGNALED(status));
        CHECK(WTERMSIG(status) == signal);
        CHECK(other.alive());
        CHECK(target.sendSignal(signal).status == ProcessStatus::NotBound);
    }
}

TEST_CASE("An exited target is reported rather than treated as a successful send") {
    Child child;
    REQUIRE(child.pid() > 0);
    const auto identity = readProcessIdentity(child.pid());
    REQUIRE(identity.has_value());
    ProcessTarget target;
    if (!requireBinding(target, *identity)) return;
    child.release();
    REQUIRE(WIFEXITED(child.wait()));
    CHECK(target.check().status == ProcessStatus::Exited);
    CHECK(target.sendSignal(SIGKILL).status == ProcessStatus::Exited);
    CHECK_FALSE(target.isBound());
    CHECK(target.bind(*identity).status == ProcessStatus::Exited);
    CHECK_FALSE(target.isBound());
}

TEST_CASE("Cancelled, replaced and moved confirmations release their descriptors") {
    Child child;
    REQUIRE(child.pid() > 0);
    const auto identity = readProcessIdentity(child.pid());
    REQUIRE(identity.has_value());
    const int descriptors = openDescriptors();
    REQUIRE(descriptors >= 0);
    {
        ProcessTarget original;
        if (!requireBinding(original, *identity)) return;
        CHECK(openDescriptors() == descriptors + 1);
        CHECK(original.bind(*identity).status == ProcessStatus::Ready);
        CHECK(openDescriptors() == descriptors + 1);
        ProcessTarget moved(std::move(original));
        CHECK_FALSE(original.isBound());
        CHECK(moved.identity() == *identity);
        ProcessTarget assigned;
        REQUIRE(assigned.bind(*identity).status == ProcessStatus::Ready);
        assigned = std::move(moved);
        CHECK_FALSE(moved.isBound());
        CHECK(openDescriptors() == descriptors + 1);
        assigned.reset();
        CHECK(openDescriptors() == descriptors);
        REQUIRE(assigned.bind(*identity).status == ProcessStatus::Ready);
    }
    CHECK(openDescriptors() == descriptors);
    CHECK(child.alive());
}

TEST_CASE("Unbound and invalid targets are never signalled") {
    ProcessTarget target;
    CHECK(target.bind({0, 0}).status == ProcessStatus::Unavailable);
    CHECK(target.bind({-1, 0}).status == ProcessStatus::Unavailable);
    CHECK(target.check().status == ProcessStatus::NotBound);
    CHECK(target.sendSignal(SIGKILL).status == ProcessStatus::NotBound);
}

TEST_CASE("Failed sending also consumes the binding") {
    Child child;
    REQUIRE(child.pid() > 0);
    const auto identity = readProcessIdentity(child.pid());
    REQUIRE(identity.has_value());
    ProcessTarget target;
    if (!requireBinding(target, *identity)) return;
    CHECK(target.sendSignal(-1).status == ProcessStatus::Unavailable);
    CHECK_FALSE(target.isBound());
    CHECK(child.alive());
}

TEST_CASE("Unsupported pidfd open or send refuses without a raw-PID fallback") {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    for (const bool blockOpen : {true, false}) {
        const int status = unsupportedOperations(blockOpen);
        if (status == 77)
            MESSAGE("Kernel sandbox prevented isolated unsupported-syscall test.");
        else
            CHECK(status == 0);
    }
#else
    MESSAGE("Build headers lack pidfd syscalls; safe operations must refuse.");
#endif
}

TEST_CASE("Process CPU and IO baselines require the same PID and exact start tick") {
    const ProcessCounters previous{{123, 100}, 1000, 5000, 7000, true};
    ProcessCounters now{{123, 100}, 1200, 5200, 7400, true};
    const auto rates = processRates(now, &previous, 2, 100);
    CHECK(rates.cpuPercent == doctest::Approx(100));
    CHECK(rates.readRate == doctest::Approx(100));
    CHECK(rates.writeRate == doctest::Approx(200));
    // A reused PID may already have larger counters; clamping subtraction is not identity checking.
    ++now.identity.startTicks;
    auto reused = processRates(now, &previous, 2, 100);
    CHECK(reused.cpuPercent == 0);
    CHECK(reused.readRate == 0);
    CHECK(reused.writeRate == 0);
    now.identity = {124, 100};
    CHECK(processRates(now, &previous, 2, 100).cpuPercent == 0);
    // No previous record after exit (or first appearance) must start from a fresh zero-rate baseline.
    now.identity = previous.identity;
    const auto fresh = processRates(now, nullptr, 2, 100);
    CHECK(fresh.cpuPercent == 0);
    CHECK(fresh.readRate == 0);
    CHECK(fresh.writeRate == 0);
    now.ioKnown = false;
    CHECK(processRates(now, &previous, 2, 100).readRate == 0);
    CHECK(processRates(now, &previous, 0, 100).cpuPercent == 0);
    CHECK(processRates(now, &previous, 2, 0).cpuPercent == 0);
    now.ticks = now.read = now.written = 0;
    now.ioKnown = true;
    CHECK(processRates(now, &previous, 2, 100).cpuPercent == 0);
    CHECK(processRates(now, &previous, 2, 100).writeRate == 0);
}

TEST_CASE("Sampler keeps a child's raw identity and drops exited processes") {
    Child child;
    REQUIRE(child.pid() > 0);
    const auto identity = readProcessIdentity(child.pid());
    REQUIRE(identity.has_value());
    Sampler sampler;
    const auto sample = sampler.sample();
    bool found = false;
    for (const auto& p : sample->processes) {
        if (p.pid != child.pid()) continue;
        found = true;
        CHECK(p.startTicks == identity->startTicks);
        CHECK(p.cpuPercent == 0);
        CHECK(p.readRate == 0);
        CHECK(p.writeRate == 0);
    }
    REQUIRE(found);
    child.release();
    REQUIRE(WIFEXITED(child.wait()));
    const auto next = sampler.sample();
    for (const auto& p : next->processes) CHECK(p.pid != child.pid());
}

TEST_CASE("Memory pressure is PSI some avg10, not RAM occupancy or PSI full") {
    SystemSnapshot snapshot;
    parseMemoryStats(snapshot, kMeminfo, "full avg10=75.00 avg60=0 total=0\nsome avg10=0.00 avg60=0 total=0\n");
    REQUIRE(snapshot.memoryKnown);
    REQUIRE(snapshot.memoryPressureKnown);
    CHECK(snapshot.memUsed == 900 * 1024);
    CHECK(snapshot.memCached == 165 * 1024);
    CHECK(snapshot.memoryPressure == 0);  // 90% RAM used is not 90% pressure
    REQUIRE(snapshot.swapKnown);
    CHECK(snapshot.swapUsed == 50 * 1024);
    parseMemoryStats(snapshot, kMeminfo, "some avg10=25.50 avg60=99.00 total=100\nfull avg10=1.00\n");
    CHECK(snapshot.memoryPressure == doctest::Approx(0.255));
}

TEST_CASE("Unknown or malformed memory readings never become known normal pressure") {
    SystemSnapshot snapshot;
    for (const auto input : {"", "full avg10=0.00\n", "some avg60=0.00\n", "some avg10=invalid\n",
                             "some avg10=0.00junk\n", "some avg10=nan\n", "some avg10=-1\n", "some avg10=100.01\n"}) {
        parseMemoryStats(snapshot, kMeminfo, "some avg10=50.00\n");
        REQUIRE(snapshot.memoryPressureKnown);
        parseMemoryStats(snapshot, kMeminfo, input);
        CHECK_FALSE(snapshot.memoryPressureKnown);
        CHECK(snapshot.memoryKnown);
    }
    parseMemoryStats(snapshot, "", "some avg10=1.00\n");
    CHECK_FALSE(snapshot.memoryKnown);
    CHECK_FALSE(snapshot.swapKnown);
    CHECK(snapshot.memoryPressureKnown);  // independent availability
    for (const auto bad : {"MemTotal: 0 kB\n", "MemTotal: rubbish kB\n", "MemTotal: 1000 bytes\n",
                           "MemTotal: 18446744073709551615 kB\n"}) {
        parseMemoryStats(snapshot, bad, "");
        CHECK_FALSE(snapshot.memoryKnown);
    }
    std::string invalid(kMeminfo);
    invalid.replace(invalid.find("MemAvailable: 100 kB"), std::string("MemAvailable: 100 kB").size(),
                    "MemAvailable: 1001 kB");
    parseMemoryStats(snapshot, invalid, "");
    CHECK_FALSE(snapshot.memoryKnown);
    CHECK(snapshot.memUsed == 0);
    parseMemoryStats(snapshot, "SwapTotal: 0 kB\nSwapFree: 0 kB\n", "some avg10=100.00\n");
    CHECK(snapshot.swapKnown);
    CHECK(snapshot.swapUsed == 0);
    CHECK(snapshot.memoryPressureKnown);
    CHECK(snapshot.memoryPressure == 1);
}
