#include "system_stats.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>

namespace {

double monotonicSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/// Reads a small file in one go (procfs files report size 0, so no seeking/stat).
bool readFile(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "re");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

std::string readLine(const std::string& path) {
    std::string s;
    if (!readFile(path.c_str(), s)) return {};
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

bool exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

std::optional<std::uint64_t> meminfoValue(std::string_view text, std::string_view key) {
    while (!text.empty()) {
        const size_t end = text.find('\n');
        auto line = text.substr(0, end);
        text.remove_prefix(line.size() + (end != std::string_view::npos ? 1 : 0));
        if (!line.starts_with(key)) continue;
        line.remove_prefix(key.size());
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos) return std::nullopt;
        line.remove_prefix(first);
        std::uint64_t value = 0;
        const auto [tail, error] = std::from_chars(line.data(), line.data() + line.size(), value);
        if (error != std::errc{} || value > std::numeric_limits<std::uint64_t>::max() / 1024) return std::nullopt;
        auto unit = line.substr(static_cast<size_t>(tail - line.data()));
        const size_t unitStart = unit.find_first_not_of(" \t");
        if (unitStart == std::string_view::npos) return std::nullopt;
        unit.remove_prefix(unitStart);
        const size_t unitEnd = unit.find_last_not_of(" \t\r");
        if (unit.substr(0, unitEnd + 1) != "kB") return std::nullopt;
        return value * 1024;
    }
    return std::nullopt;
}

std::optional<double> pressureValue(std::string_view text) {
    while (!text.empty()) {
        const size_t end = text.find('\n');
        const auto line = text.substr(0, end);
        text.remove_prefix(line.size() + (end != std::string_view::npos ? 1 : 0));
        if (!line.starts_with("some ")) continue;
        const size_t at = line.find(" avg10=");
        if (at == std::string_view::npos) return std::nullopt;
        const auto field = line.substr(at + 7);
        const auto token = field.substr(0, field.find_first_of(" \t\r"));
        if (token.empty()) return std::nullopt;
        double value = 0;
        const auto [tail, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || tail != token.data() + token.size() || !std::isfinite(value) || value < 0 ||
            value > 100)
            return std::nullopt;
        return value / 100;
    }
    return std::nullopt;
}

}  // namespace

Sampler::Sampler() {
    m_ticksPerSecond = sysconf(_SC_CLK_TCK);
    m_pageSize = sysconf(_SC_PAGESIZE);
    std::string info;
    if (readFile("/proc/cpuinfo", info)) {
        size_t at = info.find("model name");
        if (at != std::string::npos) {
            size_t colon = info.find(':', at);
            size_t end = info.find('\n', colon);
            m_cpuModel = info.substr(colon + 2, end - colon - 2);
        }
    }
    // CPU temperature sensor (AMD k10temp/zenpower, Intel coretemp) and an AMD GPU load counter.
    for (int i = 0; i < 32 && m_tempPath.empty(); ++i) {
        std::string base = "/sys/class/hwmon/hwmon" + std::to_string(i);
        std::string name = readLine(base + "/name");
        if (name == "k10temp" || name == "coretemp" || name == "zenpower" || name == "cpu_thermal")
            m_tempPath = base + "/temp1_input";
    }
    for (int i = 0; i < 4 && m_gpuPath.empty(); ++i) {
        std::string p = "/sys/class/drm/card" + std::to_string(i) + "/device/gpu_busy_percent";
        if (exists(p)) m_gpuPath = p;
    }
}

const std::string& Sampler::userName(uid_t uid) {
    auto it = m_users.find(uid);
    if (it != m_users.end()) return it->second;
    passwd pw{};
    passwd* result = nullptr;
    char buf[1024];
    std::string name = getpwuid_r(uid, &pw, buf, sizeof buf, &result) == 0 && result ? pw.pw_name : std::to_string(uid);
    return m_users.emplace(uid, name).first->second;
}

std::shared_ptr<SystemSnapshot> Sampler::sample() {
    auto s = std::make_shared<SystemSnapshot>();
    s->timestamp = monotonicSeconds();
    const double dt = m_lastTime > 0 ? std::max(1e-3, s->timestamp - m_lastTime) : 0.0;
    m_lastTime = s->timestamp;
    s->currentUid = getuid();
    char host[256] = {};
    gethostname(host, sizeof host - 1);
    s->hostname = host;
    utsname un{};
    if (uname(&un) == 0) s->kernel = std::string(un.sysname) + " " + un.release;

    readCpu(*s, dt);
    readMemory(*s, dt);
    readDisks(*s, dt);
    readFileSystems(*s);
    readNetwork(*s, dt);
    readProcesses(*s, dt);
    return s;
}

