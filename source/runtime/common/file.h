//
// Created by 甘尧 on 2023/9/25.
//

#pragma once

#include <mutex>
#include <utility>
#include "common_funcs.h"
#include "types.h"

#define HAS_UNIX_FD defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)

namespace swift::runtime {

enum class FileAccessMode {
    None = 0,
    /**
     * If the file at path exists, it opens the file for reading.
     * If the file at path does not exist, it fails to open the file.
     */
    Read = 1 << 0,
    /**
     * If the file at path exists, the existing contents of the file are erased.
     * The empty file is then opened for writing.
     * If the file at path does not exist, it creates and opens a new empty file for writing.
     */
    Write = 1 << 1,
    /**
     * If the file at path exists, it opens the file for reading and writing.
     * If the file at path does not exist, it fails to open the file.
     */
    ReadWrite = Read | Write,
    /**
     * If the file at path exists, it opens the file for appending.
     * If the file at path does not exist, it creates and opens a new empty file for appending.
     */
    Append = 1 << 2,
    /**
     * If the file at path exists, it opens the file for both reading and appending.
     * If the file at path does not exist, it creates and opens a new empty file for both
     * reading and appending.
     */
    ReadAppend = Read | Append,
};

DECLARE_ENUM_FLAG_OPERATORS(FileAccessMode)

class File : DeleteCopyAndMove {
public:
    explicit File(std::string path) : path(std::move(path)) {}

    virtual ~File();

    static bool Create(const std::string& path);
    static bool Delete(const std::string& path);
    bool Open(FileAccessMode mode);
    bool IsOpen();
    bool Close();
    size_t Size();
    bool Resize(size_t new_size);
    bool Read(void* dest, size_t offset, size_t size);
    bool Write(void* src, size_t offset, size_t size);
    void* Map(size_t offset, size_t size);
    bool Flush();
    bool Commit();
    std::string GetPath();

    template <typename T> T Read(size_t offset = 0) {
        T t;
        Read(&t, offset, sizeof(T));
        return std::move(t);
    }

    template <typename T> void Write(T& t, size_t offset) { Write(&t, offset, sizeof(T)); }

private:
    std::string path;
    std::FILE* file{};
    std::mutex lock;
    FileAccessMode open_mode;
};

bool Unmap(void* mem, size_t size);

}  // namespace swift::runtime
