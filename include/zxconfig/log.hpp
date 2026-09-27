// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <zxconfig/error.hpp>
#include <zxconfig/source.hpp>

#include <atomic>
#include <cstdio>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace zxconfig::log {

/// @brief Verbosity of the engine log, ordered so that a higher value is a
///        finer level.
enum class Level : std::uint8_t {
    Error = 0,  ///< failures only
    Warn  = 1,  ///< failures and warnings
    Info  = 2,  ///< normal operational record
    Debug = 3,  ///< subsystem decisions
    Trace = 4,  ///< per-token and per-evaluation record
};

/// @brief Where a rendered record goes.
enum class Sink : std::uint8_t {
    Stderr,  ///< the default; unbuffered relative to the TUI alternate screen
    File,    ///< an opened path
    None,    ///< discard; used when a front end renders diagnostics itself
};

/// @brief Process-wide logging configuration.
class Logger {
public:
    /// @brief The process logger.
    [[nodiscard]] static Logger& instance() noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// @brief Raise or lower the recorded level.
    /// @param[in] level  Records strictly finer than this are dropped.
    /// @note Has no effect on diagnostics, which are emitted regardless.
    void set_level(Level level) noexcept;

    [[nodiscard]] Level level() const noexcept;

    /// @brief Request or suppress color.
    void set_color(bool enabled) noexcept;

    [[nodiscard]] bool color() const noexcept;

    /// @brief Direct records to standard error.
    /// @note Closes a previously opened file sink.
    void use_stderr();

    /// @brief Mirror every written record into @p capture.
    void set_capture(std::function<void(Level, const std::string&)> capture);

    /// @brief Direct records to @p path, created or truncated.
    ///
    /// @param[in] path  Destination.  Opened with the process umask.
    /// @retval IoFailure         when the path cannot be opened
    /// @retval PermissionDenied  when the operating system refuses
    ///
    /// @note On failure the previous sink is left in place.
    [[nodiscard]] Result<void> use_file(std::string_view path);

    /// @brief Drop records instead of writing them.
    void silence() noexcept;

    /// @brief Emit one diagnostic.
    /// @param[in] diag   The condition.  Not retained.
    /// @param[in] arena  Arena the span was produced against, or nullptr.
    void emit(const Diagnostic& diag, const SourceArena* arena = nullptr);

    /// @brief Emit a free-form record at @p level.
    template <typename... Args>
    void record(Level level, std::format_string<Args...> fmt, Args&&... args) {
        if (!enabled(level)) return;
        write(level, std::format(fmt, std::forward<Args>(args)...));
    }

    /// @brief True when a record at @p level would be written.
    /// @note Cheap and lock-free; safe to call before building an expensive
    ///       argument that only a trace record would consume.
    [[nodiscard]] bool enabled(Level level) const noexcept;

    /// @brief Number of records actually written since construction.
    [[nodiscard]] std::uint64_t emitted() const noexcept;

    /// @brief Number of error and fatal diagnostics emitted.
    [[nodiscard]] std::uint64_t errors() const noexcept;

    /// @brief Number of warning diagnostics emitted.
    [[nodiscard]] std::uint64_t warnings() const noexcept;

private:
    Logger() noexcept;

    void write(Level level, std::string text);
    void write_diagnostic(const Diagnostic& diag, const SourceArena* arena);

    std::mutex mutex_;
    Level level_{Level::Info};
    Sink sink_{Sink::Stderr};
    bool color_{false};
    std::FILE* file_{nullptr};
    bool owns_file_{false};
    std::function<void(Level, const std::string&)> capture_;

    std::atomic<std::uint64_t> emitted_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> warnings_{0};
};

/// @brief The process logger.  Equivalent to Logger::instance().
[[nodiscard]] inline Logger& logger() noexcept { return Logger::instance(); }

/// @brief Stable, lowercase name of a level.  Never null.
[[nodiscard]] constexpr const char* to_string(Level level) noexcept {
    switch (level) {
        case Level::Error: return "error";
        case Level::Warn:  return "warn";
        case Level::Info:  return "info";
        case Level::Debug: return "debug";
        case Level::Trace: return "trace";
    }
    return "info";
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    logger().record(Level::Error, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    logger().record(Level::Warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    logger().record(Level::Info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    logger().record(Level::Debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    logger().record(Level::Trace, fmt, std::forward<Args>(args)...);
}

/// @brief Emit a free-form record at an explicit level.
template <typename... Args>
void record(Level level, std::format_string<Args...> fmt, Args&&... args) {
    logger().record(level, fmt, std::forward<Args>(args)...);
}

/// @brief Emit a diagnostic through the process logger.
/// @param[in] diag   The condition.
/// @param[in] arena  Arena the span refers to, or nullptr.
inline void emit(const Diagnostic& diag, const SourceArena* arena = nullptr) {
    logger().emit(diag, arena);
}

}  // namespace zxconfig::log