// --- CPU ----------------------------------------------------------------------------------------

void Sampler::readCpu(SystemSnapshot& s, double dt) {
    s.cpuModel = m_cpuModel;
    std::string text;
    if (!readFile("/proc/stat", text)) return;
    std::istringstream in(text);
    std::string line;
    std::vector<CpuTicks> cores;
    CpuTicks total;
    while (std::getline(in, line)) {
        if (line.rfind("cpu", 0) != 0) {
            if (line.rfind("procs_running", 0) == 0) break;
            continue;
        }
        CpuTicks t;
        char label[16];
        std::sscanf(line.c_str(), "%15s %lu %lu %lu %lu %lu %lu %lu %lu", label, &t.user, &t.nice, &t.system, &t.idle,
                    &t.iowait, &t.irq, &t.softirq, &t.steal);
        if (std::strcmp(label, "cpu") == 0) total = t;
        else cores.push_back(t);
    }
    s.logicalCores = static_cast<int>(cores.size());
    auto usage = [](const CpuTicks& now, const CpuTicks& prev, double& user, double& system, double& idle, double& io) {
        double d = static_cast<double>(now.total() - prev.total());
        if (d <= 0) {
            user = system = io = 0;
            idle = 1;
            return;
        }
        user = static_cast<double>((now.user + now.nice) - (prev.user + prev.nice)) / d;
        system = static_cast<double>((now.system + now.irq + now.softirq + now.steal) -
                                     (prev.system + prev.irq + prev.softirq + prev.steal)) / d;
        io = static_cast<double>(now.iowait - prev.iowait) / d;
        idle = static_cast<double>(now.idle - prev.idle) / d;
    };
    if (dt > 0) {
        usage(total, m_prevTotal, s.cpuUser, s.cpuSystem, s.cpuIdle, s.cpuIowait);
        for (size_t i = 0; i < cores.size() && i < m_prevCores.size(); ++i) {
            double idle = 0;
            double io = 0;
            CoreUsage c;
            usage(cores[i], m_prevCores[i], c.user, c.system, idle, io);
            s.cores.push_back(c);
        }
    }
    if (s.cores.size() != cores.size()) s.cores.assign(cores.size(), CoreUsage{});
    m_prevTotal = total;
    m_prevCores = cores;

    double freqSum = 0;
    int freqCount = 0;
    for (int i = 0; i < s.logicalCores; ++i) {
        std::string f = readLine("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq");
        if (!f.empty()) {
            freqSum += std::atof(f.c_str()) / 1000.0;
            ++freqCount;
        }
    }
    if (freqCount) s.frequencyMHz = freqSum / freqCount;
    if (!m_tempPath.empty()) {
        std::string t = readLine(m_tempPath);
        if (!t.empty()) s.temperatureC = std::atof(t.c_str()) / 1000.0;
    }
    if (!m_gpuPath.empty()) {
        std::string g = readLine(m_gpuPath);
        if (!g.empty()) s.gpuBusy = std::atof(g.c_str()) / 100.0;
    }
    std::string load = readLine("/proc/loadavg");
    std::sscanf(load.c_str(), "%lf %lf %lf", &s.loadAverage[0], &s.loadAverage[1], &s.loadAverage[2]);
    std::string up = readLine("/proc/uptime");
    s.uptime = std::atof(up.c_str());
}

// --- memory -----------------------------------------------------------------------------------------

void parseMemoryStats(SystemSnapshot& s, std::string_view meminfo, std::string_view pressure) {
    const auto total = meminfoValue(meminfo, "MemTotal:");
    const auto available = meminfoValue(meminfo, "MemAvailable:");
    const auto buffers = meminfoValue(meminfo, "Buffers:");
    const auto shared = meminfoValue(meminfo, "Shmem:");
    const auto cached = meminfoValue(meminfo, "Cached:");
    const auto reclaimable = meminfoValue(meminfo, "SReclaimable:");
    s.memoryKnown = total && *total > 0 && available && *available <= *total && buffers && shared && cached &&
                    reclaimable && *cached <= std::numeric_limits<std::uint64_t>::max() - *reclaimable;
    s.memTotal = total.value_or(0);
    s.memAvailable = available.value_or(0);
    s.memBuffers = buffers.value_or(0);
    s.memShared = shared.value_or(0);
    s.memUsed = s.memoryKnown ? s.memTotal - s.memAvailable : 0;
    const std::uint64_t cache = s.memoryKnown ? *cached + *reclaimable : 0;
    s.memCached = cache - std::min(cache, s.memShared);
    const auto swapTotal = meminfoValue(meminfo, "SwapTotal:");
    const auto swapFree = meminfoValue(meminfo, "SwapFree:");
    s.swapKnown = swapTotal && swapFree && *swapFree <= *swapTotal;
    s.swapTotal = swapTotal.value_or(0);
    s.swapUsed = s.swapKnown ? s.swapTotal - *swapFree : 0;
    const auto stall = pressureValue(pressure);
    s.memoryPressureKnown = stall.has_value();
    s.memoryPressure = stall.value_or(0);
}

