// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/log.hpp>

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <utility>

namespace zxconfig::log {
namespace {

constexpr const char* reset = "\033[0m";

/// @brief ANSI prefix for a record level.  Empty when color is off.
const char* level_color(Level level, bool color) noexcept {
    if (!color) return "";
    switch (level) {
        case Level::Error: return "\033[1m";
        case Level::Warn:  return "\033[1m";
        case Level::Info:  return "";
        case Level::Debug: return "\033[2m";
        case Level::Trace: return "\033[2m";
    }
    return "";
}

const char* level_label(Level level) noexcept {
    switch (level) {
        case Level::Error: return "error";
        case Level::Warn:  return "warn";
        case Level::Info:  return "info";
        case Level::Debug: return "debug";
        case Level::Trace: return "trace";
    }
    return "info";
}

/// @brief Column caret for a span that lies on a single resolved line.
std::string caret(std::uint32_t column, std::uint32_t width) {
    std::string out;
    out.reserve(static_cast<std::size_t>(column) + width);
    out.append(static_cast<std::size_t>(column - 1), ' ');
    out.append(static_cast<std::size_t>(std::max<std::uint32_t>(width, 1)), '^');
    return out;
}

}  // namespace

Logger& Logger::instance() noexcept {
    static Logger logger;
    return logger;
}

Logger::Logger() noexcept {
    color_ = ::isatty(fileno(stderr)) == 1;
}

void Logger::set_level(Level level) noexcept {
    std::scoped_lock lock{mutex_};
    level_ = level;
}

Level Logger::level() const noexcept {
    return level_;
}

void Logger::set_color(bool enabled) noexcept {
    std::scoped_lock lock{mutex_};
    color_ = enabled;
}

bool Logger::color() const noexcept {
    return color_;
}

void Logger::use_stderr() {
    std::scoped_lock lock{mutex_};
    if (owns_file_ && file_ != nullptr) std::fclose(file_);
    file_ = nullptr;
    owns_file_ = false;
    sink_ = Sink::Stderr;
    color_ = ::isatty(fileno(stderr)) == 1;
}

void Logger::set_capture(std::function<void(Level, const std::string&)> capture) {
    std::scoped_lock lock{mutex_};
    capture_ = std::move(capture);
}

Result<void> Logger::use_file(std::string_view path) {
    std::FILE* opened = std::fopen(std::string{path}.c_str(), "w");
    if (opened == nullptr) {
        const auto code = errno == EACCES ? ErrorCode::PermissionDenied : ErrorCode::IoFailure;
        return std::unexpected(Diagnostic::error(
            code, "cannot open log '" + std::string{path} + "': " + std::strerror(errno)));
    }

    std::scoped_lock lock{mutex_};
    if (owns_file_ && file_ != nullptr) std::fclose(file_);
    file_ = opened;
    owns_file_ = true;
    sink_ = Sink::File;
    color_ = false;
    return {};
}

void Logger::silence() noexcept {
    std::scoped_lock lock{mutex_};
    if (owns_file_ && file_ != nullptr) std::fclose(file_);
    file_ = nullptr;
    owns_file_ = false;
    sink_ = Sink::None;
}

bool Logger::enabled(Level level) const noexcept {
    return static_cast<std::uint8_t>(level) <= static_cast<std::uint8_t>(level_);
}

std::uint64_t Logger::emitted() const noexcept { return emitted_.load(std::memory_order_relaxed); }
std::uint64_t Logger::errors() const noexcept  { return errors_.load(std::memory_order_relaxed); }
std::uint64_t Logger::warnings() const noexcept { return warnings_.load(std::memory_order_relaxed); }

void Logger::emit(const Diagnostic& diag, const SourceArena* arena) {
    if (diag.severity == Severity::Warning) {
        warnings_.fetch_add(1, std::memory_order_relaxed);
    } else if (diag.severity == Severity::Error || diag.severity == Severity::Fatal) {
        errors_.fetch_add(1, std::memory_order_relaxed);
    }
    write_diagnostic(diag, arena);
}

void Logger::write(Level level, std::string text) {
    std::scoped_lock lock{mutex_};
    if (sink_ == Sink::None) return;

    const bool use_color = color_ && sink_ == Sink::Stderr;
    const char* paint = level_color(level, use_color);
    const char* end = use_color && paint[0] != '\0' ? reset : "";

    std::FILE* out = sink_ == Sink::File ? file_ : stderr;
    std::fprintf(out, "%s%s%s: %s\n", paint, level_label(level), end, text.c_str());
    std::fflush(out);
    emitted_.fetch_add(1, std::memory_order_relaxed);
    if (capture_) capture_(level, text);
}

void Logger::write_diagnostic(const Diagnostic& diag, const SourceArena* arena) {
    std::scoped_lock lock{mutex_};
    if (sink_ == Sink::None) return;

    const bool use_color = color_ && sink_ == Sink::Stderr;
    const bool bad = diag.severity == Severity::Error || diag.severity == Severity::Fatal;
    const char* paint = use_color ? (bad ? "\033[1m" : "") : "";
    const char* end = use_color && paint[0] != '\0' ? reset : "";

    std::FILE* out = sink_ == Sink::File ? file_ : stderr;

    const ResolvedLocation where =
        arena != nullptr && diag.span.is_valid() ? arena->resolve(diag.span.begin) : ResolvedLocation{};

    if (where.is_valid()) {
        std::fprintf(out, "%s:%u:%u: %s%s%s[%s]: %s\n",
                     where.file->path.c_str(), where.line, where.column,
                     paint, to_string(diag.severity), end,
                     to_string(diag.code), diag.message.c_str());

        const auto text = arena->line_text(diag.span.begin);
        std::fprintf(out, "%.*s\n", static_cast<int>(text.size()), text.data());

        const auto begin_col = where.column;
        std::uint32_t width = 1;
        if (diag.span.end.file_id == diag.span.begin.file_id
            && diag.span.end.offset > diag.span.begin.offset) {
            const auto end_col = diag.span.end.offset - where.offset + where.column;
            width = end_col > begin_col ? end_col - begin_col : 1;
        }
        const auto mark = caret(begin_col, width);
        std::fprintf(out, "%s%s%s\n", paint, mark.c_str(), end);
    } else {
        std::fprintf(out, "%s%s%s[%s]: %s\n",
                     paint, to_string(diag.severity), end,
                     to_string(diag.code), diag.message.c_str());
    }

    std::fflush(out);
    emitted_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace zxconfig::log
