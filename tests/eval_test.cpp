// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/cache.hpp>
#include <zxconfig/eval.hpp>
#include <zxconfig/parser.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/utsname.h>

namespace {

namespace fs = std::filesystem;

/// @brief Owns everything a Runtime points into.  A Runtime borrows its
///        document, so the harness must outlive it in the test body.
struct Harness {
    zxconfig::SourceArena arena;
    zxconfig::ParseResult parsed;
    zxconfig::Runtime runtime;

    explicit Harness(std::string_view text,
                     std::vector<zxconfig::Diagnostic>* notes = nullptr)
        : parsed(parse(text)),
          runtime{parsed.document, notes} {}

private:
    zxconfig::ParseResult parse(std::string_view text) {
        auto id = arena.add("t.zx", std::string{text});
        EXPECT_TRUE(id.has_value());
        return zxconfig::parse_document(arena, *id);
    }
};

}  // namespace

TEST(EvalValue, renders_by_declared_type) {
    using zxconfig::render_value;
    EXPECT_EQ(render_value(true, zxconfig::ValueType::Bool), "true");
    EXPECT_EQ(render_value(std::int64_t{2}, zxconfig::ValueType::Tristate), "y");
    EXPECT_EQ(render_value(std::int64_t{255}, zxconfig::ValueType::Hex), "0xFF");
    EXPECT_EQ(render_value(std::string{"abc"}, zxconfig::ValueType::String), "abc");
    EXPECT_EQ(render_value(std::monostate{}, zxconfig::ValueType::Int), "<unset>");
}

TEST(Coerce, accepts_and_rejects_by_type) {
    std::vector<std::string> choices{"none", "full"};

    auto boolean = zxconfig::coerce("true", zxconfig::ValueType::Bool, choices);
    ASSERT_TRUE(boolean.has_value());
    // A boolean that is on contributes y in tristate expressions.
    EXPECT_EQ(zxconfig::truth_level(*boolean), 2);

    auto bad_bool = zxconfig::coerce("maybe", zxconfig::ValueType::Bool, choices);
    EXPECT_EQ(bad_bool.error().code, zxconfig::ErrorCode::TypeMismatch);

    auto tri = zxconfig::coerce("m", zxconfig::ValueType::Tristate, choices);
    ASSERT_TRUE(tri.has_value());
    EXPECT_EQ(std::get<std::int64_t>(*tri), 1);

    auto hex = zxconfig::coerce("0x1F", zxconfig::ValueType::Hex, choices);
    ASSERT_TRUE(hex.has_value());
    EXPECT_EQ(std::get<std::int64_t>(*hex), 31);

    auto choice = zxconfig::coerce("full", zxconfig::ValueType::Enum, choices);
    EXPECT_TRUE(choice.has_value());
    auto bad_choice = zxconfig::coerce("voluntary", zxconfig::ValueType::Enum, choices);
    EXPECT_EQ(bad_choice.error().code, zxconfig::ErrorCode::UnknownChoice);

    auto set = zxconfig::coerce("none, full", zxconfig::ValueType::Set, choices);
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(std::get<std::vector<std::string>>(*set).size(), 2u);
}

TEST(TruthLevel, follows_tristate_rules) {
    EXPECT_EQ(zxconfig::truth_level(std::monostate{}), 0);
    EXPECT_EQ(zxconfig::truth_level(false), 0);
    EXPECT_EQ(zxconfig::truth_level(true), 2);
    EXPECT_EQ(zxconfig::truth_level(std::int64_t{2}), 2);
    EXPECT_EQ(zxconfig::truth_level(std::int64_t{1}), 1);
    EXPECT_EQ(zxconfig::truth_level(std::string{""}), 0);
    EXPECT_EQ(zxconfig::truth_level(std::string{"x"}), 1);
}

TEST(TruthLevel, nonzero_integers_are_true) {
    // Any nonzero integer is true; negatives count as fully on, matching
    // "nonzero is true" without touching the n/m/y tristate levels.
    EXPECT_EQ(zxconfig::truth_level(std::int64_t{-1}), 2);
    EXPECT_EQ(zxconfig::truth_level(std::int64_t{-4096}), 2);

    Harness harness{R"(zxconfig 1.1
computed Pick : int = -1 ? 10 : 20
computed Shift : int = !-7 ? 1 : 0
)"};
    ASSERT_TRUE(harness.parsed.ok());
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::int64_t>(*harness.runtime.value("Pick")), 10);
    EXPECT_EQ(std::get<std::int64_t>(*harness.runtime.value("Shift")), 0);
}