void Sampler::readMemory(SystemSnapshot& s, double dt) {
    std::string text;
    readFile("/proc/meminfo", text);
    parseMemoryStats(s, text, readLine("/proc/pressure/memory"));

    std::string vm;
    if (readFile("/proc/vmstat", vm)) {
        auto value = [&](const char* key) {
            size_t at = vm.find(key);
            return at == std::string::npos ? 0ull : std::strtoull(vm.c_str() + at + std::strlen(key), nullptr, 10);
        };
        std::uint64_t in = value("\npswpin ");
        std::uint64_t out = value("\npswpout ");
        if (dt > 0) {
            s.swapInRate = static_cast<double>(in - m_prevSwapIn) / dt;
            s.swapOutRate = static_cast<double>(out - m_prevSwapOut) / dt;
        }
        m_prevSwapIn = in;
        m_prevSwapOut = out;
    }
}

// --- disks ------------------------------------------------------------------------------------------

void Sampler::readDisks(SystemSnapshot& s, double dt) {
    std::string text;
    if (!readFile("/proc/diskstats", text)) return;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        unsigned major = 0;
        unsigned minor = 0;
        char name[64];
        unsigned long long reads = 0, readsMerged = 0, readSectors = 0, readMs = 0, writes = 0, writesMerged = 0,
                           writeSectors = 0;
        if (std::sscanf(line.c_str(), "%u %u %63s %llu %llu %llu %llu %llu %llu %llu", &major, &minor, name, &reads,
                        &readsMerged, &readSectors, &readMs, &writes, &writesMerged, &writeSectors) < 10)
            continue;
        // Whole physical disks only: partitions and virtual devices (zram, loop, dm) would double count.
        std::string dev = name;
        if (!exists("/sys/block/" + dev + "/device")) continue;
        DiskDevice d;
        d.name = dev;
        d.model = readLine("/sys/block/" + dev + "/device/model");
        d.bytesRead = readSectors * 512;
        d.bytesWritten = writeSectors * 512;
        auto& prev = m_prevDisks[dev];
        if (dt > 0 && prev.reads + prev.writes > 0) {
            d.readRate = static_cast<double>((readSectors - prev.readSectors) * 512) / dt;
            d.writeRate = static_cast<double>((writeSectors - prev.writeSectors) * 512) / dt;
            d.readOps = static_cast<double>(reads - prev.reads) / dt;
            d.writeOps = static_cast<double>(writes - prev.writes) / dt;
        }
        prev = {readSectors, writeSectors, reads, writes};
        s.diskReadRate += d.readRate;
        s.diskWriteRate += d.writeRate;
        s.diskReadOps += d.readOps;
        s.diskWriteOps += d.writeOps;
        s.disks.push_back(std::move(d));
    }
}

void Sampler::readFileSystems(SystemSnapshot& s) {
    if (m_fsCountdown-- > 0) {
        s.fileSystems = m_fsCache;
        return;
    }
    m_fsCountdown = 5;
    static const std::set<std::string> kReal = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "vfat", "exfat",
                                                "ntfs", "ntfs3", "fuseblk", "zfs", "bcachefs", "jfs", "reiserfs"};
    std::string text;
    m_fsCache.clear();
    if (readFile("/proc/self/mounts", text)) {
        std::istringstream in(text);
        std::string device, mount, type;
        std::set<std::string> seen;
        std::string rest;
        while (in >> device >> mount >> type && std::getline(in, rest)) {
            if (!kReal.count(type) || !seen.insert(device).second) continue;  // btrfs subvolumes share a device
            struct statvfs v{};
            if (statvfs(mount.c_str(), &v) != 0 || v.f_blocks == 0) continue;
            FileSystem fs;
            fs.mountPoint = mount;
            fs.device = device;
            fs.type = type;
            fs.total = static_cast<std::uint64_t>(v.f_blocks) * v.f_frsize;
            fs.available = static_cast<std::uint64_t>(v.f_bavail) * v.f_frsize;
            m_fsCache.push_back(fs);
        }
    }
    std::sort(m_fsCache.begin(), m_fsCache.end(), [](const FileSystem& a, const FileSystem& b) {
        if (a.mountPoint == "/") return true;
        if (b.mountPoint == "/") return false;
        return a.mountPoint < b.mountPoint;
    });
    s.fileSystems = m_fsCache;
}

