// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/generator.hpp>
#include <zxconfig/parser.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

struct Harness {
    zxconfig::SourceArena arena;
    zxconfig::ParseResult parsed;
    zxconfig::Runtime runtime;

    explicit Harness(std::string_view text)
        : parsed([&] {
              auto id = arena.add("t.zx", std::string{text});
              EXPECT_TRUE(id.has_value());
              return zxconfig::parse_document(arena, *id);
          }()),
          runtime{parsed.document} {}

    [[nodiscard]] std::string header(std::string prefix = {}) const {
        const auto& directives = parsed.document.generates;
        EXPECT_FALSE(directives.empty());
        zxconfig::GenerateDirective directive = directives.front();
        if (!prefix.empty()) directive.prefix = std::move(prefix);
        auto rendered = zxconfig::render_header(runtime, directive, "test");
        EXPECT_TRUE(rendered.has_value())
            << (rendered ? "" : rendered.error().message);
        return rendered ? *rendered : "";
    }
};

bool has(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

}  // namespace

TEST(MacroName, maps_namespaces_and_case) {
    Harness harness{R"(zxconfig 1.0
option SMP : bool
)"};
    const auto* symbol = harness.parsed.document.symbols.at("SMP");
    EXPECT_EQ(zxconfig::macro_name(*symbol, "CONFIG_"), "CONFIG_SMP");
}

TEST(MacroName, maps_namespaced_symbols_like_linux) {
    Harness harness{R"(zxconfig 1.0
option VIRTIO_NET : bool
)"};
    const auto* symbol = harness.parsed.document.symbols.at("VIRTIO_NET");
    // net.VIRTIO_NET would be the namespaced spelling; the raw name alone
    // already exercises the same mapping.
    EXPECT_EQ(zxconfig::macro_name(*symbol, "CONFIG_"), "CONFIG_VIRTIO_NET");
    EXPECT_EQ(zxconfig::macro_name(*symbol, "CONFIG_", "m"), "CONFIG_VIRTIO_NET_M");
}

