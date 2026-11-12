// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/error.hpp>
#include <zxconfig/hash.hpp>
#include <zxconfig/types.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zxconfig {

/// @brief Cache format version.
inline constexpr std::uint32_t k_cache_format = 1;

/// @brief One assignment a user made.
struct Assignment {
    std::string symbol;
    std::string value;
    ValueType type{ValueType::String};
};

/// @brief A resolved configuration, independent of any output format.
struct CacheDocument {
    std::uint32_t format{k_cache_format};
    std::string manifest;         ///< canonical path of the root manifest
    std::string manifest_digest;  ///< sha256 of the manifest set, lowercase hex
    std::vector<Assignment> assignments;

    /// @brief The assignment for @p symbol, or nullptr when the user never set it.
    [[nodiscard]] const Assignment* find(std::string_view symbol) const noexcept;
};

/// @brief Verdict of comparing a cache against the manifest just loaded.
enum class CacheState : std::uint8_t {
    Absent,   ///< no cache file exists; defaults apply
    Clean,    ///< digest and format match; assignments apply directly
    Stale,    ///< the manifest changed since the cache was written
    Broken,   ///< the file exists but cannot be parsed, or is a newer format
};

/// @brief What an invocation intends to do about the verdict.
enum class CacheDisposition : std::uint8_t {
    Apply,        ///< take the assignments
    ApplyPartial, ///< take the assignments that still name a known symbol
    Reset,        ///< ignore the cache and start from defaults
    Refuse,       ///< stop and make the operator decide
};

/// @brief Outcome of a reconciliation.
struct Reconciliation {
    CacheState state{CacheState::Absent};
    CacheDisposition disposition{CacheDisposition::Reset};
    std::vector<std::string> kept;
    std::vector<std::string> dropped;
    std::vector<std::string> unknown;  ///< assignments whose symbol is not declared
};

/// @brief Stable, lowercase name of a cache verdict.  Never null.
[[nodiscard]] constexpr const char* to_string(CacheState state) noexcept {
    switch (state) {
        case CacheState::Absent:   return "absent";
        case CacheState::Clean:    return "clean";
        case CacheState::Stale:    return "stale";
        case CacheState::Broken:   return "broken";
    }
    return "broken";
}

/// @brief Stable, lowercase name of a reconciliation outcome.  Never null.
[[nodiscard]] constexpr const char* to_string(CacheDisposition disposition) noexcept {
    switch (disposition) {
        case CacheDisposition::Apply:        return "apply";
        case CacheDisposition::ApplyPartial: return "apply-partial";
        case CacheDisposition::Reset:        return "reset";
        case CacheDisposition::Refuse:       return "refuse";
    }
    return "refuse";
}

/// @brief Decide what to do with a cache given the manifest's symbol set.
/// @param[in] state     Verdict from load_cache().
/// @param[in] document  The parsed cache.  Ignored when @p state is Absent.
/// @param[in] declared  Symbols the current manifest declares.
/// @param[in] force     True when the operator asked to apply regardless.
[[nodiscard]] Reconciliation reconcile(CacheState state,
                                       const CacheDocument& document,
                                       std::span<const std::string_view> declared,
                                       bool force);

/// @brief Digest of a manifest set.
/// @param[in] parts  Alternating path, contents, path, contents.
[[nodiscard]] std::string digest_manifest(std::span<const std::string_view> parts);

/// @brief Default cache path for a manifest: `<dir>/.zxconfig.cache`.
[[nodiscard]] std::filesystem::path default_cache_path(const std::filesystem::path& manifest);

/// @brief Resolve a preset reference to a path.
///
/// Search configured directories in declaration order, then the manifest's
/// directory. Relative search directories resolve against the root manifest.
/// An empty search list defaults to presets/. Absolute names are used as given.
///
/// @param[in] name      Preset reference as written on the command line or
///                      typed in the dialog.
/// @param[in] manifest  Path of the root manifest.
/// @param[in] directories Ordered search directories from presets_dir directives.
[[nodiscard]] std::filesystem::path resolve_preset(std::string_view name,
                                                   const std::filesystem::path& manifest,
                                                   std::span<const std::string> directories = {});