// --- network ----------------------------------------------------------------------------------------

void Sampler::readNetwork(SystemSnapshot& s, double dt) {
    std::string text;
    if (!readFile("/proc/net/dev", text)) return;
    std::map<std::string, std::string> addresses;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || addresses.count(a->ifa_name)) continue;
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, buf, sizeof buf);
            addresses[a->ifa_name] = buf;
        }
        freeifaddrs(list);
    }
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);
    std::getline(in, line);
    while (std::getline(in, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        name.erase(0, name.find_first_not_of(' '));
        if (name == "lo") continue;
        unsigned long long rx = 0, rxp = 0, tx = 0, txp = 0, skip = 0;
        std::sscanf(line.c_str() + colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &rx, &rxp, &skip, &skip,
                    &skip, &skip, &skip, &skip, &tx, &txp);
        NetworkInterface n;
        n.name = name;
        n.rxBytes = rx;
        n.txBytes = tx;
        n.up = readLine("/sys/class/net/" + name + "/operstate") == "up";
        n.wireless = exists("/sys/class/net/" + name + "/wireless");
        if (auto it = addresses.find(name); it != addresses.end()) n.address = it->second;
        auto& prev = m_prevNet[name];
        if (dt > 0 && prev.rx + prev.tx > 0) {
            n.rxRate = static_cast<double>(rx - prev.rx) / dt;
            n.txRate = static_cast<double>(tx - prev.tx) / dt;
            n.rxPackets = static_cast<double>(rxp - prev.rxPackets) / dt;
            n.txPackets = static_cast<double>(txp - prev.txPackets) / dt;
        }
        prev = {rx, tx, rxp, txp};
        if (!n.up && rx + tx == 0) continue;  // unused virtual interfaces
        s.netRxRate += n.rxRate;
        s.netTxRate += n.txRate;
        s.netRxPackets += n.rxPackets;
        s.netTxPackets += n.txPackets;
        s.interfaces.push_back(std::move(n));
    }
    std::sort(s.interfaces.begin(), s.interfaces.end(), [](const auto& a, const auto& b) {
        if (a.up != b.up) return a.up;
        return a.rxBytes + a.txBytes > b.rxBytes + b.txBytes;
    });
}

// --- processes --------------------------------------------------------------------------------------