TEST(Evaluator, resolves_defaults_and_visibility) {
    Harness runtime_harness{R"(zxconfig 1.1
option SMP : bool = false { label "smp" }
option NR_CPUS : int = 4 {
    range 1 .. 512
    when SMP
}
menu "Memory" visible SMP {
    option NUMA : bool = false
}
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());

    EXPECT_TRUE(runtime.is_visible("SMP"));
    // NR_CPUS is hidden because SMP is false.
    EXPECT_FALSE(runtime.is_visible("NR_CPUS"));
    // The menu is hidden, so everything under it is hidden too.
    EXPECT_FALSE(runtime.is_visible("NUMA"));

    const auto* cpus = runtime.value("NR_CPUS");
    ASSERT_NE(cpus, nullptr);
    EXPECT_EQ(std::get<std::int64_t>(*cpus), 4);
}

TEST(Evaluator, conditional_defaults_pick_the_first_true_case) {
    Harness runtime_harness{R"(zxconfig 1.1
option MODEL : enum = voluntary {
    choices none | voluntary | full
}
option HZ : int = 100 {
    default {
        1000 when MODEL == full
        250  when MODEL == voluntary
    }
}
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    const auto* hz = runtime.value("HZ");
    ASSERT_NE(hz, nullptr);
    EXPECT_EQ(std::get<std::int64_t>(*hz), 250);
}

TEST(Evaluator, computed_symbols_follow_dependencies) {
    Harness runtime_harness{R"(zxconfig 1.1
option BASE : hex = 0x100
computed SIZE : hex = BASE + 0xFF
computed PAGES : int = SIZE / 4096
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("SIZE")), 0x1FF);
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("PAGES")), 0);
}

TEST(Evaluator, arithmetic_and_logic_semantics) {
    auto notes = std::vector<zxconfig::Diagnostic>{};
    Harness rt_harness{R"(zxconfig 1.1
option A : bool = true
option B : tristate = m
computed OR : tristate = A or B
computed AND : tristate = A and B
computed NOT : tristate = not B
)", &notes};
    auto& rt = rt_harness.runtime;
    ASSERT_TRUE(rt.evaluate().has_value());
    // y or m = y, y and m = m, not m = m under kconfig rules.
    EXPECT_EQ(std::get<std::int64_t>(*rt.value("OR")), 2);
    EXPECT_EQ(std::get<std::int64_t>(*rt.value("AND")), 1);
    EXPECT_EQ(std::get<std::int64_t>(*rt.value("NOT")), 1);
}

TEST(Evaluator, detects_value_cycles) {
    Harness runtime_harness{R"(zxconfig 1.1
option A : bool = B
option B : bool = A
)"};
    auto& runtime = runtime_harness.runtime;
    auto failed = runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::CircularDependency);
}

TEST(Evaluator, division_by_zero_fails_the_expression) {
    Harness runtime_harness{R"(zxconfig 1.1
option Z : int = 0
computed X : int = 5 / Z
)"};
    auto& runtime = runtime_harness.runtime;
    auto failed = runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::DivisionByZero);
}

TEST(Evaluator, unknown_symbols_are_reported) {
    Harness runtime_harness{R"(zxconfig 1.1
computed X : bool = NOTHING
)"};
    auto& runtime = runtime_harness.runtime;
    auto failed = runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::UnknownSymbol);
}

TEST(Evaluator, env_and_shell_probes) {
    ::setenv("ZX_TEST_VAR", "hello", 1);
    Harness runtime_harness{R"(zxconfig 1.1
option E : string = env("ZX_TEST_VAR")
option S : string = shell("echo world")
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*runtime.value("E")), "hello");
    EXPECT_EQ(std::get<std::string>(*runtime.value("S")), "world");
}

TEST(Evaluator, failed_probe_uses_fallback) {
    Harness runtime_harness{R"(zxconfig 1.1
option S : string = shell("exit 1") fallback "backup"
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*runtime.value("S")), "backup");
}

TEST(Evaluator, user_values_are_validated) {
    Harness runtime_harness{R"(zxconfig 1.1
option N : int = 4 { range 1 .. 512 }
option E : enum = none { choices none | full }
option P : path = "/tmp"
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());

    auto out_of_range = runtime.set_user_value("N", "9999");
    EXPECT_EQ(out_of_range.error().code, zxconfig::ErrorCode::RangeViolated);

    auto bad_choice = runtime.set_user_value("E", "voluntary");
    EXPECT_EQ(bad_choice.error().code, zxconfig::ErrorCode::UnknownChoice);

    auto missing_path = runtime.set_user_value("P", "/no/such/path", zxconfig::ValueType::Path);
    // No path constraints declared, so any text is accepted.
    EXPECT_TRUE(missing_path.has_value());

    auto ok = runtime.set_user_value("N", "8");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("N")), 8);
    EXPECT_TRUE(runtime.is_user_set("N"));

    auto back = runtime.reset_symbol("N");
    ASSERT_TRUE(back.has_value());
    EXPECT_FALSE(runtime.is_user_set("N"));
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("N")), 4);
}

