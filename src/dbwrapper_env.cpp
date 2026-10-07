// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/digibyte-config.h>
#endif

#include <dbwrapper_env.h>
#include <util/fs.h>
#include <util/syserror.h>

#include <leveldb/env.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

leveldb::Status FileError(const std::string& filename, int error)
{
#ifdef WIN32
    const auto description = Win32ErrorString(error);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return leveldb::Status::NotFound(filename, description);
    }
#else
    const auto description = SysErrorString(error);
    if (error == ENOENT || error == ENOTDIR) {
        return leveldb::Status::NotFound(filename, description);
    }
#endif
    return leveldb::Status::IOError(filename, description);
}

class NoMmapRandomAccessFile final : public leveldb::RandomAccessFile {
public:
    explicit NoMmapRandomAccessFile(std::string filename) : m_filename(std::move(filename)) {}

    ~NoMmapRandomAccessFile() override
    {
#ifdef WIN32
        if (m_handle != INVALID_HANDLE_VALUE) ::CloseHandle(m_handle);
#else
        if (m_fd != -1) ::close(m_fd);
#endif
    }

    leveldb::Status Open()
    {
#ifdef WIN32
        const auto path = fs::PathFromString(m_filename);
        m_handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) return FileError(m_filename, ::GetLastError());
#else
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        do {
            m_fd = ::open(m_filename.c_str(), flags);
        } while (m_fd == -1 && errno == EINTR);
        if (m_fd == -1) return FileError(m_filename, errno);
#ifndef O_CLOEXEC
        if (::fcntl(m_fd, F_SETFD, FD_CLOEXEC) == -1) return FileError(m_filename, errno);
#endif
#endif
        return leveldb::Status::OK();
    }

    leveldb::Status Read(uint64_t offset, size_t n, leveldb::Slice* result, char* scratch) const override
    {
        *result = leveldb::Slice(scratch, 0);
#ifdef WIN32
        constexpr uint64_t max_offset = std::numeric_limits<LONGLONG>::max();
#else
        constexpr uint64_t max_offset = std::numeric_limits<off_t>::max();
#endif
        if (offset > max_offset || n > max_offset - offset) {
            return leveldb::Status::InvalidArgument(m_filename, "Read offset is out of range");
        }
        if (n == 0) return leveldb::Status::OK();

#ifdef WIN32
        // Keep the seek and read together for a synchronous Windows handle.
        const std::lock_guard<std::mutex> lock(m_mutex);
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!::SetFilePointerEx(m_handle, position, nullptr, FILE_BEGIN)) {
            return FileError(m_filename, ::GetLastError());
        }
#endif

        size_t done{0};
        while (done < n) {
#ifdef WIN32
            const auto chunk = static_cast<DWORD>(std::min<size_t>(n - done, std::numeric_limits<DWORD>::max()));
            DWORD received{0};
            if (!::ReadFile(m_handle, scratch + done, chunk, &received, nullptr)) {
                const auto error = ::GetLastError();
                *result = leveldb::Slice(scratch, done);
                if (error == ERROR_HANDLE_EOF) break;
                return FileError(m_filename, error);
            }
#else
            const size_t chunk = std::min<size_t>(n - done, std::numeric_limits<ssize_t>::max());
            const auto received = ::pread(m_fd, scratch + done, chunk, static_cast<off_t>(offset + done));
            if (received < 0) {
                if (errno == EINTR) continue;
                const int error = errno;
                *result = leveldb::Slice(scratch, done);
                return FileError(m_filename, error);
            }
#endif
            if (received == 0) break;
            done += static_cast<size_t>(received);
        }
        // EOF is a short read. LevelDB checks block length and checksums itself.
        *result = leveldb::Slice(scratch, done);
        return leveldb::Status::OK();
    }

    std::string GetName() const override { return m_filename; }

private:
    const std::string m_filename;
#ifdef WIN32
    HANDLE m_handle{INVALID_HANDLE_VALUE};
    mutable std::mutex m_mutex;
#else
    int m_fd{-1};
#endif
};

class NoMmapEnv final : public leveldb::EnvWrapper {
public:
    NoMmapEnv() : EnvWrapper(leveldb::Env::Default()) {}

    leveldb::Status NewRandomAccessFile(const std::string& filename, leveldb::RandomAccessFile** result) override
    {
        *result = nullptr;
        auto file = std::make_unique<NoMmapRandomAccessFile>(filename);
        const auto status = file->Open();
        if (status.ok()) *result = file.release();
        return status;
    }
};

} // namespace

namespace dbwrapper_private {

std::unique_ptr<leveldb::Env> MakeNoMmapEnv()
{
    return std::make_unique<NoMmapEnv>();
}

} // namespace dbwrapper_private
