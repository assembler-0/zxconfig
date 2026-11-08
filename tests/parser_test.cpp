// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/parser.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using zxconfig::ExprKind;
using zxconfig::TokenKind;

namespace {

zxconfig::ParseResult parse_text(std::string_view text) {
    zxconfig::SourceArena arena;
    auto id = arena.add("t.zx", std::string{text});
    EXPECT_TRUE(id.has_value());
    return zxconfig::parse_document(arena, *id);
}

const zxconfig::Symbol* find(const zxconfig::ParseResult& result, std::string_view name) {
    const auto it = result.document.symbols.find(std::string{name});
    return it == result.document.symbols.end() ? nullptr : it->second;
}

}  // namespace

TEST(Parser, parses_the_version_header) {
    auto result = parse_text("zxconfig 1.0\noption A : bool\n");
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.document.version_major, 1);
    EXPECT_EQ(result.document.version_minor, 0);
}

TEST(Parser, accepts_a_header_without_minor) {
    auto result = parse_text("zxconfig 1\n");
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.document.version_minor, 0);
}

TEST(Parser, rejects_a_missing_header) {
    auto result = parse_text("option A : bool\n");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code, zxconfig::ErrorCode::MissingVersion);
}

TEST(Parser, rejects_a_newer_format) {
    auto result = parse_text("zxconfig 2.0\n");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code, zxconfig::ErrorCode::UnsupportedVersion);
}

TEST(Parser, parses_options_with_types) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option A : bool\n"
        "option B : tristate\n"
        "option C : int\n"
        "option D : uint\n"
        "option E : hex\n"
        "option F : string\n"
        "option G : path\n"
        "option H : enum\n"
        "option I : set\n"
        "option J : list<string>\n");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.document.symbols.size(), 10u);
    EXPECT_EQ(find(result, "A")->type, zxconfig::ValueType::Bool);
    EXPECT_EQ(find(result, "B")->type, zxconfig::ValueType::Tristate);
    EXPECT_EQ(find(result, "J")->type, zxconfig::ValueType::List);
    EXPECT_EQ(find(result, "J")->element, zxconfig::ValueType::String);
}

TEST(Parser, parses_defaults) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option A : bool = true\n"
        "option B : tristate = m\n"
        "option C : int = 4\n"
        "option D : hex = 0xFFFF\n"
        "option E : string = \"text\"\n"
        "option F : set = {}\n"
        "option G : list<string> = [\"a\", \"b\"]\n");
    ASSERT_TRUE(result.ok());

    EXPECT_EQ(find(result, "A")->meta.defaults.front().value->number, 1);
    EXPECT_EQ(find(result, "B")->meta.defaults.front().value->kind, ExprKind::Ident);
    EXPECT_EQ(find(result, "B")->meta.defaults.front().value->text, "m");
    EXPECT_EQ(find(result, "C")->meta.defaults.front().value->number, 4);
    EXPECT_EQ(find(result, "D")->meta.defaults.front().value->number, 0xFFFF);
    EXPECT_EQ(find(result, "E")->meta.defaults.front().value->text, "text");
    EXPECT_EQ(find(result, "F")->meta.defaults.front().value->kind, ExprKind::SequenceLit);
    EXPECT_TRUE(find(result, "F")->meta.defaults.front().value->args.empty());
    ASSERT_EQ(find(result, "G")->meta.defaults.front().value->args.size(), 2u);
    EXPECT_EQ(find(result, "G")->meta.defaults.front().value->args[0]->text, "a");
}

TEST(Parser, parses_binary_expression_spans_without_moving_before_reading) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option A : bool = true {\n"
        "    warn A == true : \"A is enabled\"\n"
        "}\n");
    ASSERT_TRUE(result.ok()) << result.diagnostics.front().message;
    const auto* symbol = find(result, "A");
    ASSERT_NE(symbol, nullptr);
    ASSERT_EQ(symbol->meta.warnings.size(), 1u);
    ASSERT_NE(symbol->meta.warnings.front().condition, nullptr);
    EXPECT_EQ(symbol->meta.warnings.front().condition->kind, ExprKind::Binary);
}

