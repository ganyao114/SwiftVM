#include <linux/perf_event.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <fcntl.h>

namespace {
struct Error {
    std::string message;
};

[[noreturn]] void Fail(const std::string& message) { throw Error{message}; }

void Need(bool condition, const char* operation) {
    if (!condition) Fail(std::string(operation) + ": " + std::strerror(errno));
}

struct Descriptor {
    int value{-1};

    ~Descriptor() {
        if (value >= 0) close(value);
    }

    int Release() { return std::exchange(value, -1); }
};

class Child {
public:
    Child(unsigned cpu, const std::string& prefix, char** command) {
        std::array<int, 2> input{}, output{};
        Need(pipe(input.data()) == 0, "create guest input pipe");
        Descriptor input_read{input[0]}, input_write{input[1]};
        Need(pipe(output.data()) == 0, "create guest output pipe");
        Descriptor output_read{output[0]}, output_write{output[1]};
        Descriptor errors{open((prefix + ".stderr").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600)};
        Need(errors.value >= 0, "create guest stderr");
        pid = fork();
        Need(pid >= 0, "fork benchmark");
        if (!pid) {
            cpu_set_t affinity;
            CPU_ZERO(&affinity);
            CPU_SET(cpu, &affinity);
            if (sched_setaffinity(0, sizeof(affinity), &affinity) ||
                dup2(input_read.value, STDIN_FILENO) < 0 ||
                dup2(output_write.value, STDOUT_FILENO) < 0 ||
                dup2(errors.value, STDERR_FILENO) < 0)
                _exit(126);
            for (int fd : {input_read.value,
                           input_write.value,
                           output_read.value,
                           output_write.value,
                           errors.value})
                close(fd);
            execvp(command[0], command);
            _exit(127);
        }
        write_fd.value = input_write.Release();
        read_fd.value = output_read.Release();
    }

    ~Child() {
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }

    std::string Line() {
        std::string line;
        for (;;) {
            pollfd pending{read_fd.value, POLLIN, 0};
            Need(poll(&pending, 1, 30000) > 0, "wait for guest protocol");
            char ch{};
            Need(read(read_fd.value, &ch, 1) == 1, "read guest protocol");
            if (ch == '\n') return line;
            line += ch;
            if (line.size() > 1024) Fail("guest protocol line exceeds 1024 bytes");
        }
    }

    void Send(char token) { Need(write(write_fd.value, &token, 1) == 1, "release guest phase"); }

    int Wait() {
        int status{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            const auto result = waitpid(pid, &status, WNOHANG);
            Need(result >= 0, "wait for benchmark exit");
            if (result == pid) {
                pid = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            usleep(10000);
        }
        Fail("benchmark did not exit within 10 seconds");
    }

    pid_t PID() const { return pid; }

private:
    pid_t pid{-1};
    Descriptor read_fd, write_fd;
};

struct Result {
    std::uint64_t nanoseconds{}, checksum{};
};

pid_t Ready(Child& child) {
    const auto line = child.Line();
    std::istringstream fields(line);
    std::string tag;
    pid_t tid{};
    if (!(fields >> tag >> tid) || tag != "READY" || tid <= 0)
        Fail("expected READY tid, received: " + line);
    return tid;
}

Result Done(Child& child) {
    const auto line = child.Line();
    std::istringstream fields(line);
    std::string tag;
    Result result;
    if (!(fields >> tag >> result.nanoseconds >> result.checksum) || tag != "DONE")
        Fail("expected DONE nanoseconds checksum, received: " + line);
    return result;
}

class Counters {
public:
    explicit Counters(pid_t tid, unsigned cpu) {
        constexpr std::array<std::uint64_t, 3> events{
                PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_BRANCH_MISSES};
        for (unsigned i = 0; i < events.size(); ++i) {
            perf_event_attr attribute{};
            attribute.size = sizeof(attribute);
            attribute.type = PERF_TYPE_HARDWARE;
            attribute.config = events[i];
            attribute.disabled = i == 0;
            attribute.pinned = i == 0;
            attribute.exclude_kernel = attribute.exclude_hv = 1;
            attribute.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                                    PERF_FORMAT_TOTAL_TIME_RUNNING;
            descriptors[i].value = static_cast<int>(syscall(SYS_perf_event_open,
                                                            &attribute,
                                                            tid,
                                                            cpu,
                                                            i ? descriptors[0].value : -1,
                                                            PERF_FLAG_FD_CLOEXEC));
            Need(descriptors[i].value >= 0, "open guest-thread PMU group");
        }
    }