/// @brief Discover presets across the same paths used by resolve_preset().
/// Returns exact absolute paths, sorted within each directory and deduplicated.
[[nodiscard]] std::vector<std::string> discover_presets(
    const std::filesystem::path& manifest, std::span<const std::string> directories = {});

/// @brief A deprecated-name rewrite: the old spelling, its replacement, and
///        the message the spec attaches to the rename.
struct Rename {
    std::string_view old_name;
    std::string_view new_name;
    std::string_view message;
};

/// @brief Rewrite deprecated symbol names in @p document.
///
/// The spec: an old name "still parses, but tools warn and rewrite".  One
/// report is returned per old name encountered; when a document holds both
/// spellings the new assignment wins and the old one is dropped, reported as
/// a clash rather than silently overwritten.
///
/// @return Human-readable reports, one per rewritten or dropped assignment.
[[nodiscard]] std::vector<std::string> rename_assignments(
    CacheDocument& document, std::span<const Rename> renames);

/// @brief Read and parse a cache file.
/// @param[in] path             Cache path.
/// @param[in] expected_digest  Digest of the manifest just loaded.  Empty
///                             skips the staleness check.
/// @retval CacheMissing      when no file exists at @p path
/// @retval CacheCorrupt      when the file does not parse
/// @retval CacheUnsupported  when the format version is newer than supported
/// @retval CacheStale        when the digest does not match
[[nodiscard]] Result<CacheDocument> load_cache(const std::filesystem::path& path,
                                               std::string_view expected_digest = {});

/// @brief Write a cache file atomically.
/// @param[in] path      Destination.
/// @param[in] document  Document to write.  Its format must be k_cache_format.
/// @retval WriteFailed        when the file cannot be created or renamed
/// @retval PermissionDenied   when the operating system refuses
/// @retval UnsupportedVersion conceptually; an unsupported format is an
///                            Invariant, because only this engine writes caches.
[[nodiscard]] Result<void> store_cache(const std::filesystem::path& path,
                                       const CacheDocument& document);

/// @brief Take @p path's lock and write the cache atomically.
///
/// The one persistence core for every writer: the CLI's `--save` and the
/// interface's save action both persist through it, so lock discipline and
/// durability cannot drift between them.
/// @param[in] path          Destination.
/// @param[in] document      Document to write.  Its format must be k_cache_format.
/// @param[in] lock_timeout  Seconds to wait for the lock.  Zero tries once.
/// @retval LockBusy         when another invocation holds it past @p lock_timeout
/// @retval WriteFailed      when the file cannot be created or renamed
/// @retval PermissionDenied when the operating system refuses
[[nodiscard]] Result<void> store_cache_locked(const std::filesystem::path& path,
                                              const CacheDocument& document,
                                              std::uint32_t lock_timeout);

/// @brief Render a document to its textual form.
[[nodiscard]] std::string format_cache(const CacheDocument& document);

/// @brief Parse a document from its textual form.
/// @retval CacheCorrupt      on a malformed line or a duplicated symbol
/// @retval CacheUnsupported  on a newer format version
[[nodiscard]] Result<CacheDocument> parse_cache(std::string_view text);

/// @brief An exclusive advisory lock on a cache file.
class CacheLock {
public:
    CacheLock() = default;
    ~CacheLock();

    CacheLock(const CacheLock&) = delete;
    CacheLock& operator=(const CacheLock&) = delete;
    CacheLock(CacheLock&& other) noexcept;
    CacheLock& operator=(CacheLock&& other) noexcept;

    /// @brief Acquire the lock for @p cache_path.
    ///
    /// @param[in] cache_path   The cache the lock guards.
    /// @param[in] timeout      Seconds to wait before giving up.  Zero tries once.
    /// @retval LockBusy     when another invocation holds it past @p timeout
    /// @retval WriteFailed  when the lock file cannot be created
    [[nodiscard]] static Result<CacheLock> acquire(const std::filesystem::path& cache_path,
                                                   std::uint32_t timeout_seconds = 5);

    /// @brief True when this object currently holds a lock.
    [[nodiscard]] bool held() const noexcept { return fd_ >= 0; }

    /// @brief Release early.  Destructor-safe to call again.
    void release() noexcept;

private:
    int fd_{-1};
};

}  // namespace zxconfig