TEST(Evaluator, visibility_updates_after_a_write) {
    Harness runtime_harness{R"(zxconfig 1.1
option SMP : bool = false
option NR_CPUS : int = 4 { when SMP }
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_FALSE(runtime.is_visible("NR_CPUS"));

    ASSERT_TRUE(runtime.set_user_value("SMP", "true").has_value());
    EXPECT_TRUE(runtime.is_visible("NR_CPUS"));

    ASSERT_TRUE(runtime.set_user_value("SMP", "false").has_value());
    EXPECT_FALSE(runtime.is_visible("NR_CPUS"));
}

TEST(Evaluator, validation_reports_violations_at_their_severity) {
    Harness runtime_harness{R"(zxconfig 1.1
option RAM : int = 128
error RAM < 256 : "RAM must be at least 256"
warn RAM < 256 : "small RAM"
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    auto findings = runtime.run_validation();
    ASSERT_EQ(findings.size(), 2u);
    EXPECT_EQ(findings[0].severity, zxconfig::Severity::Error);
    EXPECT_EQ(findings[1].severity, zxconfig::Severity::Warning);
    // Both severities trigger when their condition holds.
}

TEST(Evaluator, to_cache_round_trips_through_apply) {
    Harness runtime_harness{R"(zxconfig 1.1
option SMP : bool = false
option N : int = 4
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    ASSERT_TRUE(runtime.set_user_value("SMP", "true").has_value());
    ASSERT_TRUE(runtime.set_user_value("N", "64").has_value());

    auto document = runtime.to_cache("manifest", std::string(64, 'a'));
    ASSERT_EQ(document.assignments.size(), 2u);

    Harness fresh_harness{R"(zxconfig 1.1
option SMP : bool = false
option N : int = 4
)"};
    auto& fresh = fresh_harness.runtime;
    for (const auto& assignment : document.assignments) {
        ASSERT_TRUE(fresh.apply_assignment(assignment.symbol, assignment.value,
                                           assignment.type)
                        .has_value());
    }
    // A boolean assignment reads back as y under tristate semantics.
    EXPECT_EQ(zxconfig::truth_level(*fresh.value("SMP")), 2);
    EXPECT_EQ(std::get<std::int64_t>(*fresh.value("N")), 64);
}

TEST(Evaluator, metadata_warnings_fire_when_the_condition_holds) {
    // The spec's `warn CLANG == "" : "clang not found"` warns when the
    // condition holds, as does the top-level form.
    Harness harness{R"(zxconfig 1.1
option TAG : string = "set"
option EMPTY : string = "" {
    warn EMPTY == "" : "empty is empty"
}
option FULL : string = "x" {
    warn FULL == "" : "full is empty"
}
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    auto findings = harness.runtime.run_validation();
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].message, "empty is empty");
    EXPECT_EQ(findings[0].severity, zxconfig::Severity::Warning);
}

TEST(Implications, offer_fixes_and_use_the_conflict_message) {
    Harness harness{R"(zxconfig 1.1
option FORCE : bool = true {
    implies MODEL == full
    conflict MODEL { resolve "override MODEL?" }
}
option MODEL : enum = voluntary { choices none | voluntary | full }
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    auto open = harness.runtime.unsatisfied_implications();
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].source, "FORCE");
    EXPECT_EQ(open[0].message, "override MODEL?");
    EXPECT_FALSE(open[0].blocks_save);
    ASSERT_EQ(open[0].fixes.size(), 1u);
    EXPECT_EQ(open[0].fixes[0].symbol, "MODEL");
    EXPECT_EQ(open[0].fixes[0].value, "full");

    ASSERT_TRUE(harness.runtime.set_user_value("MODEL", "full").has_value());
    EXPECT_TRUE(harness.runtime.unsatisfied_implications().empty());
}

TEST(Implications, or_form_offers_alternatives) {
    Harness harness{R"(zxconfig 1.1
option NET : bool = true {
    implies DRV_A or DRV_B
}
option DRV_A : bool = false
option DRV_B : bool = false
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    auto open = harness.runtime.unsatisfied_implications();
    ASSERT_EQ(open.size(), 1u);
    ASSERT_EQ(open[0].fixes.size(), 2u);
    EXPECT_EQ(open[0].fixes[0].symbol, "DRV_A");
    EXPECT_EQ(open[0].fixes[1].symbol, "DRV_B");

    // Either fix closes it.
    ASSERT_TRUE(harness.runtime.set_user_value("DRV_B", "true").has_value());
    EXPECT_TRUE(harness.runtime.unsatisfied_implications().empty());
}

TEST(Implications, error_policy_blocks_and_a_dormant_source_is_silent) {
    Harness harness{R"(zxconfig 1.1
option FORCE : bool = true {
    implies HARD
    conflict HARD { error "hard failure" }
}
option HARD : bool = false
option IDLE : bool = false {
    implies HARD
    conflict HARD { error "never fires" }
}
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    auto open = harness.runtime.unsatisfied_implications();
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].source, "FORCE");
    EXPECT_TRUE(open[0].blocks_save);
}

TEST(Aliases, resolve_in_expressions_and_on_write) {
    Harness harness{R"(zxconfig 1.1
option TARGET : bool = false
alias SHORT = TARGET
computed USES : bool = SHORT
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("USES")), 0);

    // A write through the alias lands on the target's slot: accessors resolve
    // the alias, and the cache documents the canonical name.
    ASSERT_TRUE(harness.runtime.set_user_value("SHORT", "true").has_value());
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("TARGET")), 2);
    EXPECT_TRUE(harness.runtime.is_user_set("TARGET"));
    EXPECT_TRUE(harness.runtime.is_user_set("SHORT"));
    const auto document = harness.runtime.to_cache("m", std::string(64, 'a'));
    ASSERT_EQ(document.assignments.size(), 1u);
    EXPECT_EQ(document.assignments[0].symbol, "TARGET");
}

