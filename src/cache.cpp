// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/cache.hpp>
#include <zxconfig/hash.hpp>
#include <zxconfig/text.hpp>
#include <zxconfig/version.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace zxconfig {
namespace {

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief A legal symbol name: one or more identifier segments joined by
///        dots.  The reader must accept exactly the names the parser can
///        produce, namespaces included — rejecting `arch.KASLR` here made
///        every save through a namespaced include unloadable.
bool legal_symbol(std::string_view name) noexcept {
    if (name.empty()) return false;
    std::size_t segment = 0;
    for (const char c : name) {
        if (c == '.') {
            if (segment == 0) return false;  // empty segment
            segment = 0;
            continue;
        }
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (segment == 0 ? !alpha : !(alpha || digit)) return false;
        ++segment;
    }
    return segment != 0;  // no trailing dot
}

/// @brief Quote @p value so it occupies exactly one line.
std::string quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    out += "\\u{";
                    out.push_back(digits[c >> 4]);
                    out.push_back(digits[c & 0xF]);
                    out.push_back('}');
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

Result<std::string> unquote(std::string_view text, std::uint32_t line_no) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return std::unexpected(Diagnostic::error(
            ErrorCode::CacheCorrupt,
            "line " + std::to_string(line_no) + ": value is not quoted"));
    }
    // The body goes through the shared escape decoder, so a cache value and a
    // manifest string literal decode identically.
    text.remove_prefix(1);
    text.remove_suffix(1);

    auto decoded = decode_escapes(text);
    if (!decoded.ok()) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::CacheCorrupt,
            "line " + std::to_string(line_no)
                + ": bad escape at byte " + std::to_string(*decoded.error)));
    }
    return std::move(decoded.value);
}

bool declared_contains(std::span<const std::string_view> declared, std::string_view symbol) {
    return std::find(declared.begin(), declared.end(), symbol) != declared.end();
}

}  // namespace

const Assignment* CacheDocument::find(std::string_view symbol) const noexcept {
    for (const auto& assignment : assignments) {
        if (assignment.symbol == symbol) return &assignment;
    }
    return nullptr;
}


std::string digest_manifest(std::span<const std::string_view> parts) {
    return digest_parts(parts);
}

std::vector<std::string> rename_assignments(CacheDocument& document,
                                            std::span<const Rename> renames) {
    std::vector<std::string> reports;
    for (const auto& rename : renames) {
        for (auto it = document.assignments.begin(); it != document.assignments.end();) {
            if (it->symbol != rename.old_name) {
                ++it;
                continue;
            }
            if (document.find(rename.new_name) != nullptr) {
                reports.push_back("'" + std::string{rename.old_name}
                    + "' and '" + std::string{rename.new_name}
                    + "' are both present; keeping '" + std::string{rename.new_name} + "'");
                it = document.assignments.erase(it);
                continue;
            }
            it->symbol = std::string{rename.new_name};
            reports.push_back(rename.message.empty()
                ? "'" + std::string{rename.old_name} + "' was renamed to '"
                    + std::string{rename.new_name} + "'"
                : std::string{rename.message});
            ++it;
        }
    }
    return reports;
}

std::filesystem::path default_cache_path(const std::filesystem::path& manifest) {
    auto dir = manifest.parent_path();
    if (dir.empty()) dir = ".";
    return dir / ".zxconfig.cache";
}

std::filesystem::path resolve_preset(std::string_view name,
                                     const std::filesystem::path& manifest) {
    std::filesystem::path target{std::string{name}};
    if (target.is_absolute()) return target;
    const auto base = manifest.parent_path().empty()
        ? std::filesystem::path{"."} : manifest.parent_path();

    // Candidates in order: presets/ first (what the dialog lists), then the
    // manifest's own directory; each with the conventional extension
    // appended, so a bare `ci` finds presets/ci.zxconfig.
    const std::filesystem::path dirs[] = {base / "presets", base};
    for (const auto& dir : dirs) {
        for (const std::string_view suffix : {"", ".zxconfig"}) {
            const auto candidate = dir / (target.string() + std::string{suffix});
            if (std::filesystem::exists(candidate)) return candidate;
        }
    }
    return base / target;
}

