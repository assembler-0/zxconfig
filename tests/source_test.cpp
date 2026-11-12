// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/source.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / "zxconfig-tests";
        fs::create_directories(path_);
    }

    ~TempDir() { fs::remove_all(path_); }

    const fs::path& path() const { return path_; }

    fs::path write(const std::string& name, std::string_view text) const {
        const auto full = path_ / name;
        fs::create_directories(full.parent_path());
        std::ofstream out{full, std::ios::binary};
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return full;
    }

private:
    fs::path path_;
};

}  // namespace

TEST(Utf8, accepts_ascii_and_multibyte) {
    EXPECT_FALSE(zxconfig::first_invalid_utf8("abc").has_value());
    EXPECT_FALSE(zxconfig::first_invalid_utf8("π").has_value());
    EXPECT_FALSE(zxconfig::first_invalid_utf8("").has_value());
}

TEST(Utf8, rejects_overlong_and_truncated) {
    const auto overlong = zxconfig::first_invalid_utf8("\xC0\x80");
    ASSERT_TRUE(overlong.has_value());
    EXPECT_EQ(*overlong, 0u);

    const auto truncated = zxconfig::first_invalid_utf8("\xE2\x82");
    ASSERT_TRUE(truncated.has_value());
    EXPECT_EQ(*truncated, 0u);

    const auto surrogate = zxconfig::first_invalid_utf8("\xED\xA0\x80");
    ASSERT_TRUE(surrogate.has_value());
}

TEST(Newlines, crlf_and_bare_cr_become_lf) {
    std::string text = "a\r\nb\rc";
    zxconfig::normalize_newlines(text);
    EXPECT_EQ(text, "a\nb\nc");
}

TEST(LineIndex, counts_with_and_without_trailing_newline) {
    const auto with = zxconfig::index_lines("a\nb\n");
    ASSERT_EQ(with.size(), 3u);
    EXPECT_EQ(with[0].last, 1u);
    EXPECT_EQ(with[2].first, with[2].last);

    const auto without = zxconfig::index_lines("a\nb");
    ASSERT_EQ(without.size(), 2u);
    EXPECT_EQ(without[1].first, 2u);
    EXPECT_EQ(without[1].last, 3u);

    EXPECT_EQ(zxconfig::index_lines("").size(), 1u);
}

TEST(SourceArena, admits_text_and_resolves_positions) {
    zxconfig::SourceArena arena;
    auto id = arena.add("input.zx", "one\ntwo\nthree");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 1u);

    const auto* file = arena.find(*id);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->line_count(), 3u);
    EXPECT_TRUE(file->canonical.empty());

    const auto first = arena.resolve({*id, 0});
    EXPECT_EQ(first.line, 1u);
    EXPECT_EQ(first.column, 1u);

    const auto second = arena.resolve({*id, 4});
    EXPECT_EQ(second.line, 2u);
    EXPECT_EQ(second.column, 1u);
    EXPECT_EQ(arena.line_text({*id, 5}), "two");

    const auto past_end = arena.resolve({*id, 1000});
    EXPECT_TRUE(past_end.is_valid());
    EXPECT_EQ(past_end.offset, file->size());

    EXPECT_FALSE(arena.resolve({0, 0}).is_valid());
    EXPECT_EQ(arena.find(9), nullptr);
}

TEST(SourceArena, strips_bom_on_admit) {
    zxconfig::SourceArena arena;
    auto id = arena.add("bom.zx", "\xEF\xBB\xBFoption");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(arena.find(*id)->text, "option");
}

TEST(SourceArena, rejects_invalid_utf8_on_admit) {
    zxconfig::SourceArena arena;
    auto id = arena.add("bad.zx", "\xFF");
    ASSERT_FALSE(id.has_value());
    EXPECT_EQ(id.error().code, zxconfig::ErrorCode::EncodingInvalid);
}

TEST(SourceArena, loads_a_file_and_deduplicates) {
    TempDir dir;
    const auto file = dir.write("root.zx", "zxconfig 1.1\n");

    zxconfig::SourceArena arena;
    auto first = arena.load(file.string());
    ASSERT_TRUE(first.has_value()) << first.error().message;
    EXPECT_EQ(arena.find(*first)->line_count(), 2u);
    EXPECT_FALSE(arena.find(*first)->canonical.empty());

    auto second = arena.load(file.string());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(arena.count(), 1u);
}

TEST(SourceArena, rejects_a_missing_file) {
    zxconfig::SourceArena arena;
    auto loaded = arena.load("/no/such/zxconfig/file.zx");
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code, zxconfig::ErrorCode::FileNotFound);
}

TEST(SourceArena, rejects_a_directory) {
    TempDir dir;
    zxconfig::SourceArena arena;
    auto loaded = arena.load(dir.path().string());
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code, zxconfig::ErrorCode::DirectoryExpected);
}

TEST(SourceArena, rejects_an_empty_path) {
    zxconfig::SourceArena arena;
    auto loaded = arena.load("");
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code, zxconfig::ErrorCode::FileNotFound);
}

TEST(SourceArena, confines_includes_to_the_root) {
    TempDir dir;
    const auto inside = dir.write("sub/leaf.zx", "zxconfig 1.1\n");
    const auto outside = dir.write("outside.zx", "zxconfig 1.1\n");

    zxconfig::SourceArena arena;
    auto escaped = arena.load("../outside.zx", inside.parent_path().string(), dir.path().string() + "/sub");
    ASSERT_FALSE(escaped.has_value());
    EXPECT_EQ(escaped.error().code, zxconfig::ErrorCode::PathEscapesRoot);

    auto kept = arena.load(outside.string(), {}, outside.parent_path().string());
    EXPECT_TRUE(kept.has_value()) << kept.error().message;
}

TEST(SourceArena, clear_invalidates_ids) {
    zxconfig::SourceArena arena;
    auto id = arena.add("x", "y");
    ASSERT_TRUE(id.has_value());
    arena.clear();
    EXPECT_EQ(arena.count(), 0u);
    EXPECT_EQ(arena.find(*id), nullptr);
}