TEST(Parser, parses_metadata) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option SMP : bool = false {\n"
        "    label \"Symmetric multiprocessing\"\n"
        "    help  \"Enable support for multiple CPU cores.\"\n"
        "    tags  [\"smp\", \"cpu\"]\n"
        "    order 99\n"
        "    range 1 .. 512\n"
        "    pattern \"^\\\\d+\"\n"
        "    path must_exist | is_dir\n"
        "    choices a | b\n"
        "    when C\n"
        "    enabled D >= 4\n"
        "    implies E\n"
        "}\n");
    ASSERT_TRUE(result.ok()) << result.diagnostics.front().message;

    const auto* smp = find(result, "SMP");
    ASSERT_NE(smp, nullptr);
    EXPECT_EQ(smp->meta.label, "Symmetric multiprocessing");
    EXPECT_EQ(smp->meta.help, "Enable support for multiple CPU cores.");
    ASSERT_EQ(smp->meta.tags.size(), 2u);
    EXPECT_EQ(smp->meta.order, 99);
    ASSERT_TRUE(smp->meta.range.has_value());
    EXPECT_EQ(smp->meta.range->lo, 1);
    EXPECT_EQ(smp->meta.range->hi, 512);
    EXPECT_EQ(smp->meta.pattern, "^\\d+");
    ASSERT_EQ(smp->meta.path_constraints.size(), 2u);
    ASSERT_EQ(smp->meta.choices.size(), 2u);
    ASSERT_NE(smp->meta.when, nullptr);
    ASSERT_NE(smp->meta.enabled, nullptr);
    ASSERT_EQ(smp->meta.implies.size(), 1u);
}

TEST(Parser, respects_operator_precedence) {
    auto result = parse_text("zxconfig 1.0\ncomputed X : int = 1 + 2 * 3\n");
    ASSERT_TRUE(result.ok());
    const auto* x = find(result, "X");
    const auto& top = *x->meta.defaults.front().value;
    ASSERT_EQ(top.kind, ExprKind::Binary);
    EXPECT_EQ(top.op.kind, TokenKind::Plus);
    EXPECT_EQ(top.args[0]->number, 1);
    ASSERT_EQ(top.args[1]->kind, ExprKind::Binary);
    EXPECT_EQ(top.args[1]->op.kind, TokenKind::Star);
    EXPECT_EQ(top.args[1]->args[0]->number, 2);
    EXPECT_EQ(top.args[1]->args[1]->number, 3);
}

TEST(Parser, folds_boolean_operators) {
    auto result = parse_text("zxconfig 1.0\ncomputed X : bool = a && b || !c\n");
    ASSERT_TRUE(result.ok());
    const auto& top = *find(result, "X")->meta.defaults.front().value;
    ASSERT_EQ(top.kind, ExprKind::Binary);
    EXPECT_EQ(top.op.kind, TokenKind::PipePipe);
    EXPECT_EQ(top.args[0]->op.kind, TokenKind::AmpAmp);
    ASSERT_EQ(top.args[1]->kind, ExprKind::Unary);
    EXPECT_EQ(top.args[1]->op.kind, TokenKind::Bang);
}

TEST(Parser, parses_ternary_and_bitwise) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "computed X : hex = is_defined(A) ? (1 << 8) - 1 : B | 0xFF\n");
    ASSERT_TRUE(result.ok()) << result.diagnostics.front().message;
    const auto& top = *find(result, "X")->meta.defaults.front().value;
    ASSERT_EQ(top.kind, ExprKind::Ternary);
    ASSERT_EQ(top.args.size(), 3u);
    EXPECT_EQ(top.args[0]->kind, ExprKind::Call);
    EXPECT_EQ(top.args[0]->text, "is_defined");
    EXPECT_EQ(top.args[1]->op.kind, TokenKind::Minus);
    EXPECT_EQ(top.args[2]->op.kind, TokenKind::Pipe);
}

TEST(Parser, parses_probe_with_fallback) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option HOSTNAME : string = shell(\"hostname\") fallback \"localhost\"\n");
    ASSERT_TRUE(result.ok());
    const auto& value = *find(result, "HOSTNAME")->meta.defaults.front().value;
    ASSERT_EQ(value.kind, ExprKind::Fallback);
    ASSERT_EQ(value.args.size(), 2u);
    EXPECT_EQ(value.args[0]->kind, ExprKind::Call);
    EXPECT_EQ(value.args[0]->text, "shell");
    EXPECT_EQ(value.args[1]->text, "localhost");
}

TEST(Parser, rejects_an_unknown_builtin) {
    auto result = parse_text("zxconfig 1.0\ncomputed X : int = frobnicate(1)\n");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code, zxconfig::ErrorCode::UnknownBuiltin);
}

TEST(Parser, rejects_a_wrong_arity) {
    auto result = parse_text("zxconfig 1.0\ncomputed X : int = min(1)\n");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code, zxconfig::ErrorCode::ArityMismatch);
}

