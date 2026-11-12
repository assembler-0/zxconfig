// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/cache.hpp>
#include <zxconfig/version.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

TEST(Sha256, matches_the_known_empty_digest) {
    EXPECT_EQ(zxconfig::sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, matches_a_known_digest) {
    EXPECT_EQ(zxconfig::sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(CacheFormat, round_trips_assignments) {
    zxconfig::CacheDocument document;
    document.manifest = "/src/ZXConfig";
    document.manifest_digest = zxconfig::sha256_hex("zxconfig 1.1\n");
    document.assignments.push_back({"SMP", "true", zxconfig::ValueType::Bool});
    document.assignments.push_back({"VERSION", "6.1 \"rc\"", zxconfig::ValueType::String});
    document.assignments.push_back({"BASE", "0xFFFF", zxconfig::ValueType::Hex});

    const auto rendered = zxconfig::format_cache(document);
    EXPECT_EQ(rendered.find("zxcache 1\n"), 0u);
    EXPECT_NE(rendered.find("writer zxconfig " + std::string{zxconfig::k_version}),
              std::string::npos);

    auto parsed = zxconfig::parse_cache(rendered);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(parsed->manifest, document.manifest);
    EXPECT_EQ(parsed->assignments.size(), 3u);

    const auto* version = parsed->find("VERSION");
    ASSERT_NE(version, nullptr);
    EXPECT_EQ(version->value, "6.1 \"rc\"");
    EXPECT_EQ(version->type, zxconfig::ValueType::String);
}

TEST(CacheFormat, round_trips_control_bytes_through_the_shared_decoder) {
    zxconfig::CacheDocument document;
    document.manifest_digest = std::string(64, 'a');
    document.assignments.push_back({"PATHS", std::string{"a\x01" "b"}, zxconfig::ValueType::List});

    const auto rendered = zxconfig::format_cache(document);
    EXPECT_NE(rendered.find("\\u{01}"), std::string::npos);

    auto parsed = zxconfig::parse_cache(rendered);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(parsed->find("PATHS")->value, std::string{"a\x01" "b"});
}

TEST(CacheFormat, accepts_unicode_escapes_like_the_lexer) {
    auto parsed = zxconfig::parse_cache(
        "zxcache 1\nset NAME string \"caf\\u{00e9}\"\n");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(parsed->find("NAME")->value, "caf\xC3\xA9");
}

TEST(CacheFormat, orders_symbols_regardless_of_insertion) {
    zxconfig::CacheDocument document;
    document.manifest_digest = std::string(64, 'a');
    document.assignments.push_back({"ZED", "1", zxconfig::ValueType::Int});
    document.assignments.push_back({"ALPHA", "2", zxconfig::ValueType::Int});

    const auto rendered = zxconfig::format_cache(document);
    EXPECT_LT(rendered.find("ALPHA"), rendered.find("ZED"));
}

TEST(CacheFormat, rejects_a_newer_version) {
    auto parsed = zxconfig::parse_cache("zxcache 9\n");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, zxconfig::ErrorCode::CacheUnsupported);
}

TEST(CacheFormat, rejects_a_duplicate_symbol) {
    const char* text =
        "zxcache 1\n"
        "set SMP bool \"true\"\n"
        "set SMP bool \"false\"\n";
    auto parsed = zxconfig::parse_cache(text);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, zxconfig::ErrorCode::CacheCorrupt);
}

TEST(CacheFormat, rejects_an_unquoted_value) {
    auto parsed = zxconfig::parse_cache("zxcache 1\nset SMP bool true\n");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, zxconfig::ErrorCode::CacheCorrupt);
}

TEST(CacheStore, writes_atomically_and_reloads) {
    const auto dir = fs::temp_directory_path() / "zxconfig-cache-test";
    fs::remove_all(dir);
    fs::create_directory(dir);
    const auto path = dir / ".zxconfig.cache";

    zxconfig::CacheDocument document;
    document.manifest = (dir / "ZXConfig").string();
    document.manifest_digest = zxconfig::sha256_hex("manifest");
    document.assignments.push_back({"SMP", "true", zxconfig::ValueType::Bool});

    auto stored = zxconfig::store_cache(path, document);
    ASSERT_TRUE(stored.has_value()) << stored.error().message;
    EXPECT_FALSE(fs::exists(path.string() + ".tmp"));

    auto loaded = zxconfig::load_cache(path, document.manifest_digest);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->find("SMP")->value, "true");

    auto stale = zxconfig::load_cache(path, std::string(64, 'b'));
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().code, zxconfig::ErrorCode::CacheStale);

    fs::remove_all(dir);
}

TEST(CacheStore, reports_a_missing_cache) {
    auto loaded = zxconfig::load_cache("/no/such/zxconfig.cache");
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code, zxconfig::ErrorCode::CacheMissing);
}

