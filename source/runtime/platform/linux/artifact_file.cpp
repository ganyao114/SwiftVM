#include "artifact_file.h"
#include "base/error.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swift::runtime::platform {
namespace {
class File {
public:
    explicit File(int value) : fd(value) {}

    ~File() {
        if (fd >= 0) close(fd);
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    int Get() const noexcept { return fd; }

    void Close() {
        const auto value = fd;
        fd = -1;
        if (close(value)) ThrowSystemError("close artifact", errno);
    }

private:
    int fd;
};
}  // namespace

std::vector<std::uint8_t> ReadArtifactFile(const std::string& path,
                                           bool private_owner,
                                           std::size_t limit) {
    const int raw = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw < 0) ThrowSystemError("open artifact", errno);
    File file{raw};
    struct stat status{};
    if (fstat(raw, &status)) ThrowSystemError("stat artifact", errno);
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > limit)
        ThrowError("artifact is not a regular file within its size limit",
                   ErrorKind::InvalidArgument);
    if (private_owner && (status.st_uid != geteuid() || (status.st_mode & 0022)))
        ThrowError("code cache is not owned privately by this user", ErrorKind::InvalidArgument);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t done{};
    while (done < bytes.size()) {
        const auto amount =
                read(raw, bytes.data() + done, std::min(bytes.size() - done, std::size_t{1} << 20));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) ThrowSystemError("read artifact", errno);
        if (!amount) ThrowError("artifact was truncated while reading", ErrorKind::InvalidArgument);
        done += static_cast<std::size_t>(amount);
    }
    return bytes;
}

void WriteArtifactFile(const std::string& path, std::span<const std::uint8_t> bytes) {
    if (bytes.size() > ArtifactFileLimit)
        ThrowError("artifact exceeds its size limit", ErrorKind::ResourceLimit);
    std::string temporary = path + ".tmp-XXXXXX";
    const int raw = mkstemp(temporary.data());
    if (raw < 0) ThrowSystemError("create artifact temporary file", errno);

    struct Temporary {
        std::string& path;

        ~Temporary() {
            if (!path.empty()) unlink(path.c_str());
        }
    } cleanup{temporary};

    File file{raw};
    if (fcntl(raw, F_SETFD, FD_CLOEXEC) || fchmod(raw, 0600))
        ThrowSystemError("protect artifact temporary file", errno);
    std::size_t done{};
    while (done < bytes.size()) {
        const auto amount = write(
                raw, bytes.data() + done, std::min(bytes.size() - done, std::size_t{1} << 20));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) ThrowSystemError("write artifact", errno);
        if (!amount) ThrowError("artifact write made no progress", ErrorKind::System);
        done += static_cast<std::size_t>(amount);
    }
    if (fsync(raw)) ThrowSystemError("sync artifact", errno);
    file.Close();
    if (rename(temporary.c_str(), path.c_str())) ThrowSystemError("publish artifact", errno);
    temporary.clear();
    const auto slash = path.find_last_of('/');
    const auto directory = slash == std::string::npos ? "."
                           : slash == 0               ? "/"
                                                      : path.substr(0, slash);
    const int parent = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) ThrowSystemError("open artifact parent", errno);
    File parent_file{parent};
    if (fsync(parent)) ThrowSystemError("sync artifact parent", errno);
}

void EnsureCacheDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0700) && errno != EEXIST)
        ThrowSystemError("create code cache directory", errno);
    struct stat status{};
    if (lstat(path.c_str(), &status)) ThrowSystemError("stat code cache directory", errno);
    if (!S_ISDIR(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0077))
        ThrowError("code cache directory must be owned by this user with mode 0700",
                   ErrorKind::InvalidArgument);
}
}  // namespace swift::runtime::platform