TEST(Parser, parses_menus) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "menu \"Memory\" {\n"
        "    option A : bool\n"
        "    menu \"NUMA\" visible SMP {\n"
        "        option B : bool\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    const auto stats = zxconfig::document_stats(result.document);
    EXPECT_EQ(stats.symbols, 2u);
    EXPECT_EQ(stats.menus, 3u);  // implicit root plus the two named menus

    const auto& memory = std::get<zxconfig::MenuPtr>(result.document.root->children[0]);
    EXPECT_EQ(memory->label, "Memory");
    const auto& numa = std::get<zxconfig::MenuPtr>(memory->children[1]);
    EXPECT_EQ(numa->label, "NUMA");
    ASSERT_NE(numa->visible, nullptr);
    EXPECT_EQ(numa->visible->text, "SMP");
}

TEST(Parser, parses_conditional_defaults) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option HZ : int {\n"
        "    default {\n"
        "        1000 when PREEMPT_MODEL == realtime\n"
        "        100\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    const auto& defaults = find(result, "HZ")->meta.defaults;
    ASSERT_EQ(defaults.size(), 2u);
    ASSERT_NE(defaults[0].condition, nullptr);
    EXPECT_EQ(defaults[0].condition->op.kind, TokenKind::EqEq);
    EXPECT_EQ(defaults[1].condition, nullptr);
    EXPECT_EQ(defaults[1].value->number, 100);
}

TEST(Parser, parses_computed_with_branches) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "computed MAX_IRQ : int {\n"
        "    1024 when ARCH == arm64\n"
        "    512\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    const auto* irq = find(result, "MAX_IRQ");
    EXPECT_TRUE(irq->computed);
    EXPECT_EQ(irq->meta.defaults.size(), 2u);
}

TEST(Parser, parses_validate_and_warn) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "validate \"Insufficient memory\" {\n"
        "    RAM_SIZE >= THREADS * 128\n"
        "}\n"
        "warn \"Low memory\" {\n"
        "    RAM_SIZE >= 256\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.document.validations.size(), 2u);
    EXPECT_FALSE(result.document.validations[0].warning);
    EXPECT_TRUE(result.document.validations[1].warning);
}

TEST(Parser, parses_generate_directives) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "generate header \"config.h\"\n"
        "generate makefile \"config.mk\"\n"
        "generate template \"config.ld\" from \"linker.ld.tpl\"\n");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.document.generates.size(), 3u);
    EXPECT_EQ(result.document.generates[0].backend,
              zxconfig::GenerateDirective::Backend::Header);
    EXPECT_EQ(result.document.generates[2].backend,
              zxconfig::GenerateDirective::Backend::Template);
    EXPECT_EQ(result.document.generates[2].template_path, "linker.ld.tpl");
}

TEST(Parser, parses_alias_and_deprecated) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "alias NET_VIRTIO = VIRTIO_NET\n"
        "deprecated SLAB_DEBUG -> SLUB_DEBUG {\n"
        "    message \"renamed in 1.1\"\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.document.aliases.size(), 1u);
    EXPECT_EQ(result.document.aliases[0].alias, "NET_VIRTIO");
    ASSERT_EQ(result.document.deprecated.size(), 1u);
    EXPECT_EQ(result.document.deprecated[0].old_name, "SLAB_DEBUG");
    EXPECT_EQ(result.document.deprecated[0].new_name, "SLUB_DEBUG");
    EXPECT_EQ(result.document.deprecated[0].message, "renamed in 1.1");
}

TEST(Parser, reports_duplicate_symbols) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option A : bool\n"
        "option A : bool\n");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.back().code, zxconfig::ErrorCode::DuplicateSymbol);
    ASSERT_EQ(result.document.symbols.count("A"), 1u);
}

TEST(Parser, keeps_parsing_after_errors) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option : bool\n"
        "option B : bool\n"
        "option C : ???\n"
        "option D : bool\n");
    ASSERT_FALSE(result.ok());
    EXPECT_NE(find(result, "B"), nullptr);
    EXPECT_NE(find(result, "D"), nullptr);
}