TEST(PresetPaths, ordered_directories_replace_default_and_empty_means_root) {
    const auto dir = fs::temp_directory_path() / "zxconfig-preset-search";
    fs::create_directories(dir / "first");
    fs::create_directories(dir / "second");
    fs::create_directories(dir / "presets");
    const auto manifest = dir / "ZXConfig";
    std::ofstream{dir / "first/ci.zxconfig"} << "zxcache 1\n";
    std::ofstream{dir / "second/ci.zxconfig"} << "zxcache 1\n";
    std::ofstream{dir / "second/custom"} << "zxcache 1\n";
    std::ofstream{dir / "presets/legacy.zxconfig"} << "zxcache 1\n";
    std::ofstream{dir / "local.zxconfig"} << "zxcache 1\n";
    fs::create_directory(dir / "first/custom");
    const std::vector<std::string> paths{"missing", "first", "second"};
    EXPECT_EQ(zxconfig::resolve_preset("ci", manifest, paths), dir / "first/ci.zxconfig");
    EXPECT_EQ(zxconfig::resolve_preset("custom", manifest, paths), dir / "second/custom");
    EXPECT_EQ(zxconfig::resolve_preset("local", manifest, paths), dir / "local.zxconfig");
    EXPECT_EQ(zxconfig::resolve_preset("legacy", manifest, paths), dir / "legacy");
    EXPECT_EQ(zxconfig::resolve_preset("legacy", manifest), dir / "presets/legacy.zxconfig");
    EXPECT_EQ(zxconfig::resolve_preset("local", manifest, std::vector<std::string>{""}), dir / "local.zxconfig");
    EXPECT_EQ(zxconfig::resolve_preset((dir / "second/ci.zxconfig").string(), manifest, paths),
              dir / "second/ci.zxconfig");
    fs::remove_all(dir);
}

TEST(PresetPaths, discovery_keeps_exact_paths_and_order_without_lock_files) {
    const auto dir = fs::temp_directory_path() / "zxconfig-preset-discovery";
    fs::create_directories(dir / "a");
    fs::create_directories(dir / "b");
    std::ofstream{dir / "a/ci.zxconfig"} << "zxcache 1\n";
    std::ofstream{dir / "a/ci.zxconfig.lock"} << "";
    std::ofstream{dir / "a/notes.md"} << "not a preset";
    std::ofstream{dir / "b/ci.zxconfig"} << "zxcache 1\n";
    const std::vector<std::string> paths{"missing", "a", "a", (dir / "b").string()};
    const auto found = zxconfig::discover_presets(dir / "ZXConfig", paths);
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0], (dir / "a/ci.zxconfig").string());
    EXPECT_EQ(found[1], (dir / "b/ci.zxconfig").string());
    EXPECT_EQ(zxconfig::resolve_preset(found[1], dir / "ZXConfig", paths), dir / "b/ci.zxconfig");
    fs::remove_all(dir);
}

TEST(Reconcile, absent_resets_and_clean_applies) {
    zxconfig::CacheDocument document;
    document.assignments.push_back({"SMP", "true", zxconfig::ValueType::Bool});
    document.assignments.push_back({"GONE", "1", zxconfig::ValueType::Int});

    const std::string_view declared[] = {"SMP", "NR_CPUS"};

    const auto absent = zxconfig::reconcile(zxconfig::CacheState::Absent, document, declared, false);
    EXPECT_EQ(absent.disposition, zxconfig::CacheDisposition::Reset);

    const auto clean = zxconfig::reconcile(zxconfig::CacheState::Clean, document, declared, false);
    EXPECT_EQ(clean.disposition, zxconfig::CacheDisposition::Apply);
    EXPECT_EQ(clean.unknown.size(), 1u);
    EXPECT_EQ(clean.unknown.front(), "GONE");
}

TEST(Reconcile, stale_refuses_unless_forced) {
    zxconfig::CacheDocument document;
    document.assignments.push_back({"SMP", "true", zxconfig::ValueType::Bool});
    document.assignments.push_back({"GONE", "1", zxconfig::ValueType::Int});
    const std::string_view declared[] = {"SMP"};

    const auto refused = zxconfig::reconcile(zxconfig::CacheState::Stale, document, declared, false);
    EXPECT_EQ(refused.disposition, zxconfig::CacheDisposition::Refuse);
    EXPECT_TRUE(refused.kept.empty());

    const auto forced = zxconfig::reconcile(zxconfig::CacheState::Stale, document, declared, true);
    EXPECT_EQ(forced.disposition, zxconfig::CacheDisposition::ApplyPartial);
    ASSERT_EQ(forced.kept.size(), 1u);
    EXPECT_EQ(forced.kept.front(), "SMP");
    ASSERT_EQ(forced.dropped.size(), 1u);
    EXPECT_EQ(forced.dropped.front(), "GONE");
}

TEST(CacheFormat, rejects_a_non_hex_digest) {
    auto parsed = zxconfig::parse_cache(
        "zxcache 1\n"
        "digest zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, zxconfig::ErrorCode::CacheCorrupt);
}

TEST(CacheLock, is_exclusive) {
    const auto dir = fs::temp_directory_path() / "zxconfig-lock-test";
    fs::create_directories(dir);
    const auto cache = dir / ".zxconfig.cache";

    auto first = zxconfig::CacheLock::acquire(cache, 0);
    ASSERT_TRUE(first.has_value()) << first.error().message;

    auto second = zxconfig::CacheLock::acquire(cache, 0);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, zxconfig::ErrorCode::LockBusy);

    first->release();
    auto third = zxconfig::CacheLock::acquire(cache, 0);
    EXPECT_TRUE(third.has_value()) << third.error().message;

    fs::remove_all(dir);
}