TEST(Evaluator, ordinary_user_edits_do_not_report_cache_type_mismatches) {
    std::vector<zxconfig::Diagnostic> diagnostics;
    Harness harness{R"(zxconfig 1.1
option A : bool = false
)", &diagnostics};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    ASSERT_TRUE(harness.runtime.set_user_value("A", "true").has_value());
    EXPECT_TRUE(diagnostics.empty());
}

TEST(Aliases, chains_resolve_and_cycles_stop) {
    Harness harness{R"(zxconfig 1.1
option REAL : bool = true
alias MID = REAL
alias FAR = MID
computed USES : bool = FAR
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("USES")), 2);

    Harness loop{R"(zxconfig 1.1
alias A = B
alias B = A
computed USES : bool = A
)"};
    auto failed = loop.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::UnknownSymbol);
}

TEST(Interpolation, expands_symbols_in_string_values) {
    Harness harness{R"(zxconfig 1.1
option PREFIX : string = "/usr/local"
option ARCH : enum = x86_64 { choices x86_64 | arm64 }
computed BIN_DIR : string = "${PREFIX}/bin"
computed LIB_DIR : string = "${PREFIX}/lib/${ARCH}"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("BIN_DIR")), "/usr/local/bin");
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("LIB_DIR")),
              "/usr/local/lib/x86_64");
}

TEST(Interpolation, aliases_resolve_through_the_chain) {
    Harness harness{R"(zxconfig 1.1
option REAL : string = "/opt"
alias SHORT = REAL
computed USES : string = "${SHORT}/bin"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("USES")), "/opt/bin");
}

TEST(Interpolation, unknown_names_and_unset_targets_fail) {
    Harness unknown{R"(zxconfig 1.1
computed X : string = "${NOPE}/bin"
)"};
    auto failed = unknown.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::Interpolation);

    Harness unclosed{R"(zxconfig 1.1
computed X : string = "${NOPE"
)"};
    auto bad = unclosed.runtime.evaluate();
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, zxconfig::ErrorCode::Interpolation);

    Harness unset{R"(zxconfig 1.1
option U : string
computed X : string = "${U}/bin"
)"};
    auto missing = unset.runtime.evaluate();
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, zxconfig::ErrorCode::UndefinedValue);
}

TEST(Interpolation, macro_leftovers_resolve_at_evaluation) {
    // A macro body's ${param} is substituted at expansion; what is left
    // names symbols and resolves when values are computed.
    Harness harness{R"(zxconfig 1.1
option PREFIX : string = "/usr"
macro M(x) {
    option D_${x} : string = "${PREFIX}/${x}"
}
M(bin)
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("D_bin")), "/usr/bin");
}

TEST(Interpolation, list_items_interpolate_too) {
    Harness harness{R"(zxconfig 1.1
option PREFIX : string = "/usr"
computed PATHS : list<string> = ["${PREFIX}/bin", "${PREFIX}/lib"]
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    const auto& items = std::get<std::vector<std::string>>(*harness.runtime.value("PATHS"));
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0], "/usr/bin");
    EXPECT_EQ(items[1], "/usr/lib");
}

TEST(Expressions, collections_work_in_all_condition_fields) {
    Harness harness{R"(zxconfig 1.1
option Arch : enum = s390x { choices s390x | arm64 }
option Flags : set = {kasan, ubsan} { choices kasan | ubsan }
option Ordered : list<string> = ["first", "second"]
option Visible : bool = true {
    visible Arch == s390x and Flags == {ubsan, kasan}
    enabled contains(Flags, kasan)
    warn Flags == {} : "flags must be nonempty"
}
option Legacy : bool = true { when Flags == {kasan, ubsan, kasan} }
menu "Predicates" visible Flags == {ubsan, kasan} {
    option Child : bool = true
}
computed IsSet : bool = type_of(Flags) == "set"
computed IsList : bool = type_of(Ordered) == "list"
computed OrderedEqual : bool = Ordered == ["second", "first"]
computed Chosen : int = Flags == {ubsan, kasan} ? 3 : 4
computed ConditionalSet : bool = Flags == (true ? {ubsan, kasan} : {})
computed References : list<string> = ["${Arch}", ${Arch}]
computed Guarded : bool = false and (1 / 0 == 1)
error Flags != {ubsan, kasan} : "set equality"
warn ${Arch} != s390x : "scalar enum equality"
)"};
    ASSERT_TRUE(harness.parsed.ok());
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_TRUE(harness.runtime.is_visible("Visible"));
    EXPECT_TRUE(harness.runtime.is_visible("Legacy"));
    EXPECT_TRUE(harness.runtime.is_visible("Child"));
    EXPECT_TRUE(harness.runtime.is_enabled("Visible"));
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("IsSet")), 2);
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("IsList")), 2);
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("OrderedEqual")), 0);
    EXPECT_EQ(std::get<std::int64_t>(*harness.runtime.value("Chosen")), 3);
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("ConditionalSet")), 2);
    EXPECT_EQ(std::get<std::vector<std::string>>(*harness.runtime.value("References")),
              (std::vector<std::string>{"s390x", "s390x"}));
    EXPECT_TRUE(harness.runtime.run_validation().empty());
    ASSERT_TRUE(harness.runtime.set_user_value("Flags", "kasan").has_value());
    EXPECT_FALSE(harness.runtime.is_visible("Visible"));
    EXPECT_FALSE(harness.runtime.is_visible("Child"));
    EXPECT_EQ(harness.runtime.run_validation().size(), 1u);
}