    void Start() {
        Need(ioctl(descriptors[0].value, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) == 0,
             "reset PMU group");
        Need(ioctl(descriptors[0].value, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) == 0,
             "enable PMU group");
    }

    void Stop() {
        Need(ioctl(descriptors[0].value, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) == 0,
             "disable PMU group");
        Need(read(descriptors[0].value, &counts, sizeof(counts)) == sizeof(counts),
             "read PMU group");
        if (counts.number != 3 || !counts.running || counts.running != counts.enabled)
            Fail("PMU group was unavailable or multiplexed");
    }

    struct {
        std::uint64_t number{}, enabled{}, running{};
        std::array<std::uint64_t, 3> values{};
    } counts;

private:
    std::array<Descriptor, 3> descriptors;
};

void Run(unsigned cpu, const std::string& prefix, char** command) {
    Child child{cpu, prefix, command};
    const auto pid = child.PID();
    const auto warm_tid = Ready(child);
    child.Send('G');
    const auto warm = Done(child);
    child.Send('X');
    const auto tid = Ready(child);
    if (tid != warm_tid) Fail("guest changed host thread between benchmark rounds");
    const auto task = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid);
    Need(access(task.c_str(), F_OK) == 0, "confirm host TID belongs to benchmark process");
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    Need(sched_getaffinity(tid, sizeof(affinity), &affinity) == 0, "read guest-thread affinity");
    if (CPU_COUNT(&affinity) != 1 || !CPU_ISSET(cpu, &affinity))
        Fail("guest thread is not pinned to the requested CPU");
    Counters counters{tid, cpu};
    counters.Start();
    child.Send('G');
    const auto measured = Done(child);
    counters.Stop();
    child.Send('X');
    const auto status = child.Wait();
    if (status || warm.checksum != measured.checksum)
        Fail("guest failed or checksum differs between benchmark rounds");
    const auto& counts = counters.counts;
    Descriptor result{open((prefix + ".json").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600)};
    Need(result.value >= 0, "create benchmark result");
    FILE* output = fdopen(result.Release(), "w");
    Need(output != nullptr, "open benchmark result stream");
    std::fprintf(output,
                 "{\"child_pid\":%d,\"guest_tid\":%d,\"cpu\":%u,"
                 "\"warmup_guest_ns\":%llu,\"guest_ns\":%llu,\"checksum\":%llu,"
                 "\"cycles\":%llu,\"instructions\":%llu,\"branch_misses\":%llu,"
                 "\"time_enabled_ns\":%llu,\"time_running_ns\":%llu,\"exit_code\":%d}\n",
                 pid,
                 tid,
                 cpu,
                 static_cast<unsigned long long>(warm.nanoseconds),
                 static_cast<unsigned long long>(measured.nanoseconds),
                 static_cast<unsigned long long>(measured.checksum),
                 static_cast<unsigned long long>(counts.values[0]),
                 static_cast<unsigned long long>(counts.values[1]),
                 static_cast<unsigned long long>(counts.values[2]),
                 static_cast<unsigned long long>(counts.enabled),
                 static_cast<unsigned long long>(counts.running),
                 status);
    Need(std::fclose(output) == 0, "write benchmark result");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fputs("usage: pmu-region CPU OUTPUT_PREFIX COMMAND [ARG...]\n", stderr);
        return 64;
    }
    signal(SIGPIPE, SIG_IGN);
    try {
        Run(static_cast<unsigned>(std::stoul(argv[1])), argv[2], argv + 3);
        return 0;
    } catch (const Error& error) {
        std::fprintf(stderr, "pmu-region: %s\n", error.message.c_str());
        return 1;
    }
}
