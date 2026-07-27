#pragma once

#include <cstdlib>
#include <string>
#include <sys/stat.h>

namespace swift::linux {

// Resolves an absolute guest pathname through SVM_SYSROOT when the redirected
// object exists. Relative paths, and every path when SVM_SYSROOT is unset,
// are returned byte-for-byte unchanged. Falling back to the original path is
// intentional: it keeps host passthrough available for objects not supplied
// by the guest sysroot (for example /dev/null).
inline std::string ResolveGuestPath(const std::string& guest_path) {
    if (guest_path.empty() || guest_path.front() != '/') {
        return guest_path;
    }
    const char* root_env = std::getenv("SVM_SYSROOT");
    if (!root_env || !*root_env) {
        return guest_path;
    }

    std::string root(root_env);
    if (char* absolute_root = ::realpath(root.c_str(), nullptr)) {
        root = absolute_root;
        std::free(absolute_root);
    }
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    std::string redirected = root == "/" ? guest_path : root + guest_path;
    struct stat st {};
    if (::lstat(redirected.c_str(), &st) == 0) {
        return redirected;
    }
    return guest_path;
}

}  // namespace swift::linux