TEST(Namespaces, defining_file_controls_scope_and_include_conditions_update) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "zxconfig-file-namespace-eval";
    fs::create_directories(dir);
    std::ofstream{dir / "root.zx"} << R"(zxconfig 1.1
option Arch : enum = s390x { choices s390x | arm64 }
option Value : int = 7
include "arch.zx" visible Arch == s390x
computed GlobalRead : int = is_defined(arch.Copy) ? arch.Copy : 0
)";
    std::ofstream{dir / "arch.zx"} << R"(zxconfig 1.1
namespace arch
option Arch : enum = arm64 { choices s390x | arm64 }
option Value : int = 9
computed Copy : int = Value
computed Text : string = "${Value}"
computed Defined : bool = is_defined(Value)
alias Short = Value
menu "Local menu" visible Value == 9 { option Inside : bool = true }
error Value != 9 : "local scope"
)";
    zxconfig::SourceArena arena;
    auto id = arena.load((dir / "root.zx").string(), {}, dir.string());
    ASSERT_TRUE(id.has_value());
    auto parsed = zxconfig::parse_document(arena, *id, dir.string());
    ASSERT_TRUE(parsed.ok());
    zxconfig::Runtime runtime{parsed.document};
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("arch.Copy")), 9);
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("GlobalRead")), 9);
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("arch.Short")), 9);
    EXPECT_EQ(std::get<std::string>(*runtime.value("arch.Text")), "9");
    EXPECT_EQ(zxconfig::truth_level(*runtime.value("arch.Defined")), 2);
    EXPECT_TRUE(runtime.run_validation().empty());
    EXPECT_TRUE(runtime.is_visible("arch.Inside"));
    ASSERT_TRUE(runtime.set_user_value("Arch", "arm64").has_value());
    EXPECT_FALSE(runtime.is_visible("arch.Inside"));
    EXPECT_FALSE(runtime.is_visible("arch.Value"));
    // The include is deactivated, so the guarded read falls back and the
    // file's exports are inert.
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("GlobalRead")), 0);
    const auto* dormant_copy = runtime.symbol("arch.Copy");
    ASSERT_NE(dormant_copy, nullptr);
    EXPECT_FALSE(runtime.is_symbol_active(*dormant_copy));
    // Writes into the deactivated file are refused as undeclared.
    auto dormant_write = runtime.set_user_value("arch.Value", "3");
    ASSERT_FALSE(dormant_write.has_value());
    ASSERT_TRUE(runtime.set_user_value("Arch", "s390x").has_value());
    EXPECT_TRUE(runtime.is_visible("arch.Inside"));
    EXPECT_TRUE(runtime.is_visible("arch.Value"));
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("GlobalRead")), 9);
    fs::remove_all(dir);
}

TEST(HostBuiltins, values_match_the_running_host_and_expand_in_probes) {
    struct utsname info{};
    ASSERT_EQ(::uname(&info), 0);
    Harness harness{R"(zxconfig 1.1
computed Os : string = HOST_OS
computed Cpu : string = HOST_ARCH
computed Release : string = "${HOST_KERNEL}"
computed Node : string = HOST_NODE
computed Query : string = shell("printf '%s' '${HOST_OS}'")
computed Present : bool = is_defined(HOST_OS)
)"};
    ASSERT_TRUE(harness.parsed.ok());
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("Os")), info.sysname);
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("Cpu")), info.machine);
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("Release")), info.release);
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("Node")), info.nodename);
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("Query")), info.sysname);
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("Present")), 2);
}

TEST(Expressions, rejects_set_list_equality_and_collection_ordering) {
    Harness mixed{R"(zxconfig 1.1
computed Mixed : bool = {a} == ["a"]
)"};
    ASSERT_TRUE(mixed.parsed.ok());
    auto failed = mixed.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::TypeMismatch);
    Harness ordered{R"(zxconfig 1.1
computed Ordered : bool = {a} < {b}
)"};
    failed = ordered.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::TypeMismatch);
}

