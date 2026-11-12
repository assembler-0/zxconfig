// SPDX-License-Identifier: Apache-2.0

#ifndef ZXCONFIG_IO_HPP
#define ZXCONFIG_IO_HPP

#include <zxconfig/error.hpp>

#include <filesystem>
#include <string_view>

namespace zxconfig {

/// @brief Write @p data to @p path atomically and durably.
/// @param path  Final destination.  Parent directories must exist.
/// @param data  Bytes to write.
/// @param span  Source location for diagnostics, when the write backs a
///              construct in the manifest.
/// @return Error with @ref ErrorCode::WriteFailed when any step fails; the
///         temporary is removed and the target keeps its previous contents.
[[nodiscard]] Result<void> write_atomically(const std::filesystem::path& path,
                                            std::string_view data,
                                            SourceSpan span = {});

}  // namespace zxconfig

#endif  // ZXCONFIG_IO_HPP
