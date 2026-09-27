// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/log.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST(Logger, drops_records_finer_than_the_level) {
    auto& log = zxconfig::log::logger();
    log.silence();
    log.set_level(zxconfig::log::Level::Warn);

    EXPECT_TRUE(log.enabled(zxconfig::log::Level::Error));
    EXPECT_TRUE(log.enabled(zxconfig::log::Level::Warn));
    EXPECT_FALSE(log.enabled(zxconfig::log::Level::Info));
    EXPECT_FALSE(log.enabled(zxconfig::log::Level::Trace));

    const auto before = log.emitted();
    zxconfig::log::debug("this must not be written");
    EXPECT_EQ(log.emitted(), before);
}

TEST(Logger, writes_a_file_sink_without_color) {
    const auto path = fs::temp_directory_path() / "zxconfig-log-test";
    fs::remove(path);

    auto& log = zxconfig::log::logger();
    auto opened = log.use_file(path.string());
    ASSERT_TRUE(opened.has_value()) << opened.error().message;

    log.set_level(zxconfig::log::Level::Info);
    zxconfig::log::info("record {}", 7);

    zxconfig::SourceArena arena;
    auto id = arena.add("cfg.zx", "option SMP\n");
    ASSERT_TRUE(id.has_value());
    zxconfig::SourceSpan span;
    span.begin = {*id, 7};
    span.end = {*id, 10};
    log.emit(zxconfig::Diagnostic::error(zxconfig::ErrorCode::UnexpectedToken, "bad token", span), &arena);

    log.silence();

    std::ifstream in{path};
    std::string text{std::istreambuf_iterator<char>{in}, {}};
    fs::remove(path);

    EXPECT_NE(text.find("info: record 7"), std::string::npos);
    EXPECT_NE(text.find("cfg.zx:1:8: error[unexpected-token]: bad token"), std::string::npos);
    EXPECT_NE(text.find("^^^"), std::string::npos);
    EXPECT_EQ(text.find("\033"), std::string::npos);
    EXPECT_GE(log.errors(), 1u);
}

TEST(Logger, reports_a_missing_log_path) {
    auto& log = zxconfig::log::logger();
    auto opened = log.use_file("/no/such/dir/zxconfig.log");
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code, zxconfig::ErrorCode::IoFailure);
}
