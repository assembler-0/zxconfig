// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/error.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxconfig {

/// @brief One line of a loaded source, as a byte range into that source.
struct Line {
    std::uint32_t first{0};
    std::uint32_t last{0};
};

/// @brief A source file resident in the arena.
struct SourceFile {
    std::uint32_t id{0};          ///< 1-based index into the arena
    std::string path;             ///< path as requested, before resolution
    std::string canonical;        ///< absolute, symlink-resolved path; empty if unresolved
    std::string text;             ///< complete contents, newline translation applied
    std::vector<Line> lines;      ///< one entry per line, including a trailing empty line
    std::uint32_t includer{0};    ///< id of the file whose include brought this one in; 0 if root
    std::uint32_t include_offset{0}; ///< byte offset of that include directive

    /// @brief Byte length of the loaded text.
    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(text.size());
    }

    /// @brief Number of lines.  Always at least one for a loaded file.
    [[nodiscard]] std::uint32_t line_count() const noexcept {
        return static_cast<std::uint32_t>(lines.size());
    }
};

/// @brief A resolved position: file, 1-based line, 1-based column, byte offset.
struct ResolvedLocation {
    const SourceFile* file{nullptr};
    std::uint32_t line{1};
    std::uint32_t column{1};
    std::uint32_t offset{0};

    [[nodiscard]] bool is_valid() const noexcept { return file != nullptr; }
};

/// @brief Owns every source file loaded during one invocation.
/// @note Not safe for concurrent mutation.  Resolution of an existing id is
///       safe concurrently with other resolutions, not with load() or clear().
class SourceArena {
public:
    /// @brief Hard ceiling on a single source file, in bytes.
    static constexpr std::uint64_t k_max_file_bytes = 64ull * 1024ull * 1024ull;

    SourceArena() = default;

    SourceArena(const SourceArena&) = delete;
    SourceArena& operator=(const SourceArena&) = delete;
    SourceArena(SourceArena&&) noexcept = default;
    SourceArena& operator=(SourceArena&&) noexcept = default;

    /// @brief Load a file from disk into the arena.
    /// @param[in] path  Path as written in the include or on the command line.
    /// @param[in] base  Directory relative paths resolve against.  Empty means
    ///                  the process working directory.
    /// @param[in] root  Directory the resolved path may not escape.  Empty
    ///                  disables the check.
    /// @param[in] includer        Id of the file requesting the load, or 0.
    /// @param[in] include_offset  Byte offset of the requesting directive.
    ///
    /// @return The id of the loaded file.
    /// @retval FileNotFound, PermissionDenied, IoFailure, DirectoryExpected
    /// @retval PathEscapesRoot  when resolution leaves @p root
    /// @retval FileTooLarge     when the file exceeds k_max_file_bytes
    /// @retval EncodingInvalid  when the contents are not UTF-8
    /// @retval AlreadyLoaded    never; a repeated path is returned as success
    ///                          with the original id.  AlreadyLoaded is reserved
    ///                          for a caller that has asked to reject one.
    ///
    /// @note On success the returned id is stable for the arena's lifetime.
    [[nodiscard]] Result<std::uint32_t> load(std::string_view path,
                                             std::string_view base = {},
                                             std::string_view root = {},
                                             std::uint32_t includer = 0,
                                             std::uint32_t include_offset = 0);

    /// @brief Admit text that did not come from a file.
    /// @param[in] name  Shown in diagnostics in place of a path.
    /// @param[in] text  Contents.  Copied.  Same newline and UTF-8 rules as load().
    /// @return The id of the admitted file.
    [[nodiscard]] Result<std::uint32_t> add(std::string name, std::string text);

    /// @brief Drop every loaded file.
    void clear() noexcept;

    /// @brief Number of loaded files.
    [[nodiscard]] std::uint32_t count() const noexcept {
        return static_cast<std::uint32_t>(files_.size());
    }

    /// @brief True when @p id names a loaded file.
    [[nodiscard]] bool contains(std::uint32_t id) const noexcept {
        return id >= 1 && id <= count();
    }

    /// @brief The file named by @p id, or nullptr when the id is unknown.
    [[nodiscard]] const SourceFile* find(std::uint32_t id) const noexcept;

    /// @brief Every loaded file, in load order.  Valid until the next mutating call.
    [[nodiscard]] std::span<const SourceFile> files() const noexcept { return files_; }

    /// @brief Resolve a location to file, line, and column.
    /// @param[in] loc  Location previously produced against this arena.
    [[nodiscard]] ResolvedLocation resolve(SourceLocation loc) const noexcept;

    /// @brief The line containing @p loc, without its terminator.
    /// @return Empty when the location names no loaded file.
    [[nodiscard]] std::string_view line_text(SourceLocation loc) const noexcept;

    /// @brief Id of a file already loaded from the same canonical path, or 0.
    [[nodiscard]] std::uint32_t find_by_canonical(std::string_view canonical) const noexcept;

private:
    /// @brief Normalize, validate, index, and append.  Shared by load() and add().
    [[nodiscard]] Result<std::uint32_t> admit(std::string path,
                                              std::string canonical,
                                              std::string text,
                                              std::uint32_t includer,
                                              std::uint32_t include_offset);

    std::vector<SourceFile> files_;
};

/// @brief Translate CRLF and CR to LF in place.
/// @param[in,out] text  Buffer to normalize.  Shrinks; never grows.
void normalize_newlines(std::string& text) noexcept;

/// @brief Build the line index for @p text.
/// @param[in] text  Newline-normalized source.
/// @return One Line per line.  A text with no trailing newline still yields a
///         final line covering the tail.  Empty text yields one empty line.
[[nodiscard]] std::vector<Line> index_lines(std::string_view text);

/// @brief Check that @p text is well-formed UTF-8.
/// @param[in] text  Byte sequence to validate.
/// @return The offset of the first ill-formed byte, or nullopt when well-formed.
/// @note Rejects over-long encodings, surrogate code points, and code points
///       above U+10FFFF.  A BOM is well-formed and is not special here.
[[nodiscard]] std::optional<std::uint32_t> first_invalid_utf8(std::string_view text) noexcept;

}  // namespace zxconfig