TEST(Parser, namespaces_included_symbols) {
    const auto dir = fs::temp_directory_path() / "zxconfig-parser-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::ofstream{dir / "net.zx"} << "zxconfig 1.0\noption VIRTIO : bool\n";
    std::ofstream{dir / "root.zx"} << "zxconfig 1.0\ninclude \"net.zx\" as net\n";

    zxconfig::SourceArena arena;
    auto root = arena.load((dir / "root.zx").string(), {}, dir.string());
    ASSERT_TRUE(root.has_value());
    auto result = zxconfig::parse_document(arena, *root, dir.string());

    ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" :
                                result.diagnostics.front().message);
    ASSERT_EQ(result.document.includes.size(), 1u);
    EXPECT_EQ(result.document.includes[0].namespace_, "net");
    ASSERT_EQ(result.document.symbols.size(), 1u);
    EXPECT_NE(result.document.symbols.find("net.VIRTIO"), result.document.symbols.end());

    fs::remove_all(dir);
}

TEST(Parser, skips_an_unnamespaced_double_include) {
    const auto dir = fs::temp_directory_path() / "zxconfig-parser-test2";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::ofstream{dir / "leaf.zx"} << "zxconfig 1.0\noption A : bool\n";
    std::ofstream{dir / "root.zx"}
        << "zxconfig 1.0\ninclude \"leaf.zx\"\ninclude \"leaf.zx\"\n";

    zxconfig::SourceArena arena;
    auto root = arena.load((dir / "root.zx").string(), {}, dir.string());
    ASSERT_TRUE(root.has_value());
    auto result = zxconfig::parse_document(arena, *root, dir.string());

    EXPECT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" :
                                result.diagnostics.front().message);
    EXPECT_EQ(result.document.symbols.size(), 1u);

    fs::remove_all(dir);
}

TEST(Parser, survives_an_include_cycle) {
    const auto dir = fs::temp_directory_path() / "zxconfig-parser-test3";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::ofstream{dir / "a.zx"} << "zxconfig 1.0\ninclude \"b.zx\"\n";
    std::ofstream{dir / "b.zx"} << "zxconfig 1.0\ninclude \"a.zx\"\n";

    zxconfig::SourceArena arena;
    auto root = arena.load((dir / "a.zx").string());
    ASSERT_TRUE(root.has_value());
    auto result = zxconfig::parse_document(arena, *root, dir.string());

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.document.symbols.size(), 0u);

    fs::remove_all(dir);
}

TEST(Parser, reports_a_missing_include) {
    zxconfig::SourceArena arena;
    auto id = arena.add("root.zx", "zxconfig 1.0\ninclude \"no/such/file.zx\"\n");
    auto result = zxconfig::parse_document(arena, *id);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code, zxconfig::ErrorCode::FileNotFound);
}

TEST(Parser, folds_include_conditions) {
    const auto dir = fs::temp_directory_path() / "zxconfig-parser-test4";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::ofstream{dir / "leaf.zx"} << "zxconfig 1.0\noption A : bool { visible B }\n";
    std::ofstream{dir / "root.zx"}
        << "zxconfig 1.0\noption B : bool\ninclude \"leaf.zx\" visible NET\n";

    zxconfig::SourceArena arena;
    auto root = arena.load((dir / "root.zx").string(), {}, dir.string());
    ASSERT_TRUE(root.has_value());
    auto result = zxconfig::parse_document(arena, *root, dir.string());

    ASSERT_TRUE(result.ok());
    const auto* a = result.document.symbols.find("A") != result.document.symbols.end()
                  ? result.document.symbols.at("A") : nullptr;
    ASSERT_NE(a, nullptr);
    ASSERT_NE(a->meta.visible, nullptr);
    ASSERT_EQ(a->meta.visible->kind, ExprKind::Binary);
    EXPECT_EQ(a->meta.visible->op.kind, TokenKind::AmpAmp);
    EXPECT_EQ(a->meta.visible->args[0]->text, "NET");
    EXPECT_EQ(a->meta.visible->args[1]->text, "B");

    fs::remove_all(dir);
}

TEST(Parser, parses_semicolon_separators) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option A : bool {\n"
        "    label \"x\"; help \"y\"; when B\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(find(result, "A")->meta.label, "x");
}

TEST(Parser, parses_conflict_clauses) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option FORCE_PREEMPT : bool = false {\n"
        "    implies PREEMPT_MODEL == full\n"
        "    conflict PREEMPT_MODEL {\n"
        "        resolve \"override PREEMPT_MODEL?\"\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    const auto& conflicts = find(result, "FORCE_PREEMPT")->meta.conflicts;
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].symbol, "PREEMPT_MODEL");
    EXPECT_EQ(conflicts[0].message, "override PREEMPT_MODEL?");
}

