// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/io.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace zxconfig {
namespace {

/// @brief Drop the temporary and report @p message against @p span.
Result<void> discard(int fd, const std::filesystem::path& temporary,
                     std::string_view message, SourceSpan span) {
    if (fd >= 0) ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return std::unexpected(Diagnostic::error(ErrorCode::WriteFailed,
                                             std::string{message}, span));
}

}  // namespace

Result<void> write_atomically(const std::filesystem::path& path,
                              std::string_view data, SourceSpan span) {
    const auto temporary = path.string() + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::WriteFailed,
            "cannot create '" + temporary + "': " + std::strerror(errno), span));
    }

    std::size_t written = 0;
    while (written < data.size()) {
        const auto n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return discard(fd, temporary,
                           "error writing '" + temporary + "': "
                               + std::strerror(errno), span);
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        return discard(fd, temporary,
                       "cannot flush '" + temporary + "': " + std::strerror(errno),
                       span);
    }
    if (::close(fd) != 0) {
        return discard(-1, temporary,
                       "cannot close '" + temporary + "': " + std::strerror(errno),
                       span);
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return std::unexpected(Diagnostic::error(
            ErrorCode::WriteFailed,
            "cannot replace '" + path.string() + "': " + ec.message(), span));
    }
    return {};
}

}  // namespace zxconfig