TEST(HeaderEmission, booleans_follow_autoconf_semantics) {
    Harness harness{R"(zxconfig 1.0
option ON : bool = true
option OFF : bool = false
generate header "config.h"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    const auto out = harness.header();

    EXPECT_TRUE(has(out, "#define CONFIG_ON 1"));
    EXPECT_TRUE(has(out, "/* CONFIG_OFF is not set */"));
    EXPECT_FALSE(has(out, "#define CONFIG_OFF"));
}

TEST(HeaderEmission, tristates_emit_module_suffixed_macros) {
    Harness harness{R"(zxconfig 1.0
option BUILTIN : tristate = y
option MODULE : tristate = m
option ABSENT : tristate = n
generate header "config.h"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    const auto out = harness.header();

    EXPECT_TRUE(has(out, "#define CONFIG_BUILTIN 1"));
    EXPECT_TRUE(has(out, "#define CONFIG_MODULE_MODULE 1"));
    EXPECT_FALSE(has(out, "#define CONFIG_MODULE 1"));
    EXPECT_TRUE(has(out, "/* CONFIG_ABSENT is not set */"));
}

TEST(HeaderEmission, scalars_strings_and_lists) {
    Harness harness{R"(zxconfig 1.0
option CPUS : int = 4
option BASE : hex = 0x1000
option NAME : string = "say \"hi\"\n"
option FLAGS : list<string> = ["a", "b"]
generate header "config.h"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    const auto out = harness.header();

    EXPECT_TRUE(has(out, "#define CONFIG_CPUS 4"));
    EXPECT_TRUE(has(out, "#define CONFIG_BASE 0x1000"));
    EXPECT_TRUE(has(out, "#define CONFIG_NAME \"say \\\"hi\\\"\\n\""));
    EXPECT_TRUE(has(out, "#define CONFIG_FLAGS \"a b\""));
}

TEST(HeaderEmission, enums_and_sets_emit_one_define_per_choice) {
    Harness harness{R"(zxconfig 1.0
option ARCH : enum = x86_64 {
    choices x86_64 | arm64
}
option DEBUG : set = {kasan} {
    choices kasan | lockdep
}
generate header "config.h"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    const auto out = harness.header();

    EXPECT_TRUE(has(out, "#define CONFIG_ARCH_X86_64 1"));
    EXPECT_TRUE(has(out, "#define CONFIG_ARCH_ARM64 0"));
    EXPECT_TRUE(has(out, "#define CONFIG_DEBUG_KASAN 1"));
    EXPECT_TRUE(has(out, "#define CONFIG_DEBUG_LOCKDEP 0"));
}

TEST(HeaderEmission, honours_a_custom_prefix) {
    Harness harness{R"(zxconfig 1.0
option SMP : bool = true
generate header "config.h" { prefix "ZX_" }
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    const auto out = harness.header();
    EXPECT_TRUE(has(out, "#define ZX_SMP 1"));
    EXPECT_FALSE(has(out, "CONFIG_"));
}

TEST(HeaderEmission, refuses_name_collisions) {
    // A choice macro and a flat symbol map to the same name: the choice
    // `A_B` of X becomes CONFIG_X_A_B, and so does a symbol literally named
    // `X_A_B`.  Generation must refuse rather than emit two definitions.
    Harness harness{R"(zxconfig 1.0
option X : enum = A { choices A | A_B }
option X_A_B : bool
generate header "config.h"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());

    const auto& directive = harness.parsed.document.generates.front();
    auto rendered = zxconfig::render_header(harness.runtime, directive, "test");
    ASSERT_FALSE(rendered.has_value());
    EXPECT_EQ(rendered.error().code, zxconfig::ErrorCode::NamespaceCollision);
}

TEST(HeaderEmission, writes_files_deterministically) {
    const auto dir = std::filesystem::temp_directory_path() / "zxconfig-gen-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    Harness harness{R"(zxconfig 1.0
option B : bool = true
option A : int = 1
generate header "config.h"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());

    auto directive = harness.parsed.document.generates.front();
    directive.output = (dir / "config.h").string();

    ASSERT_TRUE(zxconfig::generate_header(
        harness.runtime, directive, "test").has_value());

    std::ifstream first{directive.output};
    std::string first_text{std::istreambuf_iterator<char>{first}, {}};
    ASSERT_TRUE(zxconfig::generate_header(
        harness.runtime, directive, "test").has_value());
    std::ifstream second{directive.output};
    std::string second_text{std::istreambuf_iterator<char>{second}, {}};
    EXPECT_EQ(first_text, second_text);

    // Sorted emission: A before B regardless of declaration order.
    EXPECT_LT(first_text.find("CONFIG_A"), first_text.find("CONFIG_B"));

    // A relative output resolves against the base, not the working directory.
    directive.output = "relative.h";
    ASSERT_TRUE(zxconfig::generate_header(
        harness.runtime, directive, "test", dir.string()).has_value());
    EXPECT_TRUE(std::filesystem::exists(dir / "relative.h"));
    EXPECT_FALSE(std::filesystem::exists("relative.h"));

    std::filesystem::remove_all(dir);
}

TEST(OtherBackends, makefile_uses_kconfig_spellings) {
    Harness harness{R"(zxconfig 1.0
option ON : bool = true
option OFF : bool = false
option MOD : tristate = m
option CPUS : int = 4
option NAME : string = "a#b$c"
option FLAGS : list<string> = ["x", "y"]
option ARCH : enum = x86_64 { choices x86_64 | arm64 }
generate makefile "config.mk"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    auto rendered = zxconfig::render_makefile(harness.runtime, "test");
    ASSERT_TRUE(rendered.has_value());

    EXPECT_TRUE(has(*rendered, "CONFIG_ON=y"));
    EXPECT_TRUE(has(*rendered, "# CONFIG_OFF is not set"));
    EXPECT_TRUE(has(*rendered, "CONFIG_MOD=m"));
    EXPECT_TRUE(has(*rendered, "CONFIG_CPUS=4"));
    EXPECT_TRUE(has(*rendered, "CONFIG_NAME=a\\#b$$c"));
    EXPECT_TRUE(has(*rendered, "CONFIG_FLAGS=x y"));
    EXPECT_TRUE(has(*rendered, "CONFIG_ARCH=x86_64"));
}

TEST(OtherBackends, json_uses_native_values) {
    Harness harness{R"(zxconfig 1.0
option ON : bool = true
option MOD : tristate = m
option CPUS : int = 4
option NAME : string = "hi \"there\""
option FLAGS : list<string> = ["x", "y"]
option EMPTY : int
generate json "config.json"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    auto rendered = zxconfig::render_json(harness.runtime, "test");
    ASSERT_TRUE(rendered.has_value());

    EXPECT_TRUE(has(*rendered, "\"format\": 1"));
    EXPECT_TRUE(has(*rendered, "\"ON\": {\"type\": \"bool\", \"value\": true}"));
    EXPECT_TRUE(has(*rendered, "\"MOD\": {\"type\": \"tristate\", \"value\": \"m\"}"));
    EXPECT_TRUE(has(*rendered, "\"CPUS\": {\"type\": \"int\", \"value\": 4}"));
    EXPECT_TRUE(has(*rendered, "\"NAME\": {\"type\": \"string\", \"value\": \"hi \\\"there\\\"\"}"));
    EXPECT_TRUE(has(*rendered, "\"FLAGS\": {\"type\": \"list\", \"value\": [\"x\", \"y\"]}"));
    EXPECT_TRUE(has(*rendered, "\"EMPTY\": {\"type\": \"int\", \"value\": null}"));
}

TEST(OtherBackends, cmake_uses_on_off) {
    Harness harness{R"(zxconfig 1.0
option ON : bool = true
option OFF : bool = false
option MOD : tristate = m
option NAME : string = "say \"hi\""
generate cmake "config.cmake"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    auto rendered = zxconfig::render_cmake(harness.runtime, "test");
    ASSERT_TRUE(rendered.has_value());

    EXPECT_TRUE(has(*rendered, "set(CONFIG_ON ON)"));
    EXPECT_TRUE(has(*rendered, "set(CONFIG_OFF OFF)"));
    EXPECT_TRUE(has(*rendered, "set(CONFIG_MOD \"m\")"));
    EXPECT_TRUE(has(*rendered, "set(CONFIG_NAME \"say \\\"hi\\\"\")"));
}

TEST(OtherBackends, templates_interpolate_symbols) {
    Harness harness{R"(zxconfig 1.0
option BASE : hex = 0x1000
option TAG : string = "v1"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());

    auto ok = zxconfig::render_template(harness.runtime,
                                        "base = ${BASE}; /* $$ not a ref */ tag ${TAG}\n");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, "base = 0x1000; /* $ not a ref */ tag v1\n");

    auto unknown = zxconfig::render_template(harness.runtime, "${NOPE}");
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code, zxconfig::ErrorCode::Interpolation);

    auto unclosed = zxconfig::render_template(harness.runtime, "${BASE");
    ASSERT_FALSE(unclosed.has_value());
    EXPECT_EQ(unclosed.error().code, zxconfig::ErrorCode::Interpolation);
}

TEST(OtherBackends, run_generators_drives_every_backend) {
    const auto dir = std::filesystem::temp_directory_path() / "zxconfig-gen-all-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream{dir / "linker.tpl"} << "SECTIONS { . = ${BASE}; }\n";

    Harness harness{R"(zxconfig 1.0
option BASE : hex = 0x8000
generate header "out/config.h"
generate makefile "out/config.mk"
generate json "out/config.json"
generate cmake "out/config.cmake"
generate template "out/config.ld" from "linker.tpl"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());

    std::vector<std::string> failures;
    const auto ok = zxconfig::run_generators(
        harness.parsed.document, harness.runtime, "test", dir.string(), failures);
    EXPECT_EQ(ok, 5u) << (failures.empty() ? "" : failures.front());
    EXPECT_TRUE(std::filesystem::exists(dir / "out" / "config.h"));
    EXPECT_TRUE(std::filesystem::exists(dir / "out" / "config.mk"));
    EXPECT_TRUE(std::filesystem::exists(dir / "out" / "config.json"));
    EXPECT_TRUE(std::filesystem::exists(dir / "out" / "config.cmake"));
    EXPECT_TRUE(std::filesystem::exists(dir / "out" / "config.ld"));

    std::filesystem::remove_all(dir);
}
