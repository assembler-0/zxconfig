// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/io.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, {}};
}

}  // namespace

TEST(AtomicWrite, replaces_existing_content_and_leaves_no_temporary) {
    const auto dir = fs::temp_directory_path() / "zxconfig-io-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto target = dir / "out.cfg";
    std::ofstream{target} << "older and much longer content that must vanish\n";

    ASSERT_TRUE(zxconfig::write_atomically(target, "fresh bytes\n").has_value());

    EXPECT_EQ(slurp(target), "fresh bytes\n");
    EXPECT_FALSE(fs::exists(target.string() + ".tmp"));

    fs::remove_all(dir);
}

TEST(AtomicWrite, failure_keeps_target_and_cleans_the_temporary) {
    const auto dir = fs::temp_directory_path() / "zxconfig-io-test-bad";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto target = dir / "keep.cfg";
    std::ofstream{target} << "previous\n";

    // A regular file where the temporary's directory should be makes the
    // open fail with ENOTDIR, independent of process privileges.
    std::ofstream blocker{dir / "block"};
    blocker.close();
    const auto impossible = dir / "block" / "out.cfg";

    const auto result = zxconfig::write_atomically(impossible, "nope\n");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, zxconfig::ErrorCode::WriteFailed);
    EXPECT_EQ(slurp(target), "previous\n");
    EXPECT_FALSE(fs::exists(impossible.string() + ".tmp"));

    fs::remove_all(dir);
}

TEST(AtomicWrite, empty_write_creates_an_empty_file) {
    const auto dir = fs::temp_directory_path() / "zxconfig-io-test-empty";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto target = dir / "empty.cfg";
    ASSERT_TRUE(zxconfig::write_atomically(target, "").has_value());
    EXPECT_TRUE(fs::exists(target));
    EXPECT_EQ(fs::file_size(target), 0u);

    fs::remove_all(dir);
}
