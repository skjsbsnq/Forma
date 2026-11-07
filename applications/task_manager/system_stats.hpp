#pragma once

// Linux system and process statistics from /proc and /sys. A Sampler keeps the previous reading so
// it can turn cumulative counters (CPU ticks, bytes) into rates.

#include "process_control.hpp"

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ProcessInfo {
    pid_t pid = 0;
    pid_t ppid = 0;
    uid_t uid = 0;
    std::string name;
    std::string user;
    std::string command;     // full command line
    char state = '?';
    int threads = 0;
    double cpuPercent = 0;   // of one core, like Activity Monitor (can exceed 100)
    double cpuSeconds = 0;   // total CPU time used
    std::uint64_t residentBytes = 0;
    std::uint64_t virtualBytes = 0;
    bool ioKnown = false;    // /proc/<pid>/io is only readable for our own processes
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesWritten = 0;
    double readRate = 0;     // bytes per second
    double writeRate = 0;
    int sockets = -1;        // -1: not readable
    std::uint64_t startTicks = 0;  // raw start time, used for process identity
    double startedAt = 0;    // seconds since boot (display only)
    bool isKernelThread = false;
};

struct CoreUsage {
    double user = 0;    // fraction 0..1
    double system = 0;
};

struct DiskDevice {
    std::string name;   // nvme0n1, sda
    std::string model;
    double readRate = 0;
    double writeRate = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesWritten = 0;
    double readOps = 0;  // per second
    double writeOps = 0;
};

struct FileSystem {
    std::string mountPoint;
    std::string device;
    std::string type;
    std::uint64_t total = 0;
    std::uint64_t available = 0;
};

struct NetworkInterface {
    std::string name;
    std::string address;  // first IPv4, if any
    bool up = false;
    bool wireless = false;
    double rxRate = 0;    // bytes per second
    double txRate = 0;
    double rxPackets = 0; // per second
    double txPackets = 0;
    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;
};

struct SystemSnapshot {
    double timestamp = 0;  // monotonic seconds

    // CPU
    std::string cpuModel;
    int logicalCores = 0;
    double cpuUser = 0;     // fraction 0..1 of all cores
    double cpuSystem = 0;
    double cpuIdle = 1;
    double cpuIowait = 0;
    std::vector<CoreUsage> cores;
    double frequencyMHz = 0;    // average across cores
    double temperatureC = -1;   // -1: unknown
    double gpuBusy = -1;        // fraction, -1: unknown
    double loadAverage[3] = {0, 0, 0};
    int processCount = 0;
    int threadCount = 0;
    double uptime = 0;

    // Memory (bytes)
    std::uint64_t memTotal = 0;
    std::uint64_t memAvailable = 0;
    std::uint64_t memUsed = 0;      // total - available
    std::uint64_t memCached = 0;    // page cache + reclaimable slab, minus shmem
    std::uint64_t memBuffers = 0;
    std::uint64_t memShared = 0;
    std::uint64_t swapTotal = 0;
    std::uint64_t swapUsed = 0;
    bool memoryKnown = false;  // meminfo values were read and validated
    bool swapKnown = false;
    bool memoryPressureKnown = false;
    double memoryPressure = 0;      // PSI "some" avg10 / 100: fraction of time stalled, not RAM usage
    double swapInRate = 0;          // pages per second
    double swapOutRate = 0;

    // Disks / network
    std::vector<DiskDevice> disks;
    std::vector<FileSystem> fileSystems;
    std::vector<NetworkInterface> interfaces;
    double diskReadRate = 0;
    double diskWriteRate = 0;
    double diskReadOps = 0;
    double diskWriteOps = 0;
    double netRxRate = 0;
    double netTxRate = 0;
    double netRxPackets = 0;
    double netTxPackets = 0;

    std::vector<ProcessInfo> processes;

    // Host
    std::string hostname;
    std::string kernel;
    uid_t currentUid = 0;
};

class Sampler {
public:
    Sampler();
    /// Reads everything. Rates are relative to the previous call (zero on the first).
    std::shared_ptr<SystemSnapshot> sample();

private:
    struct CpuTicks {
        std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        [[nodiscard]] std::uint64_t total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
    };
    using ProcPrev = ProcessCounters;
    struct DiskPrev {
        std::uint64_t readSectors = 0, writeSectors = 0, reads = 0, writes = 0;
    };
    struct NetPrev {
        std::uint64_t rx = 0, tx = 0, rxPackets = 0, txPackets = 0;
    };

    void readCpu(SystemSnapshot& s, double dt);
    void readMemory(SystemSnapshot& s, double dt);
    void readDisks(SystemSnapshot& s, double dt);
    void readFileSystems(SystemSnapshot& s);
    void readNetwork(SystemSnapshot& s, double dt);
    void readProcesses(SystemSnapshot& s, double dt);
    const std::string& userName(uid_t uid);

    double m_lastTime = 0;
    long m_ticksPerSecond = 100;
    long m_pageSize = 4096;
    CpuTicks m_prevTotal;
    std::vector<CpuTicks> m_prevCores;
    std::uint64_t m_prevSwapIn = 0, m_prevSwapOut = 0;
    std::unordered_map<pid_t, ProcPrev> m_prevProcs;
    std::map<std::string, DiskPrev> m_prevDisks;
    std::map<std::string, NetPrev> m_prevNet;
    std::unordered_map<uid_t, std::string> m_users;
    std::string m_cpuModel;
    std::string m_tempPath;
    std::string m_gpuPath;
    int m_fsCountdown = 0;  // statvfs every few samples is plenty
    std::vector<FileSystem> m_fsCache;
};

// UI-free parsers shared by the sampler and regression tests. Missing/malformed input stays unknown.
void parseMemoryStats(SystemSnapshot& s, std::string_view meminfo, std::string_view pressure);

std::string formatBytes(double bytes, int decimals = 1);
std::string formatRate(double bytesPerSecond);
std::string formatDuration(double seconds);  // 1:02:03.45 style CPU time
std::string formatUptime(double seconds);    // "3 days, 4:05"
