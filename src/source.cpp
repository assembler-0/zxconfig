// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/source.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace zxconfig {
namespace {

namespace fs = std::filesystem;

/// @brief Map an errno-class failure onto the I/O code that names it.
ErrorCode code_for(const std::error_code& ec) noexcept {
    if (ec == std::errc::no_such_file_or_directory) return ErrorCode::FileNotFound;
    if (ec == std::errc::permission_denied)         return ErrorCode::PermissionDenied;
    if (ec == std::errc::is_a_directory)            return ErrorCode::DirectoryExpected;
    return ErrorCode::IoFailure;
}

/// @brief True when @p candidate is @p root or a lexical descendant of it.
bool is_within(const fs::path& root, const fs::path& candidate) {
    auto root_it = root.begin();
    auto cand_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++cand_it) {
        if (cand_it == candidate.end() || *root_it != *cand_it) return false;
    }
    return true;
}

}  // namespace

void normalize_newlines(std::string& text) noexcept {
    std::size_t write = 0;
    for (std::size_t read = 0; read < text.size(); ++read) {
        const char c = text[read];
        if (c == '\r') {
            text[write++] = '\n';
            if (read + 1 < text.size() && text[read + 1] == '\n') ++read;
            continue;
        }
        text[write++] = c;
    }
    text.resize(write);
}

std::vector<Line> index_lines(std::string_view text) {
    std::vector<Line> lines;
    lines.reserve(text.size() / 32 + 1);

    std::uint32_t start = 0;
    for (std::uint32_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\n') continue;
        lines.push_back(Line{start, i});
        start = i + 1;
    }
    lines.push_back(Line{start, static_cast<std::uint32_t>(text.size())});
    return lines;
}

std::optional<std::uint32_t> first_invalid_utf8(std::string_view text) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const auto n = text.size();
    std::size_t i = 0;

    while (i < n) {
        const unsigned char c = p[i];
        std::size_t need = 0;
        char32_t cp = 0;

        if (c <= 0x7F) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0)      { need = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 4; cp = c & 0x07; }
        else return static_cast<std::uint32_t>(i);

        if (i + need > n) return static_cast<std::uint32_t>(i);

        for (std::size_t k = 1; k < need; ++k) {
            const unsigned char cont = p[i + k];
            if ((cont & 0xC0) != 0x80) return static_cast<std::uint32_t>(i);
            cp = (cp << 6) | (cont & 0x3F);
        }

        const char32_t min = need == 2 ? 0x80 : need == 3 ? 0x800 : 0x10000;
        if (cp < min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return static_cast<std::uint32_t>(i);
        }
        i += need;
    }
    return std::nullopt;
}

Result<std::uint32_t> SourceArena::load(std::string_view path,
                                        std::string_view base,
                                        std::string_view root,
                                        std::uint32_t includer,
                                        std::uint32_t include_offset) {
    std::error_code ec;
    fs::path requested{std::string{path}};
    if (requested.empty()) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::FileNotFound, "empty path"));
    }
    if (requested.is_relative()) {
        const fs::path against = base.empty() ? fs::current_path(ec) : fs::path{std::string{base}};
        if (ec) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::IoFailure,
                "cannot resolve working directory: " + ec.message()));
        }
        requested = against / requested;
    }

    const fs::path canonical = fs::weakly_canonical(requested, ec);
    if (ec) {
        return std::unexpected(Diagnostic::error(
            code_for(ec),
            "cannot resolve '" + requested.string() + "': " + ec.message()));
    }

    if (!root.empty()) {
        const fs::path root_canonical = fs::weakly_canonical(fs::path{std::string{root}}, ec);
        if (ec) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::IoFailure,
                "cannot resolve root '" + std::string{root} + "': " + ec.message()));
        }
        if (!is_within(root_canonical, canonical)) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::PathEscapesRoot,
                "'" + std::string{path} + "' resolves to '" + canonical.string()
                    + "', which is outside '" + root_canonical.string() + "'"));
        }
    }

    const auto canonical_text = canonical.string();
    if (const auto prior = find_by_canonical(canonical_text); prior != 0) {
        return prior;
    }

    const fs::file_status status = fs::status(canonical, ec);
    if (ec) {
        return std::unexpected(Diagnostic::error(
            code_for(ec),
            "cannot stat '" + canonical_text + "': " + ec.message()));
    }
    if (fs::is_directory(status)) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::DirectoryExpected,
            "'" + canonical_text + "' is a directory"));
    }
    if (!fs::is_regular_file(status)) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::IoFailure,
            "'" + canonical_text + "' is not a regular file"));
    }

    const auto bytes = fs::file_size(canonical, ec);
    if (ec) {
        return std::unexpected(Diagnostic::error(
            code_for(ec),
            "cannot size '" + canonical_text + "': " + ec.message()));
    }
    if (bytes > k_max_file_bytes) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::FileTooLarge,
            "'" + canonical_text + "' is " + std::to_string(bytes)
                + " bytes, past the " + std::to_string(k_max_file_bytes) + " byte limit"));
    }

    std::ifstream in{canonical, std::ios::in | std::ios::binary};
    if (!in) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::PermissionDenied,
            "cannot open '" + canonical_text + "': " + std::strerror(errno)));
    }

    std::string text(static_cast<std::size_t>(bytes), '\0');
    in.read(text.data(), static_cast<std::streamsize>(bytes));
    if (in.bad()) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::IoFailure,
            "error reading '" + canonical_text + "'"));
    }
    // A file can shrink between the size query and the read.  Trust the read.
    text.resize(static_cast<std::size_t>(in.gcount()));

    return admit(std::string{path}, canonical_text, std::move(text), includer, include_offset);
}