TEST(Parser, parses_choice_metadata) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option PREEMPT : enum = voluntary {\n"
        "    choices none | voluntary | full\n"
        "    choice voluntary { label \"Voluntary\"; visible EXPERT }\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    const auto* preempt = find(result, "PREEMPT");
    ASSERT_EQ(preempt->meta.choices.size(), 3u);
    ASSERT_EQ(preempt->meta.choice_meta.size(), 1u);
    EXPECT_EQ(preempt->meta.choice_meta[0].name, "voluntary");
    EXPECT_EQ(preempt->meta.choice_meta[0].label, "Voluntary");
    ASSERT_NE(preempt->meta.choice_meta[0].visible, nullptr);
}

TEST(Parser, parses_metadata_warn) {
    auto result = parse_text(
        "zxconfig 1.0\n"
        "option CLANG : string = shell(\"clang --version\") fallback \"\" {\n"
        "    warn CLANG == \"\" : \"clang not found\"\n"
        "}\n");
    ASSERT_TRUE(result.ok());
    const auto& warnings = find(result, "CLANG")->meta.warnings;
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0].message, "clang not found");
    ASSERT_NE(warnings[0].condition, nullptr);
    EXPECT_EQ(warnings[0].condition->op.kind, TokenKind::EqEq);
}

TEST(Macros, expand_declarations_at_the_call_site) {
    auto result = parse_text(R"(zxconfig 1.0
macro DRIVER(id, label_name) {
    option DRV_${id} : bool = false {
        label "${label_name} Support"
        tags  ["driver", "${id}"]
    }
}

DRIVER(VIRTIO, "VirtIO Virtual Block")
DRIVER(NVME, "Non-Volatile Memory Express")
)");
    ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" :
                                result.diagnostics.front().message);
    ASSERT_EQ(result.document.symbols.size(), 2u);

    const auto* virtio = find(result, "DRV_VIRTIO");
    ASSERT_NE(virtio, nullptr);
    EXPECT_EQ(virtio->meta.label, "VirtIO Virtual Block Support");
    ASSERT_EQ(virtio->meta.tags.size(), 2u);
    EXPECT_EQ(virtio->meta.tags[0], "driver");
    EXPECT_EQ(virtio->meta.tags[1], "VIRTIO");
    EXPECT_NE(find(result, "DRV_NVME"), nullptr);
}

TEST(Macros, substitute_expression_fragments) {
    auto result = parse_text(R"(zxconfig 1.0
option NET : bool = true
export macro GUARDED(id, guard) {
    option DRV_${id} : bool = false {
        visible ${guard} and NET
    }
}
GUARDED(X, NET)
)");
    ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" :
                                result.diagnostics.front().message);
    const auto* symbol = find(result, "DRV_X");
    ASSERT_NE(symbol, nullptr);
    ASSERT_NE(symbol->meta.visible, nullptr);
    EXPECT_EQ(symbol->meta.visible->op.kind, TokenKind::KwAnd);
}

TEST(Macros, leave_non_parameter_names_for_interpolation) {
    auto result = parse_text(R"(zxconfig 1.0
option PREFIX : string = "/usr"
macro DIRS(leaf) {
    option P_${leaf} : string = "${PREFIX}/${leaf}"
}
DIRS(BIN)
)");
    ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" :
                                result.diagnostics.front().message);
    const auto* symbol = find(result, "P_BIN");
    ASSERT_NE(symbol, nullptr);
    // ${PREFIX} is a runtime reference, not a parameter: it survives
    // expansion and is interpolated at evaluation.
    // ${leaf} is a parameter and was substituted; ${PREFIX} survives.
    EXPECT_EQ(symbol->meta.defaults.front().value->text, "${PREFIX}/BIN");
}

TEST(Macros, reject_undeclared_calls_and_bad_arity) {
    auto undeclared = parse_text(R"(zxconfig 1.0
NOPE(1)
)");
    ASSERT_FALSE(undeclared.ok());
    EXPECT_EQ(undeclared.diagnostics.front().code, zxconfig::ErrorCode::UndeclaredMacro);

    auto bad_arity = parse_text(R"(zxconfig 1.0
macro ONE(a) { option X_${a} : bool }
ONE()
)");
    ASSERT_FALSE(bad_arity.ok());
    EXPECT_EQ(bad_arity.diagnostics.front().code, zxconfig::ErrorCode::ArityMismatch);
}

