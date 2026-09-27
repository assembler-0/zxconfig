// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/eval.hpp>
#include <zxconfig/parser.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

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

TEST(Evaluator, resolves_defaults_and_visibility) {
    Harness runtime_harness{R"(zxconfig 1.0
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
    Harness runtime_harness{R"(zxconfig 1.0
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
    Harness runtime_harness{R"(zxconfig 1.0
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
    Harness rt_harness{R"(zxconfig 1.0
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
    Harness runtime_harness{R"(zxconfig 1.0
option A : bool = B
option B : bool = A
)"};
    auto& runtime = runtime_harness.runtime;
    auto failed = runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::CircularDependency);
}

TEST(Evaluator, division_by_zero_fails_the_expression) {
    Harness runtime_harness{R"(zxconfig 1.0
option Z : int = 0
computed X : int = 5 / Z
)"};
    auto& runtime = runtime_harness.runtime;
    auto failed = runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::DivisionByZero);
}

TEST(Evaluator, unknown_symbols_are_reported) {
    Harness runtime_harness{R"(zxconfig 1.0
computed X : bool = NOTHING
)"};
    auto& runtime = runtime_harness.runtime;
    auto failed = runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::UnknownSymbol);
}

TEST(Evaluator, env_and_shell_probes) {
    ::setenv("ZX_TEST_VAR", "hello", 1);
    Harness runtime_harness{R"(zxconfig 1.0
option E : string = env("ZX_TEST_VAR")
option S : string = shell("echo world")
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*runtime.value("E")), "hello");
    EXPECT_EQ(std::get<std::string>(*runtime.value("S")), "world");
}

TEST(Evaluator, failed_probe_uses_fallback) {
    Harness runtime_harness{R"(zxconfig 1.0
option S : string = shell("exit 1") fallback "backup"
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*runtime.value("S")), "backup");
}

TEST(Evaluator, user_values_are_validated) {
    Harness runtime_harness{R"(zxconfig 1.0
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
    Harness runtime_harness{R"(zxconfig 1.0
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
    Harness runtime_harness{R"(zxconfig 1.0
option RAM : int = 128
validate "RAM must be at least 256" {
    RAM >= 256
}
warn "small RAM" {
    RAM >= 256
}
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    auto findings = runtime.run_validation();
    ASSERT_EQ(findings.size(), 2u);
    EXPECT_EQ(findings[0].severity, zxconfig::Severity::Error);
    EXPECT_EQ(findings[1].severity, zxconfig::Severity::Warning);
    // A warn clause fires when its condition is violated, exactly like a
    // validate clause, but without blocking the save.
}

TEST(Evaluator, to_cache_round_trips_through_apply) {
    Harness runtime_harness{R"(zxconfig 1.0
option SMP : bool = false
option N : int = 4
)"};
    auto& runtime = runtime_harness.runtime;
    ASSERT_TRUE(runtime.evaluate().has_value());
    ASSERT_TRUE(runtime.set_user_value("SMP", "true").has_value());
    ASSERT_TRUE(runtime.set_user_value("N", "64").has_value());

    auto document = runtime.to_cache("manifest", std::string(64, 'a'));
    ASSERT_EQ(document.assignments.size(), 2u);

    Harness fresh_harness{R"(zxconfig 1.0
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
    // condition holds — the inverse of the top-level warn form.
    Harness harness{R"(zxconfig 1.0
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
    Harness harness{R"(zxconfig 1.0
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
    Harness harness{R"(zxconfig 1.0
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
    Harness harness{R"(zxconfig 1.0
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
    Harness harness{R"(zxconfig 1.0
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

TEST(Aliases, chains_resolve_and_cycles_stop) {
    Harness harness{R"(zxconfig 1.0
option REAL : bool = true
alias MID = REAL
alias FAR = MID
computed USES : bool = FAR
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(zxconfig::truth_level(*harness.runtime.value("USES")), 2);

    Harness loop{R"(zxconfig 1.0
alias A = B
alias B = A
computed USES : bool = A
)"};
    auto failed = loop.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::UnknownSymbol);
}

TEST(Interpolation, expands_symbols_in_string_values) {
    Harness harness{R"(zxconfig 1.0
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
    Harness harness{R"(zxconfig 1.0
option REAL : string = "/opt"
alias SHORT = REAL
computed USES : string = "${SHORT}/bin"
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    EXPECT_EQ(std::get<std::string>(*harness.runtime.value("USES")), "/opt/bin");
}

TEST(Interpolation, unknown_names_and_unset_targets_fail) {
    Harness unknown{R"(zxconfig 1.0
computed X : string = "${NOPE}/bin"
)"};
    auto failed = unknown.runtime.evaluate();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, zxconfig::ErrorCode::Interpolation);

    Harness unclosed{R"(zxconfig 1.0
computed X : string = "${NOPE"
)"};
    auto bad = unclosed.runtime.evaluate();
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, zxconfig::ErrorCode::Interpolation);

    Harness unset{R"(zxconfig 1.0
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
    Harness harness{R"(zxconfig 1.0
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
    Harness harness{R"(zxconfig 1.0
option PREFIX : string = "/usr"
computed PATHS : list<string> = ["${PREFIX}/bin", "${PREFIX}/lib"]
)"};
    ASSERT_TRUE(harness.runtime.evaluate().has_value());
    const auto& items = std::get<std::vector<std::string>>(*harness.runtime.value("PATHS"));
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0], "/usr/bin");
    EXPECT_EQ(items[1], "/usr/lib");
}