void Sampler::readProcesses(SystemSnapshot& s, double dt) {
    DIR* proc = opendir("/proc");
    if (!proc) return;
    std::unordered_map<pid_t, ProcPrev> next;
    std::string text;
    char path[64];
    const double tps = static_cast<double>(m_ticksPerSecond);
    while (dirent* e = readdir(proc)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const pid_t pid = static_cast<pid_t>(std::atoi(e->d_name));
        std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
        if (!readFile(path, text)) continue;  // exited meanwhile
        const auto identity = parseProcessIdentity(text);
        if (!identity || identity->pid != pid) continue;
        const size_t open = text.find('(');
        const size_t close = text.rfind(')');

        ProcessInfo p;
        p.pid = pid;
        p.name = text.substr(open + 1, close - open - 1);
        // Fields after the name, starting with field 3 (state).
        char state = '?';
        int ppid = 0;
        unsigned long flags = 0, utime = 0, stime = 0, vsize = 0;
        long threads = 0, rss = 0;
        unsigned long long start = 0;
        if (std::sscanf(text.c_str() + close + 2,
                        "%c %d %*d %*d %*d %*d %lu %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %ld %*d %llu %lu %ld",
                        &state, &ppid, &flags, &utime, &stime, &threads, &start, &vsize, &rss) != 9 ||
            start != identity->startTicks)
            continue;
        p.state = state;
        p.ppid = ppid;
        p.threads = static_cast<int>(threads);
        p.cpuSeconds = static_cast<double>(utime + stime) / tps;
        p.residentBytes = static_cast<std::uint64_t>(std::max(0L, rss)) * static_cast<std::uint64_t>(m_pageSize);
        p.virtualBytes = vsize;
        p.startTicks = identity->startTicks;
        p.startedAt = static_cast<double>(p.startTicks) / tps;
        p.isKernelThread = (flags & 0x00200000) != 0;  // PF_KTHREAD

        // The owner of /proc/<pid> is the process' effective uid; cheaper than parsing status.
        struct stat st{};
        std::snprintf(path, sizeof path, "/proc/%d", pid);
        if (::stat(path, &st) == 0) p.uid = st.st_uid;
        p.user = userName(p.uid);

        if (!p.isKernelThread) {
            std::snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
            if (readFile(path, text) && !text.empty()) {
                std::string argv0 = text.substr(0, text.find('\0'));
                std::replace(text.begin(), text.end(), '\0', ' ');
                while (!text.empty() && text.back() == ' ') text.pop_back();
                p.command = text;
                // comm is truncated to 15 characters; prefer the executable's real name.
                // Some programs (Chromium, Electron) rewrite their title into argv[0] with spaces.
                argv0 = argv0.substr(0, argv0.find(' '));
                std::string base = argv0.substr(argv0.find_last_of('/') + 1);
                if (base.size() > p.name.size() && base.rfind(p.name, 0) == 0) p.name = base;
            }
        }

        ProcPrev prev;
        prev.identity = *identity;
        prev.ticks = utime + stime;
        std::snprintf(path, sizeof path, "/proc/%d/io", pid);
        if (readFile(path, text)) {
            auto field = [&](const char* key) {
                size_t at = text.find(key);
                return at == std::string::npos ? 0ull
                                               : std::strtoull(text.c_str() + at + std::strlen(key), nullptr, 10);
            };
            p.ioKnown = prev.ioKnown = true;
            p.bytesRead = prev.read = field("read_bytes: ");
            p.bytesWritten = prev.written = field("write_bytes: ");
        }
        const auto it = m_prevProcs.find(pid);
        const auto rates = processRates(prev, it != m_prevProcs.end() ? &it->second : nullptr, dt, tps);
        p.cpuPercent = rates.cpuPercent;
        p.readRate = rates.readRate;
        p.writeRate = rates.writeRate;

        // Open sockets (only countable for processes we may inspect).
        if (!p.isKernelThread && p.uid == s.currentUid) {
            std::snprintf(path, sizeof path, "/proc/%d/fd", pid);
            if (DIR* fds = opendir(path)) {
                int sockets = 0;
                char link[320];
                char target[64];
                while (dirent* f = readdir(fds)) {
                    if (f->d_name[0] == '.') continue;
                    std::snprintf(link, sizeof link, "/proc/%d/fd/%s", pid, f->d_name);
                    ssize_t n = readlink(link, target, sizeof target - 1);
                    if (n > 7 && std::strncmp(target, "socket:", 7) == 0) ++sockets;
                }
                closedir(fds);
                p.sockets = sockets;
            }
        }
        // Do not retain mixed counters if this PID exited and was reused while its files were read.
        if (readProcessIdentity(pid) != identity) continue;
        next[pid] = prev;
        s.threadCount += p.threads;
        s.processes.push_back(std::move(p));
    }
    closedir(proc);
    s.processCount = static_cast<int>(s.processes.size());
    m_prevProcs = std::move(next);
}

// --- formatting ------------------------------------------------------------------------------------

std::string formatBytes(double bytes, int decimals) {
    static const char* units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    int u = 0;
    while (bytes >= 1000 && u < 5) {
        bytes /= 1000;
        ++u;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%.0f %s", bytes, units[u]);
    else std::snprintf(buf, sizeof buf, "%.*f %s", decimals, bytes, units[u]);
    return buf;
}

std::string formatRate(double bps) { return formatBytes(bps) + "/s"; }

std::string formatDuration(double seconds) {
    auto total = static_cast<long>(seconds);
    int cents = static_cast<int>((seconds - static_cast<double>(total)) * 100);
    char buf[32];
    if (total >= 3600)
        std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld.%02d", total / 3600, (total / 60) % 60, total % 60, cents);
    else
        std::snprintf(buf, sizeof buf, "%ld:%02ld.%02d", total / 60, total % 60, cents);
    return buf;
}

std::string formatUptime(double seconds) {
    auto total = static_cast<long>(seconds);
    long days = total / 86400;
    char buf[48];
    if (days > 0)
        std::snprintf(buf, sizeof buf, "%ld day%s, %ld:%02ld", days, days == 1 ? "" : "s", (total / 3600) % 24,
                      (total / 60) % 60);
    else
        std::snprintf(buf, sizeof buf, "%ld:%02ld", total / 3600, (total / 60) % 60);
    return buf;
}