TEST(Macros, reject_redefinition) {
    auto result = parse_text(R"(zxconfig 1.0
macro M(a) { option X_${a} : bool }
macro M(a) { option Y_${a} : bool }
)");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code, zxconfig::ErrorCode::DuplicateSymbol);
}

TEST(Macros, export_reaches_the_includer_only) {
    const auto dir = fs::temp_directory_path() / "zxconfig-macro-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::ofstream{dir / "leaf.zx"} << R"(zxconfig 1.0
export macro OPEN(x) { option L_${x} : bool }
macro CLOSED(y) { option C_${y} : bool }
)";
    std::ofstream{dir / "root.zx"} << R"(zxconfig 1.0
include "leaf.zx"
OPEN(A)
CLOSED(B)
)";

    zxconfig::SourceArena arena;
    auto root = arena.load((dir / "root.zx").string(), {}, dir.string());
    ASSERT_TRUE(root.has_value());
    auto result = zxconfig::parse_document(arena, *root, dir.string());

    ASSERT_FALSE(result.ok());
    // OPEN is exported and expands; CLOSED is file-local and reports.
    EXPECT_NE(result.document.symbols.find("L_A"), result.document.symbols.end());
    EXPECT_EQ(result.document.symbols.find("C_B"), result.document.symbols.end());
    bool saw_undeclared = false;
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == zxconfig::ErrorCode::UndeclaredMacro) saw_undeclared = true;
    }
    EXPECT_TRUE(saw_undeclared);

    fs::remove_all(dir);
}

TEST(Macros, expansions_record_their_declarations) {
    auto result = parse_text(R"(zxconfig 1.0
macro M(a) { option X_${a} : bool }
M(1)
)");
    ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" :
                                result.diagnostics.front().message);
    ASSERT_EQ(result.document.macros.size(), 1u);
    EXPECT_EQ(result.document.macros.front().name, "M");
    EXPECT_EQ(result.document.macros.front().params.size(), 1u);
}

TEST(Parser, parses_declarations_after_an_include) {
    // Regression: an include used to clobber the including file's cursor,
    // silently dropping everything after it.
    const auto dir = fs::temp_directory_path() / "zxconfig-include-tail-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::ofstream{dir / "leaf.zx"} << "zxconfig 1.0\noption LEAF : bool\n";
    std::ofstream{dir / "root.zx"}
        << "zxconfig 1.0\ninclude \"leaf.zx\"\noption AFTER : bool\n";

    zxconfig::SourceArena arena;
    auto root = arena.load((dir / "root.zx").string(), {}, dir.string());
    ASSERT_TRUE(root.has_value());
    auto result = zxconfig::parse_document(arena, *root, dir.string());

    ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" :
                                result.diagnostics.front().message);
    EXPECT_NE(result.document.symbols.find("LEAF"), result.document.symbols.end());
    EXPECT_NE(result.document.symbols.find("AFTER"), result.document.symbols.end());

    fs::remove_all(dir);
}

TEST(PresentationOrder, order_overrides_declaration_order) {
    auto result = parse_text(R"(zxconfig 1.0
menu "M" {
    option A : bool
    option LATE : bool { order 99 }
    option EARLY : bool { order -5 }
    option B : bool
}
)");
    ASSERT_TRUE(result.ok());
    const auto& menu = *std::get<zxconfig::MenuPtr>(result.document.root->children[0]);
    ASSERT_EQ(menu.children.size(), 4u);
    const auto order = zxconfig::ordered_children(menu);
    ASSERT_EQ(order.size(), 4u);
    // EARLY(-5) first, LATE(99) last, the unordered pair keeps declaration order.
    EXPECT_EQ(order[0], 2u);
    EXPECT_EQ(order[1], 0u);
    EXPECT_EQ(order[2], 3u);
    EXPECT_EQ(order[3], 1u);
}

TEST(Conflicts, error_policy_parses_alongside_prompt) {
    auto result = parse_text(R"(zxconfig 1.0
option S : bool {
    conflict T { error "hard failure" }
    conflict U { resolve "soft prompt" }
}
)");
    ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" :
                                result.diagnostics.front().message);
    const auto& conflicts = find(result, "S")->meta.conflicts;
    ASSERT_EQ(conflicts.size(), 2u);
    EXPECT_EQ(conflicts[0].policy, zxconfig::ConflictPolicy::Error);
    EXPECT_EQ(conflicts[0].message, "hard failure");
    EXPECT_EQ(conflicts[1].policy, zxconfig::ConflictPolicy::Prompt);
    EXPECT_EQ(conflicts[1].message, "soft prompt");
}
