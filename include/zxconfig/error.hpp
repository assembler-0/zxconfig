// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace zxconfig {

/// @brief Origin of a diagnostic.  Decides which subsystem owns the failure
///        and how the front end groups it.
enum class ErrorDomain : std::uint8_t {
    None = 0,
    Io,          ///< file open, read, write, path resolution
    Lex,         ///< tokenization
    Parse,       ///< grammar
    Semantic,    ///< name resolution, type agreement, cycles
    Validation,  ///< cross-symbol constraints from the source
    Evaluation,  ///< expression evaluation, probes, interpolation
    Generation,  ///< output backends
    Config,      ///< invocation, cache, command line
    Internal,    ///< violated invariant; always a defect
};

/// @brief Stable identifier for one failure class.
enum class ErrorCode : std::uint16_t {
    Ok = 0,

    FileNotFound      = 100,
    PermissionDenied  = 101,
    IoFailure         = 102,
    PathEscapesRoot   = 103, ///< resolved path left the configuration root
    FileTooLarge      = 104, ///< source exceeds the configured byte ceiling
    EncodingInvalid   = 105, ///< source is not valid UTF-8
    DirectoryExpected = 106,
    AlreadyLoaded     = 107, ///< same canonical path included twice

    UnexpectedCharacter = 200,
    UnterminatedString  = 201,
    UnterminatedComment = 202,
    BadNumber           = 203,
    BadEscape           = 204,

    UnexpectedToken    = 300,
    ExpectedToken      = 301,
    MissingVersion     = 302, ///< file does not open with a version header
    UnsupportedVersion = 303,
    UnbalancedDelimiter = 304,
    UnsupportedConstruct = 305, ///< recognized but not implemented in this build
    UnknownBuiltin     = 306, ///< call to a name that is not a builtin function

    UnknownSymbol       = 400,
    DuplicateSymbol     = 401,
    TypeMismatch        = 402,
    CircularDependency  = 403,
    UnknownType         = 404,
    NamespaceCollision  = 405,
    UndeclaredMacro     = 406,
    ArityMismatch       = 407, ///< macro invoked with the wrong argument count
    DeprecatedSymbol    = 408,
    UnknownChoice       = 409,

    ConstraintViolated = 500,
    RangeViolated      = 501,
    PatternMismatch    = 502,
    PathConstraint     = 503,
    ConflictUnresolved = 504,

    DivisionByZero    = 600,
    ProbeFailed       = 601, ///< shell/env probe failed and no fallback was given
    Interpolation     = 602, ///< ${...} names a symbol that does not exist
    UndefinedValue    = 603, ///< expression read a symbol that has no value

    UnknownBackend  = 700,
    TemplateMissing = 701,
    WriteFailed     = 702,

    BadInvocation    = 800,
    CacheCorrupt     = 801,
    CacheMissing     = 802,
    CacheStale       = 803, ///< manifest digest no longer matches the cache
    CacheUnsupported = 804, ///< cache format version is newer than this engine
    LockBusy         = 805, ///< another invocation holds the cache lock

    Unreachable = 900,
    Invariant   = 901,
};

/// @brief Severity of a recorded condition.
enum class Severity : std::uint8_t {
    Note = 0,
    Warning,
    Error,
    Fatal,
};

/// @brief Byte offset into a loaded source, or into no source at all.
struct SourceLocation {
    std::uint32_t file_id{0};   ///< index into the source arena; 0 is <input>
    std::uint32_t offset{0};    ///< byte offset from the start of that file

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return file_id != 0 || offset != 0;
    }

    [[nodiscard]] friend constexpr bool operator==(SourceLocation, SourceLocation) noexcept = default;
};

/// @brief Half-open byte range [begin, end) within a single source file.
struct SourceSpan {
    SourceLocation begin{};
    SourceLocation end{};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return begin.is_valid(); }

    /// @brief A zero-width span sitting on @p loc.
    [[nodiscard]] static constexpr SourceSpan point(SourceLocation loc) noexcept {
        return {loc, loc};
    }
};

/// @brief One recorded condition.
struct Diagnostic {
    ErrorCode code{ErrorCode::Ok};
    Severity severity{Severity::Error};
    std::string message;
    SourceSpan span{};

    /// @brief Construct an error-severity diagnostic.
    /// @param[in] code     Failure class.
    /// @param[in] message  Rendered explanation.  Moved from.
    /// @param[in] span     Offending source range.  Empty if the condition
    ///                     has no source position.
    [[nodiscard]] static Diagnostic error(ErrorCode code,
                                          std::string message,
                                          SourceSpan span = {}) {
        return {code, Severity::Error, std::move(message), span};
    }

    /// @brief Construct a fatal diagnostic.  Fatal is reserved for conditions
    ///        after which continuing would produce a misleading report.
    [[nodiscard]] static Diagnostic fatal(ErrorCode code,
                                          std::string message,
                                          SourceSpan span = {}) {
        return {code, Severity::Fatal, std::move(message), span};
    }

    /// @brief Construct a warning.  A warning never fails a Result on its own.
    [[nodiscard]] static Diagnostic warning(ErrorCode code,
                                            std::string message,
                                            SourceSpan span = {}) {
        return {code, Severity::Warning, std::move(message), span};
    }

    /// @brief Construct a note, normally attached to an earlier diagnostic.
    [[nodiscard]] static Diagnostic note(ErrorCode code,
                                         std::string message,
                                         SourceSpan span = {}) {
        return {code, Severity::Note, std::move(message), span};
    }
};

/// @brief Outcome of an operation that yields @p T or one diagnostic.
template <typename T>
using Result = std::expected<T, Diagnostic>;