TEST(CacheStore, locked_persistence_refuses_a_held_lock_and_stores_when_free) {
    // One persistence core for every writer: it takes the lock, then the
    // atomic store happens under it.
    const auto dir = fs::temp_directory_path() / "zxconfig-store-locked-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto path = dir / ".zxconfig.cache";

    zxconfig::CacheDocument document;
    document.manifest = (dir / "ZXConfig").string();
    document.manifest_digest = zxconfig::sha256_hex("manifest");
    document.assignments.push_back({"SMP", "true", zxconfig::ValueType::Bool});

    auto held = zxconfig::CacheLock::acquire(path, 0);
    ASSERT_TRUE(held.has_value()) << held.error().message;
    auto busy = zxconfig::store_cache_locked(path, document, 0);
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(busy.error().code, zxconfig::ErrorCode::LockBusy);
    EXPECT_FALSE(fs::exists(path));

    held->release();
    auto stored = zxconfig::store_cache_locked(path, document, 0);
    ASSERT_TRUE(stored.has_value()) << stored.error().message;
    EXPECT_FALSE(fs::exists(path.string() + ".tmp"));
    auto loaded = zxconfig::load_cache(path, document.manifest_digest);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->find("SMP")->value, "true");

    fs::remove_all(dir);
}

TEST(Digest, changes_when_content_or_order_changes) {
    const std::string_view one[] = {"a.zx", "zxconfig 1.1\n"};
    const std::string_view two[] = {"a.zx", "zxconfig 1.0\n"};
    const std::string_view swapped[] = {"zxconfig 1.0\n", "a.zx"};

    const auto base = zxconfig::digest_manifest(one);
    EXPECT_EQ(base.size(), 64u);
    EXPECT_NE(base, zxconfig::digest_manifest(two));
    EXPECT_NE(base, zxconfig::digest_manifest(swapped));
}

TEST(CacheFormat, round_trips_namespaced_symbols) {
    // Regression: the reader's symbol grammar predates namespaces, so a save
    // through a namespaced include wrote a cache its own loader rejected.
    zxconfig::CacheDocument document;
    document.manifest_digest = std::string(64, 'a');
    document.assignments.push_back({"arch.KASLR", "true", zxconfig::ValueType::Bool});
    document.assignments.push_back({"net.DRV_X", "m", zxconfig::ValueType::Tristate});

    const auto rendered = zxconfig::format_cache(document);
    auto parsed = zxconfig::parse_cache(rendered);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_NE(parsed->find("arch.KASLR"), nullptr);
    EXPECT_NE(parsed->find("net.DRV_X"), nullptr);

    // Malformed names are still refused.
    auto empty_segment = zxconfig::parse_cache("zxcache 1\nset a..b bool \"true\"\n");
    ASSERT_FALSE(empty_segment.has_value());
    EXPECT_EQ(empty_segment.error().code, zxconfig::ErrorCode::CacheCorrupt);

    auto trailing_dot = zxconfig::parse_cache("zxcache 1\nset a. bool \"true\"\n");
    ASSERT_FALSE(trailing_dot.has_value());
    EXPECT_EQ(trailing_dot.error().code, zxconfig::ErrorCode::CacheCorrupt);
}

TEST(DeprecatedNames, rewrite_warn_and_keep_the_new_on_a_clash) {
    zxconfig::CacheDocument document;
    document.assignments.push_back({"SLAB_DEBUG", "true", zxconfig::ValueType::Bool});
    const zxconfig::Rename renames[] = {
        {"SLAB_DEBUG", "DEBUG_FEATURES", "SLAB_DEBUG was renamed in zconfig 1.1"}};

    auto reports = zxconfig::rename_assignments(document, renames);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0], "SLAB_DEBUG was renamed in zconfig 1.1");
    EXPECT_EQ(document.find("SLAB_DEBUG"), nullptr);
    ASSERT_NE(document.find("DEBUG_FEATURES"), nullptr);
    EXPECT_EQ(document.find("DEBUG_FEATURES")->value, "true");

    // Both spellings present: the new one wins and the old is dropped.
    zxconfig::CacheDocument clash;
    clash.assignments.push_back({"SLAB_DEBUG", "true", zxconfig::ValueType::Bool});
    clash.assignments.push_back({"DEBUG_FEATURES", "false", zxconfig::ValueType::Bool});
    auto clash_reports = zxconfig::rename_assignments(clash, renames);
    ASSERT_EQ(clash_reports.size(), 1u);
    EXPECT_EQ(clash_reports[0],
              "'SLAB_DEBUG' and 'DEBUG_FEATURES' are both present; keeping 'DEBUG_FEATURES'");
    ASSERT_EQ(clash.assignments.size(), 1u);
    EXPECT_EQ(clash.assignments[0].symbol, "DEBUG_FEATURES");
}