std::string format_cache(const CacheDocument& document) {
    auto ordered = document.assignments;
    std::sort(ordered.begin(), ordered.end(),
              [](const Assignment& a, const Assignment& b) { return a.symbol < b.symbol; });

    std::string out;
    out += "zxcache ";
    out += std::to_string(document.format);
    out += "\nwriter zxconfig ";
    out += k_version;
    out += "\nmanifest ";
    out += quote(document.manifest);
    out += "\ndigest ";
    out += document.manifest_digest;
    out += '\n';
    for (const auto& assignment : ordered) {
        out += "set ";
        out += assignment.symbol;
        out += ' ';
        out += to_string(assignment.type);
        out += ' ';
        out += quote(assignment.value);
        out += '\n';
    }
    return out;
}

Result<CacheDocument> parse_cache(std::string_view text) {
    CacheDocument document;
    bool saw_header = false;
    std::uint32_t line_no = 0;

    while (!text.empty()) {
        const auto cut = text.find('\n');
        auto line = trim(text.substr(0, cut));
        text.remove_prefix(cut == std::string_view::npos ? text.size() : cut + 1);
        ++line_no;
        if (line.empty() || line.front() == '#') continue;

        const auto fail = [&](std::string message) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::CacheCorrupt,
                "line " + std::to_string(line_no) + ": " + std::move(message)));
        };

        if (!saw_header) {
            constexpr std::string_view prefix = "zxcache ";
            if (!line.starts_with(prefix)) return fail("missing zxcache header");
            const auto version_text = line.substr(prefix.size());
            std::uint32_t version = 0;
            for (const char c : version_text) {
                if (c < '0' || c > '9') return fail("malformed version");
                version = version * 10 + static_cast<std::uint32_t>(c - '0');
            }
            if (version == 0) return fail("malformed version");
            if (version > k_cache_format) {
                return std::unexpected(Diagnostic::error(
                    ErrorCode::CacheUnsupported,
                    "cache format " + std::to_string(version)
                        + " is newer than supported format " + std::to_string(k_cache_format)));
            }
            document.format = version;
            saw_header = true;
            continue;
        }

        const auto field = line.substr(0, line.find(' '));
        const auto rest = trim(line.substr(field.size()));

        if (field == "manifest") {
            auto value = unquote(rest, line_no);
            if (!value) return std::unexpected(value.error());
            document.manifest = std::move(*value);
        } else if (field == "digest") {
            if (rest.size() != 64) return fail("digest must be 64 hex digits");
            document.manifest_digest = std::string{rest};
        } else if (field == "writer") {
            // Informational: records which build wrote the cache.  Readers
            // ignore the contents entirely.
        } else if (field == "set") {
            const auto symbol_end = rest.find(' ');
            if (symbol_end == std::string_view::npos) return fail("set requires symbol, form, value");
            const auto symbol = rest.substr(0, symbol_end);
            if (!legal_symbol(symbol)) return fail("illegal symbol name");

            const auto after_symbol = trim(rest.substr(symbol_end));
            const auto form_end = after_symbol.find(' ');
            if (form_end == std::string_view::npos) return fail("set requires a type and a value");

            const auto type = parse_value_type(after_symbol.substr(0, form_end));
            if (!type.has_value()) return fail("unknown value type");

            auto value = unquote(trim(after_symbol.substr(form_end)), line_no);
            if (!value) return std::unexpected(value.error());
            if (document.find(symbol) != nullptr) return fail("duplicate symbol " + std::string{symbol});

            document.assignments.push_back(
                Assignment{std::string{symbol}, std::move(*value), *type});
        } else {
            return fail("unknown field '" + std::string{field} + "'");
        }
    }

    if (!saw_header) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::CacheCorrupt, "empty cache"));
    }
    return document;
}