TEST(HiddenChoices, a_selected_hidden_member_fails_evaluation) {
    Harness harness{R"(zxconfig 1.1
option Advanced : bool = false
option Strategy : enum = tuned {
    choices fast | tuned
    choice tuned { label "Tuned"; visible Advanced }
}
computed Hint : bool = Strategy == tuned
)"};
    ASSERT_TRUE(harness.parsed.ok());
    auto failed = harness.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::UnknownChoice);
    EXPECT_NE(failed.error().message.find("'tuned' is hidden for 'Strategy'"),
              std::string::npos);

    // With the dependency satisfied the default is valid again.
    Harness visible{R"(zxconfig 1.1
option Advanced : bool = true
option Strategy : enum = tuned {
    choices fast | tuned
    choice tuned { label "Tuned"; visible Advanced }
}
)"};
    ASSERT_TRUE(visible.runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*visible.runtime.value("Strategy")), "tuned");
}

TEST(HiddenChoices, a_write_that_hides_the_current_selection_is_rejected) {
    Harness harness{R"(zxconfig 1.1
option Expert : bool = true
option Strategy : enum = fast {
    choices fast | tuned
    choice tuned { label "Tuned"; visible Expert }
}
)"};
    auto& runtime = harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    ASSERT_TRUE(runtime.set_user_value("Strategy", "tuned").has_value());

    // Turning Expert off hides the selected member, so the dependent write
    // must fail and leave the published state untouched.
    auto refused = runtime.set_user_value("Expert", "false");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, zxconfig::ErrorCode::UnknownChoice);
    EXPECT_EQ(zxconfig::truth_level(*runtime.value("Expert")), 2);
    EXPECT_EQ(std::get<std::string>(*runtime.value("Strategy")), "tuned");

    // Correcting the selection unblocks the original write.
    ASSERT_TRUE(runtime.set_user_value("Strategy", "fast").has_value());
    ASSERT_TRUE(runtime.set_user_value("Expert", "false").has_value());
}

TEST(HiddenChoices, a_selected_hidden_set_member_fails_evaluation) {
    Harness harness{R"(zxconfig 1.1
option Big : bool = false
option Tunables : set = {kasan} {
    choices kasan | ubsan
    choice kasan { label "Kasan"; visible Big }
}
)"};
    ASSERT_TRUE(harness.parsed.ok());
    auto failed = harness.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::UnknownChoice);
    EXPECT_NE(failed.error().message.find("'kasan' is hidden for 'Tunables'"),
              std::string::npos);
}

TEST(Expressions, bare_choice_names_never_resolve_through_unrelated_declarations) {    Harness harness{R"(zxconfig 1.1
option Arch : enum = s390x { choices s390x | arm64 }
option Other : bool = true
computed Wrong : bool = Other && arm64
)"};
    ASSERT_TRUE(harness.parsed.ok());
    auto failed = harness.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::UnknownSymbol);

    // The same bare name is a typed choice next to a direct enum reference.
    Harness typed{R"(zxconfig 1.1
option Arch : enum = arm64 { choices s390x | arm64 }
computed Right : bool = Arch == arm64
computed Cross : bool = Arch != s390x
)"};
    ASSERT_TRUE(typed.parsed.ok());
    ASSERT_TRUE(typed.runtime.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*typed.runtime.value("Right")), 2);
    EXPECT_EQ(zxconfig::truth_level(*typed.runtime.value("Cross")), 2);
}

/// @brief Temporary directory harness for manifests that really include
///        files; the directory is removed with the harness.
struct IncludeHarness {
    fs::path dir;
    zxconfig::SourceArena arena;

    explicit IncludeHarness(const std::string& kind) {
        const auto name = "zxconfig-" + kind;
        std::error_code ec;
        fs::remove_all(fs::temp_directory_path() / name, ec);
        dir = fs::temp_directory_path() / name;
        fs::create_directories(dir);
    }
    IncludeHarness(const IncludeHarness&) = delete;
    IncludeHarness& operator=(const IncludeHarness&) = delete;
    ~IncludeHarness() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void file(const std::string& name, std::string_view text) {
        std::ofstream{dir / name} << text;
    }
    [[nodiscard]] zxconfig::ParseResult parse() {
        auto id = arena.load((dir / "root.zx").string(), {}, dir.string());
        EXPECT_TRUE(id.has_value());
        return zxconfig::parse_document(arena, *id, dir.string());
    }
};