Result<std::uint32_t> SourceArena::add(std::string name, std::string text) {
    if (name.empty()) name = "<input>";
    return admit(std::move(name), {}, std::move(text), 0, 0);
}

Result<std::uint32_t> SourceArena::admit(std::string path,
                                         std::string canonical,
                                         std::string text,
                                         std::uint32_t includer,
                                         std::uint32_t include_offset) {
    if (text.size() >= 3
        && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    normalize_newlines(text);

    if (text.size() > k_max_file_bytes) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::FileTooLarge,
            "'" + path + "' exceeds the " + std::to_string(k_max_file_bytes) + " byte limit"));
    }

    if (const auto bad = first_invalid_utf8(text); bad.has_value()) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::EncodingInvalid,
            "'" + path + "' is not valid UTF-8 at byte " + std::to_string(*bad)));
    }

    SourceFile file;
    file.id = count() + 1;
    file.path = std::move(path);
    file.canonical = std::move(canonical);
    file.text = std::move(text);
    file.includer = includer;
    file.include_offset = include_offset;

    file.text.reserve(std::max<std::size_t>(file.text.size(), 32));
    file.lines = index_lines(file.text);

    files_.push_back(std::move(file));
    return files_.back().id;
}

void SourceArena::clear() noexcept {
    files_.clear();
}

const SourceFile* SourceArena::find(std::uint32_t id) const noexcept {
    if (!contains(id)) return nullptr;
    return &files_[id - 1];
}

ResolvedLocation SourceArena::resolve(SourceLocation loc) const noexcept {
    const SourceFile* file = find(loc.file_id);
    if (file == nullptr) return {};

    const auto offset = std::min(loc.offset, file->size());
    std::uint32_t lo = 0;
    std::uint32_t hi = file->line_count();
    while (lo + 1 < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (file->lines[mid].first <= offset) lo = mid;
        else hi = mid;
    }

    const Line& line = file->lines[lo];
    ResolvedLocation out;
    out.file = file;
    out.line = lo + 1;
    out.column = offset - line.first + 1;
    out.offset = offset;
    return out;
}

std::string_view SourceArena::line_text(SourceLocation loc) const noexcept {
    const auto resolved = resolve(loc);
    if (!resolved.is_valid()) return {};
    const Line& line = resolved.file->lines[resolved.line - 1];
    return std::string_view{resolved.file->text}.substr(line.first, line.last - line.first);
}

std::uint32_t SourceArena::find_by_canonical(std::string_view canonical) const noexcept {
    if (canonical.empty()) return 0;
    for (const auto& file : files_) {
        if (file.canonical == canonical) return file.id;
    }
    return 0;
}

}  // namespace zxconfig