Result<CacheDocument> load_cache(const std::filesystem::path& path,
                                 std::string_view expected_digest) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::CacheMissing,
            "no cache at '" + path.string() + "'"));
    }

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        const auto code = errno == EACCES ? ErrorCode::PermissionDenied : ErrorCode::IoFailure;
        return std::unexpected(Diagnostic::error(
            code, "cannot open '" + path.string() + "': " + std::strerror(errno)));
    }
    const std::string text{std::istreambuf_iterator<char>{in}, {}};

    // An empty or blank file is an absent cache, not a broken one: clearing
    // the file is how one starts over, and it must not read as corruption.
    if (std::all_of(text.begin(), text.end(),
                    [](unsigned char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; })) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::CacheMissing, "'" + path.string() + "' is empty"));
    }

    auto document = parse_cache(text);
    if (!document) return std::unexpected(document.error());

    if (!expected_digest.empty() && document->manifest_digest != expected_digest) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::CacheStale,
            "cache digest " + document->manifest_digest
                + " does not match manifest digest " + std::string{expected_digest}));
    }
    return document;
}

Result<void> store_cache(const std::filesystem::path& path, const CacheDocument& document) {
    if (document.format != k_cache_format) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::Invariant,
            "refusing to write cache format " + std::to_string(document.format)));
    }

    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out) {
            return std::unexpected(Diagnostic::error(
                ErrorCode::WriteFailed,
                "cannot create '" + temporary + "'"));
        }
        const auto rendered = format_cache(document);
        out.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
        if (!out) {
            std::filesystem::remove(temporary);
            return std::unexpected(Diagnostic::error(
                ErrorCode::WriteFailed, "error writing '" + temporary + "'"));
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        return std::unexpected(Diagnostic::error(
            ErrorCode::WriteFailed,
            "cannot replace '" + path.string() + "': " + ec.message()));
    }
    return {};
}

Reconciliation reconcile(CacheState state,
                         const CacheDocument& document,
                         std::span<const std::string_view> declared,
                         bool force) {
    Reconciliation out;
    out.state = state;

    if (state == CacheState::Absent) {
        out.disposition = CacheDisposition::Reset;
        return out;
    }
    if ((state == CacheState::Stale || state == CacheState::Broken) && !force) {
        out.disposition = CacheDisposition::Refuse;
        return out;
    }

    out.disposition = state == CacheState::Clean ? CacheDisposition::Apply
                                                 : CacheDisposition::ApplyPartial;
    for (const auto& assignment : document.assignments) {
        if (declared_contains(declared, assignment.symbol)) {
            out.kept.push_back(assignment.symbol);
        } else if (state == CacheState::Clean) {
            out.unknown.push_back(assignment.symbol);
            out.kept.push_back(assignment.symbol);
        } else {
            out.dropped.push_back(assignment.symbol);
        }
    }
    return out;
}

CacheLock::~CacheLock() { release(); }

CacheLock::CacheLock(CacheLock&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

CacheLock& CacheLock::operator=(CacheLock&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void CacheLock::release() noexcept {
    if (fd_ < 0) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
}

Result<CacheLock> CacheLock::acquire(const std::filesystem::path& cache_path,
                                     std::uint32_t timeout_seconds) {
    const auto lock_path = cache_path.string() + ".lock";
    const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return std::unexpected(Diagnostic::error(
            ErrorCode::WriteFailed,
            "cannot create lock '" + lock_path + "': " + std::strerror(errno)));
    }

    const auto deadline = ::time(nullptr) + static_cast<time_t>(timeout_seconds);
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            CacheLock lock;
            lock.fd_ = fd;
            return lock;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            ::close(fd);
            return std::unexpected(Diagnostic::error(
                ErrorCode::IoFailure,
                "cannot lock '" + lock_path + "': " + std::strerror(errno)));
        }
        if (::time(nullptr) >= deadline) {
            ::close(fd);
            return std::unexpected(Diagnostic::error(
                ErrorCode::LockBusy,
                "'" + cache_path.string() + "' is locked by another invocation"));
        }
        ::usleep(50'000);
    }
}

}  // namespace zxconfig