/// @brief Map a code back to the domain that owns it.
/// @param[in] code  Any error code, including Ok.
/// @return The owning domain, or ErrorDomain::None for Ok.
[[nodiscard]] constexpr ErrorDomain domain_of(ErrorCode code) noexcept {
    const auto n = static_cast<std::uint16_t>(code);
    if (n == 0)   return ErrorDomain::None;
    if (n < 200)  return ErrorDomain::Io;
    if (n < 300)  return ErrorDomain::Lex;
    if (n < 400)  return ErrorDomain::Parse;
    if (n < 500)  return ErrorDomain::Semantic;
    if (n < 600)  return ErrorDomain::Validation;
    if (n < 700)  return ErrorDomain::Evaluation;
    if (n < 800)  return ErrorDomain::Generation;
    if (n < 900)  return ErrorDomain::Config;
    return ErrorDomain::Internal;
}

/// @brief Stable, lowercase, single-token name of a domain.  Never null.
[[nodiscard]] constexpr const char* to_string(ErrorDomain domain) noexcept {
    switch (domain) {
        case ErrorDomain::None:       return "none";
        case ErrorDomain::Io:         return "io";
        case ErrorDomain::Lex:        return "lex";
        case ErrorDomain::Parse:      return "parse";
        case ErrorDomain::Semantic:   return "semantic";
        case ErrorDomain::Validation: return "validation";
        case ErrorDomain::Evaluation: return "evaluation";
        case ErrorDomain::Generation: return "generation";
        case ErrorDomain::Config:     return "config";
        case ErrorDomain::Internal:   return "internal";
    }
    return "internal";
}

/// @brief Stable, lowercase name of a severity.  Never null.
[[nodiscard]] constexpr const char* to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::Note:    return "note";
        case Severity::Warning: return "warning";
        case Severity::Error:   return "error";
        case Severity::Fatal:   return "fatal";
    }
    return "error";
}

/// @brief Stable, lowercase, single-token name of a code.  Never null.
[[nodiscard]] constexpr const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:                  return "ok";
        case ErrorCode::FileNotFound:        return "file-not-found";
        case ErrorCode::PermissionDenied:    return "permission-denied";
        case ErrorCode::IoFailure:           return "io-failure";
        case ErrorCode::PathEscapesRoot:     return "path-escapes-root";
        case ErrorCode::FileTooLarge:        return "file-too-large";
        case ErrorCode::EncodingInvalid:     return "encoding-invalid";
        case ErrorCode::DirectoryExpected:   return "directory-expected";
        case ErrorCode::AlreadyLoaded:       return "already-loaded";
        case ErrorCode::UnexpectedCharacter: return "unexpected-character";
        case ErrorCode::UnterminatedString:  return "unterminated-string";
        case ErrorCode::UnterminatedComment: return "unterminated-comment";
        case ErrorCode::BadNumber:           return "bad-number";
        case ErrorCode::BadEscape:           return "bad-escape";
        case ErrorCode::UnexpectedToken:     return "unexpected-token";
        case ErrorCode::ExpectedToken:       return "expected-token";
        case ErrorCode::MissingVersion:      return "missing-version";
        case ErrorCode::UnsupportedVersion:  return "unsupported-version";
        case ErrorCode::UnbalancedDelimiter: return "unbalanced-delimiter";
        case ErrorCode::UnsupportedConstruct: return "unsupported-construct";
        case ErrorCode::UnknownBuiltin:      return "unknown-builtin";
        case ErrorCode::UnknownSymbol:       return "unknown-symbol";
        case ErrorCode::DuplicateSymbol:     return "duplicate-symbol";
        case ErrorCode::TypeMismatch:        return "type-mismatch";
        case ErrorCode::CircularDependency:  return "circular-dependency";
        case ErrorCode::UnknownType:         return "unknown-type";
        case ErrorCode::NamespaceCollision:  return "namespace-collision";
        case ErrorCode::UndeclaredMacro:     return "undeclared-macro";
        case ErrorCode::ArityMismatch:       return "arity-mismatch";
        case ErrorCode::DeprecatedSymbol:    return "deprecated-symbol";
        case ErrorCode::UnknownChoice:       return "unknown-choice";
        case ErrorCode::ConstraintViolated:  return "constraint-violated";
        case ErrorCode::RangeViolated:       return "range-violated";
        case ErrorCode::PatternMismatch:     return "pattern-mismatch";
        case ErrorCode::PathConstraint:      return "path-constraint";
        case ErrorCode::ConflictUnresolved:  return "conflict-unresolved";
        case ErrorCode::DivisionByZero:      return "division-by-zero";
        case ErrorCode::ProbeFailed:         return "probe-failed";
        case ErrorCode::Interpolation:       return "interpolation";
        case ErrorCode::UndefinedValue:      return "undefined-value";
        case ErrorCode::UnknownBackend:      return "unknown-backend";
        case ErrorCode::TemplateMissing:     return "template-missing";
        case ErrorCode::WriteFailed:         return "write-failed";
        case ErrorCode::BadInvocation:       return "bad-invocation";
        case ErrorCode::CacheCorrupt:        return "cache-corrupt";
        case ErrorCode::CacheMissing:        return "cache-missing";
        case ErrorCode::CacheStale:          return "cache-stale";
        case ErrorCode::CacheUnsupported:    return "cache-unsupported";
        case ErrorCode::LockBusy:            return "lock-busy";
        case ErrorCode::Unreachable:         return "unreachable";
        case ErrorCode::Invariant:           return "invariant";
    }
    return "unknown";
}

}  // namespace zxconfig