TEST(Includes, deactivated_file_is_not_referenceable) {
    // A symbol in an inactive include is not declared: unguarded references
    // fail, guarded probes report absent, and writes to it are refused.
    IncludeHarness harness{"include-deactivated"};
    harness.file("root.zx", R"(zxconfig 1.1
option Gate : bool = false
include "extra.zx" visible Gate
computed Guarded : bool = is_defined(extra.Value)
)");
    harness.file("extra.zx",
                 "zxconfig 1.1\nnamespace extra\noption Value : bool = true\n");
    auto parsed = harness.parse();
    ASSERT_TRUE(parsed.ok()) << (parsed.diagnostics.empty() ? "" :
                                parsed.diagnostics.front().message);
    zxconfig::Runtime runtime{parsed.document};
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*runtime.value("Guarded")), 0);
    const auto* dormant = runtime.symbol("extra.Value");
    ASSERT_NE(dormant, nullptr);
    EXPECT_FALSE(runtime.is_symbol_active(*dormant));
    auto refused = runtime.set_user_value("extra.Value", "false");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, zxconfig::ErrorCode::UnknownSymbol);

    IncludeHarness ungated{"include-unguarded"};
    ungated.file("root.zx", R"(zxconfig 1.1
option Gate : bool = false
include "extra.zx" visible Gate
computed Direct : bool = extra.Value
)");
    ungated.file("extra.zx",
                 "zxconfig 1.1\nnamespace extra\noption Value : bool = true\n");
    auto bad = ungated.parse();
    ASSERT_TRUE(bad.ok());
    zxconfig::Runtime unguarded{bad.document};
    auto failed = unguarded.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::UnknownSymbol);

    // With the gate open everything reads normally again.
    IncludeHarness open{"include-open"};
    open.file("root.zx", R"(zxconfig 1.1
option Gate : bool = true
include "extra.zx" visible Gate
computed Direct : bool = extra.Value
)");
    open.file("extra.zx",
              "zxconfig 1.1\nnamespace extra\noption Value : bool = true\n");
    auto good = open.parse();
    ASSERT_TRUE(good.ok());
    zxconfig::Runtime active{good.document};
    ASSERT_TRUE(active.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*active.value("Direct")), 2);
    const auto* live = active.symbol("extra.Value");
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(active.is_symbol_active(*live));
}

TEST(Includes, a_write_that_deactivates_a_gated_include_fails_unguarded_reads) {
    // Atomic staging: writing the gate is refused while an unguarded cross
    // reference would break, and nothing is modified.
    IncludeHarness harness{"include-write-refusal"};
    harness.file("root.zx", R"(zxconfig 1.1
option Gate : bool = true
include "extra.zx" visible Gate
computed Direct : bool = extra.Value
)");
    harness.file("extra.zx",
                 "zxconfig 1.1\nnamespace extra\noption Value : bool = true\n");
    auto parsed = harness.parse();
    ASSERT_TRUE(parsed.ok());
    zxconfig::Runtime runtime{parsed.document};
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*runtime.value("Direct")), 2);

    auto refused = runtime.set_user_value("Gate", "false");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(zxconfig::truth_level(*runtime.value("Gate")), 2);
}

TEST(Includes, findings_from_a_deactivated_file_do_not_fire) {
    IncludeHarness harness{"include-quiet-findings"};
    harness.file("root.zx", R"(zxconfig 1.1
option Gate : bool = false
include "warned.zx" visible Gate
)");
    harness.file("warned.zx", R"(zxconfig 1.1
namespace warned
option Probe : bool = true { warn Probe : "included-file warning" }
warn Probe : "included-file top-level warning"
)");
    auto parsed = harness.parse();
    ASSERT_TRUE(parsed.ok());
    zxconfig::Runtime quiet{parsed.document};
    ASSERT_TRUE(quiet.evaluate().has_value());
    EXPECT_TRUE(quiet.run_validation().empty());

    IncludeHarness open{"include-voiced-findings"};
    open.file("root.zx", R"(zxconfig 1.1
option Gate : bool = true
include "warned.zx" visible Gate
)");
    open.file("warned.zx", R"(zxconfig 1.1
namespace warned
option Probe : bool = true { warn Probe : "included-file warning" }
warn Probe : "included-file top-level warning"
)");
    auto voiced = open.parse();
    ASSERT_TRUE(voiced.ok());
    zxconfig::Runtime loud{voiced.document};
    ASSERT_TRUE(loud.evaluate().has_value());
    ASSERT_EQ(loud.run_validation().size(), 2u);
}

TEST(Includes, a_file_survives_when_any_edge_is_active) {
    // The same file reached from two parents stays active while either edge
    // is open: multiple edges are disjunctive, and the fixpoint re-decides a
    // file that an earlier closed edge left inactive.
    IncludeHarness harness{"include-or"};
    harness.file("root.zx", R"(zxconfig 1.1
option RootGate : bool = false
option MidGate : bool = true
include "mid.zx"
include "leaf.zx" visible RootGate
computed MidRead : bool = leaf.Value
)");
    harness.file("mid.zx", "zxconfig 1.1\ninclude \"leaf.zx\" visible MidGate\n");
    harness.file("leaf.zx", "zxconfig 1.1\nnamespace leaf\noption Value : bool = true\n");
    auto parsed = harness.parse();
    ASSERT_TRUE(parsed.ok()) << (parsed.diagnostics.empty() ? "" :
                                parsed.diagnostics.front().message);
    zxconfig::Runtime runtime{parsed.document};
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*runtime.value("MidRead")), 2);
    const auto* live = runtime.symbol("leaf.Value");
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(runtime.is_symbol_active(*live));

    // With every edge closed the file deactivates; a guarded probe reports
    // it absent instead of failing the manifest.
    IncludeHarness closed{"include-or-closed"};
    closed.file("root.zx", R"(zxconfig 1.1
option RootGate : bool = false
option MidGate : bool = false
include "mid.zx"
include "leaf.zx" visible RootGate
computed Guarded : bool = is_defined(leaf.Value)
)");
    closed.file("mid.zx", "zxconfig 1.1\ninclude \"leaf.zx\" visible MidGate\n");
    closed.file("leaf.zx", "zxconfig 1.1\nnamespace leaf\noption Value : bool = true\n");
    auto off = closed.parse();
    ASSERT_TRUE(off.ok());
    zxconfig::Runtime quiet{off.document};
    ASSERT_TRUE(quiet.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*quiet.value("Guarded")), 0);
}

TEST(Targets, aliases_reach_global_symbols_from_namespaced_files) {
    // An alias in a namespace file may target a global symbol: the chase
    // follows the canonicalized target and the alias reads as its target.
    IncludeHarness harness{"target-scope"};
    harness.file("root.zx", R"(zxconfig 1.1
option RootValue : int = 5
include "ns.zx"
)");
    harness.file("ns.zx", R"(zxconfig 1.1
namespace ns
alias Quick = RootValue
deprecated LEGACY_WORKERS -> RootValue {
    message "workers moved"
}
)");
    auto parsed = harness.parse();
    ASSERT_TRUE(parsed.ok()) << (parsed.diagnostics.empty() ? "" :
                                parsed.diagnostics.front().message);
    // Both targets canonicalized to the global symbol, not ns.RootValue.
    ASSERT_EQ(parsed.document.aliases.size(), 1u);
    EXPECT_EQ(parsed.document.aliases[0].target, "RootValue");
    ASSERT_EQ(parsed.document.deprecated.size(), 1u);
    EXPECT_EQ(parsed.document.deprecated[0].new_name, "RootValue");
    zxconfig::Runtime runtime{parsed.document};
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::int64_t>(*runtime.value("ns.Quick")), 5);
    // Cache migration renames legacy assignments onto the canonical target.
    zxconfig::CacheDocument legacy;
    legacy.assignments.push_back({"LEGACY_WORKERS", "2", zxconfig::ValueType::Int});
    std::vector<zxconfig::Rename> renames;
    renames.reserve(parsed.document.deprecated.size());
    for (const auto& entry : parsed.document.deprecated) {
        renames.push_back({entry.old_name, entry.new_name, entry.message});
    }
    const auto renamed = zxconfig::rename_assignments(legacy, renames);
    ASSERT_EQ(renamed.size(), 1u);
    ASSERT_NE(legacy.find("RootValue"), nullptr);
    EXPECT_EQ(legacy.find("RootValue")->value, "2");
}

TEST(PathConstraints, relative_values_resolve_against_the_root_manifest_directory) {
    // The base for relative value paths is deliberately the root manifest's
    // directory: a manifest checked out anywhere still finds its project's
    // files, whatever the process working directory happens to be.
    IncludeHarness harness{"path-base"};
    harness.file("root.zx", R"(zxconfig 1.1
option Sysroot : path = "sysroot" {
    path must_exist | is_dir
}
)");
    // The target exists beside the manifest and nowhere else.
    fs::create_directories(harness.dir / "sysroot");

    auto parsed = harness.parse();
    ASSERT_TRUE(parsed.ok()) << (parsed.diagnostics.empty() ? "" :
                                parsed.diagnostics.front().message);
    zxconfig::Runtime runtime{parsed.document};
    // Defaults are validated at evaluation, so even reading the tree goes
    // through the manifest-relative base.
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*runtime.value("Sysroot")), "sysroot");

    // A staged write resolves the same way, and an absolute path is used
    // as given.
    ASSERT_TRUE(runtime.set_user_value("Sysroot", "sysroot").has_value());
    ASSERT_TRUE(
        runtime.set_user_value("Sysroot", (harness.dir / "sysroot").string())
            .has_value());

    // A name that exists only in the process working directory is refused:
    // the base is the manifest's directory, not the cwd.  The refusal names
    // the resolved path, showing which base was consulted.
    const auto probe = fs::current_path() / "zxconfig-path-cwd-probe.txt";
    std::ofstream{probe} << "probe";
    auto refused = runtime.set_user_value("Sysroot", probe.filename().string());
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, zxconfig::ErrorCode::PathConstraint);
    EXPECT_NE(refused.error().message.find(harness.dir.string()),
              std::string::npos);
    std::error_code ec;
    fs::remove(probe, ec);
}
