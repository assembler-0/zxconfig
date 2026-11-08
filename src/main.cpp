// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/ast.hpp>
#include <zxconfig/cache.hpp>
#include <zxconfig/eval.hpp>
#include <zxconfig/generator.hpp>
#include <zxconfig/lexer.hpp>
#include <zxconfig/log.hpp>
#include <zxconfig/parser.hpp>
#include <zxconfig/source.hpp>
#include <zxconfig/tui.hpp>
#include <zxconfig/version.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::string_view k_usage =
    "zxconfig — configuration engine\n"
    "\n"
    "usage: zxconfig [option]... [file]\n"
    "\n"
    "options:\n"
    "  -f, --file <path>     root configuration file (default: ./ZXConfig)\n"
    "      --root <path>     directory includes may not escape (default: the file's directory)\n"
    "      --parse-only      parse the manifest and report, then exit\n"
    "      --dump-tokens     print the token stream of the root file and exit\n"
    "      --cache-force     apply a stale cache, dropping unknown symbols\n"
    "      --preset <path>   apply a preset (cache format, repeatable)\n"
    "      --set <N>=<V>     set a symbol, e.g. --set NR_CPUS=16 (repeatable)\n"
    "      --unset <name>    return a symbol to its default (repeatable)\n"
    "      --save [path]     write the cache (or <path>) after applying inputs\n"
    "      --generate        run the generate directives and exit\n"
    "      --force           proceed despite validation errors\n"
    "      --headless        never open the interface, even on a terminal\n"
    "  -v, --verbose         record at debug level\n"
    "      --trace           record at trace level\n"
    "  -q, --quiet           record failures only\n"
    "      --log <path>      write the log to a file instead of standard error\n"
    "      --color <when>    always, never, or auto (default: auto)\n"
    "      --version         print the version and exit\n"
    "  -h, --help            print this help and exit\n";

struct Invocation {
    std::string file{"ZXConfig"};
    std::string root;
    std::string log_path;
    zxconfig::log::Level level{zxconfig::log::Level::Info};
    int color{-1};  ///< -1 auto, 0 never, 1 always
    bool parse_only{false};
    bool dump_tokens{false};
    bool cache_force{false};
    bool generate{false};
    bool headless{false};
    bool save{false};
    bool force{false};
    std::string save_path;

    /// @brief One requested input.  Presets, sets, and unsets share one list
    ///        because they apply in the order given on the command line:
    ///        `--set A=1 --unset A --set A=2` must end with A=2.
    struct Input {
        enum class Kind : std::uint8_t { Preset, Set, Unset } kind{Kind::Set};
        std::string name;
        std::string value;
    };
    std::vector<Input> inputs;
};

/// @brief Parse the command line.
/// @return The invocation, or a diagnostic naming the offending argument.
zxconfig::Result<Invocation> parse_args(int argc, char** argv) {
    Invocation out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};

        auto next = [&](std::string_view flag) -> zxconfig::Result<std::string_view> {
            if (i + 1 >= argc) {
                return std::unexpected(zxconfig::Diagnostic::error(
                    zxconfig::ErrorCode::BadInvocation,
                    std::string{flag} + " requires an argument"));
            }
            return std::string_view{argv[++i]};
        };

        if (arg == "-h" || arg == "--help") {
            std::print("{}", k_usage);
            std::exit(0);
        }
        if (arg == "--version") {
            std::println("zxconfig {}", zxconfig::k_version);
            std::exit(0);
        }
        if (arg == "-v" || arg == "--verbose") { out.level = zxconfig::log::Level::Debug; continue; }
        if (arg == "--trace")                  { out.level = zxconfig::log::Level::Trace; continue; }
        if (arg == "-q" || arg == "--quiet")   { out.level = zxconfig::log::Level::Error; continue; }
        if (arg == "--parse-only")             { out.parse_only = true; continue; }
        if (arg == "--dump-tokens")            { out.dump_tokens = true; continue; }
        if (arg == "--cache-force")            { out.cache_force = true; continue; }
        if (arg == "--generate")               { out.generate = true; continue; }
        if (arg == "--headless")               { out.headless = true; continue; }
        if (arg == "--force")                  { out.force = true; continue; }
        if (arg == "--preset") {
            auto value = next(arg);
            if (!value) return std::unexpected(value.error());
            out.inputs.push_back({Invocation::Input::Kind::Preset,
                                  std::string{*value}, {}});
            continue;
        }
        if (arg == "--unset") {
            auto value = next(arg);
            if (!value) return std::unexpected(value.error());
            out.inputs.push_back({Invocation::Input::Kind::Unset,
                                  std::string{*value}, {}});
            continue;
        }
        if (arg == "--set") {
            auto value = next(arg);
            if (!value) return std::unexpected(value.error());
            const auto eq = value->find('=');
            if (eq == std::string_view::npos || eq == 0) {
                return std::unexpected(zxconfig::Diagnostic::error(
                    zxconfig::ErrorCode::BadInvocation,
                    "--set expects NAME=VALUE"));
            }
            out.inputs.push_back({Invocation::Input::Kind::Set,
                                  std::string{value->substr(0, eq)},
                                  std::string{value->substr(eq + 1)}});
            continue;
        }
        if (arg == "--save") {
            out.save = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                out.save_path = argv[++i];
            }
            continue;
        }
        if (arg == "--color") {
            auto value = next(arg);
            if (!value) return std::unexpected(value.error());
            if (*value == "always")     out.color = 1;
            else if (*value == "never") out.color = 0;
            else if (*value == "auto")  out.color = -1;
            else return std::unexpected(zxconfig::Diagnostic::error(
                zxconfig::ErrorCode::BadInvocation,
                "--color expects always, never, or auto"));
            continue;
        }
        if (arg == "--log") {
            auto value = next(arg);
            if (!value) return std::unexpected(value.error());
            out.log_path = std::string{*value};
            continue;
        }
        if (arg == "--root") {
            auto value = next(arg);
            if (!value) return std::unexpected(value.error());
            out.root = std::string{*value};
            continue;
        }
        if (arg == "-f" || arg == "--file") {
            auto value = next(arg);
            if (!value) return std::unexpected(value.error());
            out.file = std::string{*value};
            continue;
        }
        if (arg.starts_with('-')) {
            return std::unexpected(zxconfig::Diagnostic::error(
                zxconfig::ErrorCode::BadInvocation,
                "unknown option '" + std::string{arg} + "'"));
        }
        out.file = std::string{arg};
    }
    return out;
}

/// @brief The engine's half of the tui::Controller seam.
class Session final : public zxconfig::tui::Controller {
public:
    Session(const zxconfig::Document& document,
            zxconfig::Runtime& runtime,
            const zxconfig::SourceArena& arena,
            std::string manifest_name,
            std::string digest,
            std::filesystem::path cache_path,
            std::optional<zxconfig::CacheDocument> pending_cache = std::nullopt)
        : document_{document},
          runtime_{runtime},
          arena_{arena},
          manifest_{std::move(manifest_name)},
          digest_{std::move(digest)},
          cache_path_{std::move(cache_path)},
          pending_cache_{std::move(pending_cache)} {
        zxconfig::log::logger().set_capture([this](zxconfig::log::Level level,
                                                   const std::string& text) {
            std::scoped_lock lock{log_mutex_};
            if (log_.size() == 500) log_.erase(log_.begin());
            log_.push_back({zxconfig::log::to_string(level), text});
        });

        build_entries();
        revalidate();
        refresh_implications();
        set_state("ready");
        if (error_count() > 0 || warning_count() > 0) {
            zxconfig::log::warn("validation: {} error(s), {} warning(s) — press v",
                                error_count(), warning_count());
        }
    }

    ~Session() override { zxconfig::log::logger().set_capture(nullptr); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] std::string title() const override {
        return "zxconfig " + std::string{zxconfig::k_version} + " — " + manifest_;
    }

    [[nodiscard]] std::string state_name() const override { return state_; }

    [[nodiscard]] std::size_t error_count() const override {
        std::scoped_lock lock{findings_mutex_};
        return errors_;
    }

    [[nodiscard]] std::size_t warning_count() const override {
        std::scoped_lock lock{findings_mutex_};
        return warnings_;
    }

    [[nodiscard]] std::size_t modified_count() const override {
        return runtime_.user_symbols().size();
    }

    [[nodiscard]] std::vector<zxconfig::tui::Row> rows(
        const std::vector<std::size_t>& expanded) const override {
        std::vector<zxconfig::tui::Row> out;
        out.reserve(entries_.size());
        if (entries_.empty()) return out;

        std::vector<char> listed(entries_.size(), 0);
        listed[0] = 1;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto& entry = entries_[i];
            const auto parent = entry.parent;
            const bool parent_lists = parent >= entries_.size() ? true : listed[parent] != 0;
            if (!parent_lists) continue;

            const bool is_expanded =
                std::find(expanded.begin(), expanded.end(), i) != expanded.end();

            if (entry.kind == Entry::Kind::Menu) {
                listed[i] = (i == 0 || is_expanded) ? 1 : 0;
                if (i == 0) continue;
                if (!runtime_.is_menu_visible(*entry.menu)) continue;
                out.push_back(menu_row(entry, i, is_expanded));
            } else {
                listed[i] = 0;
                const auto& symbol = *entry.symbol;
                if (!runtime_.is_visible(symbol.name)) continue;
                out.push_back(symbol_row(symbol, i));
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<std::size_t> menu_chain(std::size_t id) const override {
        std::vector<std::size_t> chain;
        for (std::size_t at = id; at < entries_.size(); at = entries_[at].parent) {
            if (entries_[at].kind != Entry::Kind::Menu) continue;
            chain.push_back(at);
            if (entries_[at].parent >= entries_.size()) break;
        }
        if (chain.size() <= 1) return {};
        chain.pop_back();
        std::reverse(chain.begin(), chain.end());
        return chain;
    }

    [[nodiscard]] std::vector<zxconfig::tui::SearchItem> search_items() const override {
        std::vector<zxconfig::tui::SearchItem> items;
        items.reserve(entries_.size());
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto& entry = entries_[i];
            if (entry.kind != Entry::Kind::Symbol) continue;
            if (!runtime_.is_visible(entry.symbol->name)) continue;
            // Tags are freeform search labels per the spec: they join the
            // searchable text so `net` finds the tag as well as the name.
            std::string label = entry.symbol->meta.label.empty()
                ? entry.symbol->raw_name : entry.symbol->meta.label;
            for (const auto& tag : entry.symbol->meta.tags) {
                label += " [" + tag + "]";
            }
            items.push_back({i, entry.symbol->name, std::move(label)});
        }
        return items;
    }

    [[nodiscard]] zxconfig::tui::Detail detail(std::size_t id) const override {
        zxconfig::tui::Detail out;
        if (id >= entries_.size() || entries_[id].kind != Entry::Kind::Symbol) return out;
        const auto& symbol = *entries_[id].symbol;

        out.name = symbol.name;
        out.label = symbol.meta.label;
        out.help = symbol.meta.help;
        out.type = zxconfig::to_string(symbol.type);
        if (symbol.computed) out.type += " (computed)";

        const auto* value = runtime_.value(symbol.name);
        out.value = value != nullptr ? zxconfig::render_value(*value, symbol.type) : "<unset>";
        const auto* fallback = runtime_.default_value(symbol.name);
        out.default_text = fallback != nullptr
            ? zxconfig::render_value(*fallback, symbol.type) : "<unset>";
        if (symbol.meta.range.has_value()) {
            out.range = std::to_string(symbol.meta.range->lo) + " .. "
                      + std::to_string(symbol.meta.range->hi);
        }
        for (std::size_t i = 0; i < symbol.meta.choices.size(); ++i) {
            if (i != 0) out.choices += ",";
            out.choices += symbol.meta.choices[i];
        }
        const auto where = arena_.resolve(symbol.span.begin);
        if (where.is_valid()) {
            // A macro-expanded symbol lives in a synthetic source named for
            // its call.  Say so instead of showing an unknown file.
            const auto& path = where.file->path;
            const auto marker = path.find("#macro ");
            if (marker != std::string::npos) {
                out.source = "expanded by " + path.substr(marker + 7)
                           + " in " + path.substr(0, marker);
            } else {
                out.source = path + ":" + std::to_string(where.line);
            }
            // Prepend the include chain: a symbol from an included fragment
            // must show which manifest pulled that fragment in.
            std::string chain;
            const auto* file = where.file;
            while (file != nullptr) {
                const auto name = file->path.substr(0, file->path.find("#macro "));
                chain = chain.empty() ? name : name + " -> " + chain;
                file = file->includer != 0 ? arena_.find(file->includer) : nullptr;
            }
            if (chain.find(" -> ") != std::string::npos) {
                out.source = chain + ": " + out.source;
            }
        }

        std::uint8_t flags = 0;
        if (runtime_.is_user_set(symbol.name)) flags |= zxconfig::tui::FLAG_MODIFIED;
        if (!runtime_.is_enabled(symbol.name)) flags |= zxconfig::tui::FLAG_LOCKED;
        {
            std::scoped_lock lock{findings_mutex_};
            if (warned_symbols_.contains(symbol.name)) {
                flags |= zxconfig::tui::FLAG_WARNING;
            }
        }
        out.flags = flags;

        std::string prefix = "CONFIG_";
        for (const auto& directive : document_.generates) {
            if (directive.backend == zxconfig::GenerateDirective::Backend::Header
                && !directive.prefix.empty()) {
                prefix = directive.prefix;
                break;
            }
        }
        out.exported = zxconfig::macro_name(symbol, prefix);
        return out;
    }

    [[nodiscard]] std::vector<zxconfig::tui::LogLine> log_tail(
        std::size_t max_lines) const override {
        std::scoped_lock lock{log_mutex_};
        const auto begin = log_.size() > max_lines ? log_.size() - max_lines : 0;
        return {log_.begin() + static_cast<std::ptrdiff_t>(begin), log_.end()};
    }

    [[nodiscard]] std::vector<std::string> validation_report() const override {
        std::scoped_lock lock{findings_mutex_};
        std::vector<std::string> lines;
        lines.reserve(findings_.size());
        for (const auto& finding : findings_) {
            const auto where = arena_.resolve(finding.span.begin);
            const std::string position = where.is_valid()
                ? where.file->path + ":" + std::to_string(where.line) : "";
            lines.push_back(std::string{zxconfig::to_string(finding.severity)}
                            + "  " + finding.message
                            + (position.empty() ? "" : "  (" + position + ")"));
        }
        return lines;
    }

    [[nodiscard]] std::string toggle(std::size_t id) override {
        if (id >= entries_.size() || entries_[id].kind != Entry::Kind::Symbol) {
            return "not a symbol";
        }
        const auto& symbol = *entries_[id].symbol;
        if (symbol.computed) return "'" + symbol.name + "' is computed and read-only";
        if (!runtime_.is_enabled(symbol.name)) {
            return "'" + symbol.name + "' is disabled by its declaration";
        }

        std::string text;
        if (symbol.type == zxconfig::ValueType::Bool) {
            const auto* value = runtime_.value(symbol.name);
            const bool current = value != nullptr && zxconfig::truth_level(*value) != 0;
            text = current ? "false" : "true";
        } else if (symbol.type == zxconfig::ValueType::Tristate) {
            const auto* value = runtime_.value(symbol.name);
            const auto level = value != nullptr ? zxconfig::truth_level(*value) : 0;
            static constexpr const char* k_next[]{"m", "y", "n"};
            text = k_next[static_cast<std::size_t>(level) % 3];
        } else {
            return "'" + symbol.name + "' has no toggle";
        }

        auto applied = runtime_.set_user_value(symbol.name, text);
        if (!applied) return applied.error().message;
        after_change();
        zxconfig::log::info("set {} = {}", symbol.name, text);
        return {};
    }

    [[nodiscard]] std::string set_text(std::size_t id, std::string text) override {
        if (id >= entries_.size() || entries_[id].kind != Entry::Kind::Symbol) {
            return "not a symbol";
        }
        const auto& symbol = *entries_[id].symbol;
        if (symbol.computed) return "'" + symbol.name + "' is computed and read-only";
        if (!runtime_.is_enabled(symbol.name)) {
            return "'" + symbol.name + "' is disabled by its declaration";
        }
        auto applied = runtime_.set_user_value(symbol.name, text);
        if (!applied) return applied.error().message;
        after_change();
        zxconfig::log::info("set {} = {}", symbol.name, text);
        return {};
    }

    [[nodiscard]] std::string set_choice(std::size_t id, std::string choice) override {
        if (id >= entries_.size() || entries_[id].kind != Entry::Kind::Symbol) {
            return "not a symbol";
        }
        const auto& symbol = *entries_[id].symbol;
        if (symbol.computed) return "'" + symbol.name + "' is computed and read-only";
        if (!runtime_.is_enabled(symbol.name)) {
            return "'" + symbol.name + "' is disabled by its declaration";
        }
        auto applied = runtime_.set_user_value(symbol.name, choice);
        if (!applied) return applied.error().message;
        after_change();
        zxconfig::log::info("set {} = {}", symbol.name, choice);
        return {};
    }

    [[nodiscard]] std::string reset(std::size_t id) override {
        if (id >= entries_.size() || entries_[id].kind != Entry::Kind::Symbol) {
            return "not a symbol";
        }
        const auto& symbol = *entries_[id].symbol;
        auto done = runtime_.reset_symbol(symbol.name);
        if (!done) return done.error().message;
        after_change();
        zxconfig::log::info("reset {}", symbol.name);
        return {};
    }

    [[nodiscard]] std::string save() override {
        // The spec: validate errors are hard errors and the configuration
        // cannot be saved while they stand.
        if (error_count() > 0) {
            return "validation errors block saving — press v for the report";
        }

        // Conflict policy: an implication gated by `conflict X { error ... }`
        // blocks the save; a prompt-policy one is reported and continues.
        const auto implications = runtime_.unsatisfied_implications();
        std::string blocked;
        for (const auto& implication : implications) {
            if (implication.blocks_save) {
                if (!blocked.empty()) blocked += "; ";
                blocked += implication.message;
            } else {
                zxconfig::log::warn("unresolved: {}", implication.message);
            }
        }
        if (!blocked.empty()) return "conflict must be resolved: " + blocked;

        auto lock = zxconfig::CacheLock::acquire(cache_path_, 1);
        if (!lock) return lock.error().message;

        auto document = runtime_.to_cache(manifest_, digest_);
        auto stored = zxconfig::store_cache(cache_path_, document);
        if (!stored) return stored.error().message;

        set_state("saved");
        zxconfig::log::info("saved {} assignment(s) to {}",
                            document.assignments.size(), cache_path_.string());

        std::vector<std::string> failures;
        const auto written = zxconfig::run_generators(
            document_, runtime_, zxconfig::k_version,
            std::filesystem::path{manifest_}.parent_path().string(), failures);
        for (const auto& failure : failures) {
            zxconfig::log::error("generate: {}", failure);
        }
        zxconfig::log::info("generated {} output(s), {} failed",
                            written, failures.size());
        return failures.empty() ? std::string{}
                                : std::to_string(failures.size())
                                      + " output(s) failed to generate";
    }

    [[nodiscard]] std::string force_cache() override {
        if (!pending_cache_) return "there is no refused cache to apply";
        std::size_t applied = 0;
        std::size_t dropped = 0;
        for (const auto& assignment : pending_cache_->assignments) {
            if (runtime_.apply_assignment(assignment.symbol, assignment.value,
                                          assignment.type)) {
                ++applied;
            } else {
                ++dropped;
            }
        }
        pending_cache_.reset();
        if (applied > 0) after_change();
        zxconfig::log::info("cache applied: {} assignment(s) kept, {} dropped",
                            applied, dropped);
        return {};
    }

    [[nodiscard]] std::optional<zxconfig::tui::Prompt> pending_prompt() const override {
        std::scoped_lock lock{prompt_mutex_};
        if (open_implications_.empty()) return std::nullopt;
        zxconfig::tui::Prompt prompt;
        prompt.id = prompt_epoch_;
        prompt.message = open_implications_.front().message;
        prompt.source = open_implications_.front().source;
        for (const auto& fix : open_implications_.front().fixes) {
            prompt.options.push_back(fix.description);
        }
        return prompt;
    }

    [[nodiscard]] std::string respond_prompt(std::size_t id, std::size_t option) override {
        zxconfig::Implication front;
        bool valid = false;
        {
            std::scoped_lock lock{prompt_mutex_};
            if (id == prompt_epoch_ && !open_implications_.empty()) {
                front = open_implications_.front();
                valid = true;
            }
        }
        if (!valid) return {};

        if (option >= front.fixes.size()) {
            // Skip: remembered so the suggestion does not immediately return.
            dismissed_.insert(front.source + '\0' + front.message);
            zxconfig::log::info("skipped suggestion: {}", front.message);
        } else {
            const auto& fix = front.fixes[option];
            auto applied = runtime_.set_user_value(fix.symbol, fix.value);
            if (!applied) return applied.error().message;
            zxconfig::log::info("applied suggestion: {}", fix.description);
            after_change();
        }
        refresh_implications();
        return {};
    }

    [[nodiscard]] std::vector<std::string> preset_items() const override {
        std::vector<std::string> out;
        const auto dir = std::filesystem::path{manifest_}.parent_path() / "presets";
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            // Lock files from --save live beside their preset; they are not
            // presets and must never be offered as one.
            if (entry.path().extension() == ".lock") continue;
            out.push_back(entry.path().filename().string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    [[nodiscard]] std::string load_preset(std::string path) override {
        const auto target = zxconfig::resolve_preset(path, manifest_);
        auto preset = zxconfig::load_cache(target);
        if (!preset) return preset.error().message;

        std::vector<zxconfig::Rename> renames;
        for (const auto& entry : document_.deprecated) {
            renames.push_back({entry.old_name, entry.new_name, entry.message});
        }
        for (const auto& report : zxconfig::rename_assignments(*preset, renames)) {
            zxconfig::log::warn("{}", report);
        }

        std::size_t applied = 0;
        std::size_t refused = 0;
        for (const auto& assignment : preset->assignments) {
            if (runtime_.apply_assignment(assignment.symbol, assignment.value,
                                          assignment.type)) {
                ++applied;
            } else {
                ++refused;
                zxconfig::log::warn("preset assignment refused for '{}': {}",
                                    assignment.symbol, "value not accepted");
            }
        }
        if (applied == 0 && refused > 0) return "no assignment could be applied";

        // A preset is not the on-disk cache: the state is now unsaved.
        after_change();
        zxconfig::log::info("preset {}: {} applied, {} refused",
                            target.string(), applied, refused);
        return {};
    }

private:
    struct Entry {
        enum class Kind : std::uint8_t { Menu, Symbol } kind{Kind::Menu};
        std::size_t parent{static_cast<std::size_t>(-1)};
        std::size_t depth{0};
        const zxconfig::Menu* menu{nullptr};
        const zxconfig::Symbol* symbol{nullptr};
    };

    void build_entries() {
        entries_.clear();
        if (document_.root == nullptr) return;
        add_menu(*document_.root, static_cast<std::size_t>(-1), 0);
    }

    void add_menu(const zxconfig::Menu& menu, std::size_t parent, std::size_t depth) {
        Entry entry;
        entry.kind = Entry::Kind::Menu;
        entry.parent = parent;
        entry.depth = depth;
        entry.menu = &menu;
        const auto self = entries_.size();
        entries_.push_back(entry);

        // Presentation order: the spec's `order` overrides declaration order.
        for (const auto index : zxconfig::ordered_children(menu)) {
            const auto& child = menu.children[index];
            if (const auto* symbol = std::get_if<zxconfig::Symbol>(&child)) {
                Entry symbol_entry;
                symbol_entry.kind = Entry::Kind::Symbol;
                symbol_entry.parent = self;
                symbol_entry.depth = depth + 1;
                symbol_entry.symbol = symbol;
                entries_.push_back(symbol_entry);
            } else if (const auto& sub = std::get<zxconfig::MenuPtr>(child)) {
                add_menu(*sub, self, depth + 1);
            }
        }
    }

    [[nodiscard]] zxconfig::tui::Row menu_row(const Entry& entry,
                                              std::size_t index,
                                              bool is_expanded) const {
        zxconfig::tui::Row row;
        row.id = index;
        row.depth = entry.depth - 1;  // the implicit root is not a visible level
        row.kind = zxconfig::tui::Kind::Menu;
        row.expandable = true;
        row.expanded = is_expanded;
        row.name = entry.menu->label;
        row.label = entry.menu->label;
        return row;
    }

    [[nodiscard]] zxconfig::tui::Row symbol_row(const zxconfig::Symbol& symbol,
                                                std::size_t index) const {
        zxconfig::tui::Row row;
        row.id = index;
        row.depth = entries_[index].depth - 1;
        row.name = symbol.name;

        auto kind_of = [](zxconfig::ValueType type) {
            switch (type) {
                case zxconfig::ValueType::Bool:     return zxconfig::tui::Kind::Bool;
                case zxconfig::ValueType::Tristate: return zxconfig::tui::Kind::Tristate;
                case zxconfig::ValueType::Int:      return zxconfig::tui::Kind::Int;
                case zxconfig::ValueType::Uint:     return zxconfig::tui::Kind::Uint;
                case zxconfig::ValueType::Hex:      return zxconfig::tui::Kind::Hex;
                case zxconfig::ValueType::String:   return zxconfig::tui::Kind::String;
                case zxconfig::ValueType::Path:     return zxconfig::tui::Kind::Path;
                case zxconfig::ValueType::Enum:     return zxconfig::tui::Kind::Enum;
                case zxconfig::ValueType::Set:      return zxconfig::tui::Kind::Set;
                case zxconfig::ValueType::List:     return zxconfig::tui::Kind::List;
            }
            return zxconfig::tui::Kind::String;
        };
        row.kind = symbol.computed ? zxconfig::tui::Kind::Computed : kind_of(symbol.type);

        row.label = symbol.meta.label.empty() ? symbol.name : symbol.meta.label;
        const auto* value = runtime_.value(symbol.name);
        row.value = value != nullptr
            ? zxconfig::render_value(*value, symbol.type) : "<unset>";
        if (row.value.size() > 32) row.value = row.value.substr(0, 29) + "...";

        if (symbol.type == zxconfig::ValueType::Tristate
            || symbol.type == zxconfig::ValueType::Bool) {
            row.tag = row.value;
        } else if (symbol.computed) {
            row.tag = "RO";
        }

        if (runtime_.is_user_set(symbol.name)) row.flags |= zxconfig::tui::FLAG_MODIFIED;
        if (!runtime_.is_enabled(symbol.name)) row.flags |= zxconfig::tui::FLAG_LOCKED;
        {
            std::scoped_lock lock{findings_mutex_};
            if (warned_symbols_.contains(symbol.name)) {
                row.flags |= zxconfig::tui::FLAG_WARNING;
            }
        }
        return row;
    }

    void after_change() {
        revalidate();
        refresh_implications();
        set_state("MODIFIED");
    }

    /// @brief Recompute the open suggestions.  Every answer changes values,
    ///        which can open or close others; the epoch lets the interface
    ///        detect a reply that raced a refresh.
    void refresh_implications() {
        auto open = runtime_.unsatisfied_implications();
        std::erase_if(open, [this](const zxconfig::Implication& item) {
            return dismissed_.contains(item.source + '\0' + item.message);
        });
        std::scoped_lock lock{prompt_mutex_};
        open_implications_ = std::move(open);
        ++prompt_epoch_;
    }

    void revalidate() {
        auto findings = runtime_.run_validation();
        std::size_t errors = 0;
        std::size_t warnings = 0;
        std::unordered_set<std::string> warned;
        for (const auto& finding : findings) {
            if (finding.severity == zxconfig::Severity::Warning) {
                ++warnings;
                // Metadata warnings carry their symbol's span, so they can be
                // flagged on the row instead of only counted in the header.
                for (const auto& [name, symbol] : document_.symbols) {
                    if (symbol->span.begin == finding.span.begin) warned.insert(name);
                }
            } else {
                ++errors;
            }
        }
        std::scoped_lock lock{findings_mutex_};
        findings_ = std::move(findings);
        errors_ = errors;
        warnings_ = warnings;
        warned_symbols_ = std::move(warned);
    }

    void set_state(std::string state) {
        if (state_ == state) return;
        zxconfig::log::info("state: {} -> {}", state_.empty() ? "init" : state_, state);
        state_ = std::move(state);
    }

    const zxconfig::Document& document_;
    zxconfig::Runtime& runtime_;
    const zxconfig::SourceArena& arena_;
    std::string manifest_;
    std::string digest_;
    std::filesystem::path cache_path_;
    std::optional<zxconfig::CacheDocument> pending_cache_;

    std::vector<Entry> entries_;
    std::string state_{"init"};

    mutable std::mutex findings_mutex_;
    std::vector<zxconfig::Diagnostic> findings_;
    std::unordered_set<std::string> warned_symbols_;
    std::size_t errors_{0};
    std::size_t warnings_{0};

    mutable std::mutex prompt_mutex_;
    std::vector<zxconfig::Implication> open_implications_;
    std::size_t prompt_epoch_{0};
    std::unordered_set<std::string> dismissed_;

    mutable std::mutex log_mutex_;
    std::vector<zxconfig::tui::LogLine> log_;
};

zxconfig::log::Level level_for(zxconfig::Severity severity) noexcept {
    switch (severity) {
        case zxconfig::Severity::Warning: return zxconfig::log::Level::Warn;
        default: return zxconfig::log::Level::Error;
    }
}

}  // namespace

int main(int argc, char** argv) {
    auto inv = parse_args(argc, argv);
    if (!inv) {
        zxconfig::log::emit(inv.error());
        return 2;
    }

    auto& log = zxconfig::log::logger();
    log.set_level(inv->level);
    if (inv->color == 1) log.set_color(true);
    if (inv->color == 0) log.set_color(false);
    if (!inv->log_path.empty()) {
        if (auto opened = log.use_file(inv->log_path); !opened) {
            zxconfig::log::emit(opened.error());
            return 2;
        }
    }

    zxconfig::SourceArena arena;
    // Includes are confined to the manifest's directory unless --root says
    // otherwise — the documented default, enforced.
    std::string root = inv->root;
    if (root.empty()) {
        const auto parent = std::filesystem::path{inv->file}.parent_path();
        root = parent.empty() ? "." : parent.string();
    }
    auto loaded = arena.load(inv->file, {}, root);
    if (!loaded) {
        zxconfig::log::emit(loaded.error(), &arena);
        return 1;
    }

    if (inv->dump_tokens) {
        auto lexed = zxconfig::lex(arena, *loaded);
        for (const auto& token : lexed.tokens) {
            const auto where = arena.resolve(token.span.begin);
            std::print("{}:{}:{} {}\n",
                       where.file->path, where.line, where.column,
                       zxconfig::to_string(token.kind));
        }
        return lexed.ok() ? 0 : 1;
    }

    auto parsed = zxconfig::parse_document(arena, *loaded, root);
    for (const auto& diagnostic : parsed.diagnostics) {
        zxconfig::log::emit(diagnostic, &arena);
    }

    // The parse may have loaded includes, which can reallocate the arena's
    // file vector; resolve the root file only now.
    const auto* file = arena.find(*loaded);

    const auto stats = zxconfig::document_stats(parsed.document);
    if (!parsed.ok()) {
        zxconfig::log::error("parsing failed: {} problem(s) reported",
                             parsed.diagnostics.size());
        return 1;
    }
    zxconfig::log::info("parsed format {}.{}: {} symbol(s), {} menu(s), {} include(s)",
                        parsed.document.version_major, parsed.document.version_minor,
                        stats.symbols, stats.menus, parsed.document.includes.size());

    const std::string manifest_name =
        file->canonical.empty() ? file->path : file->canonical;
    // The cache describes the complete manifest graph.  Hash every source in
    // arena load order, including included fragments, so editing an include
    // invalidates the cache just like editing the root manifest does.
    std::vector<std::string_view> digest_parts;
    digest_parts.reserve(static_cast<std::size_t>(arena.count()) * 2);
    for (const auto& source : arena.files()) {
        digest_parts.push_back(source.canonical.empty() ? source.path : source.canonical);
        digest_parts.push_back(source.text);
    }
    const auto digest = zxconfig::digest_manifest(digest_parts);
    const auto cache_path = zxconfig::default_cache_path(manifest_name);

    std::vector<std::string_view> declared;
    declared.reserve(parsed.document.symbols.size() + parsed.document.aliases.size());
    for (const auto& name : parsed.document.symbols | std::views::keys) {
        declared.push_back(name);
    }
    // An alias names its target: a cache written under the alias must reach
    // reconciliation instead of being dropped as unknown.
    for (const auto& alias : parsed.document.aliases) {
        declared.push_back(alias.alias);
    }

    auto cached = zxconfig::load_cache(cache_path, digest);

    // Deprecated names: warn and rewrite, as the spec requires of tools.
    std::vector<zxconfig::Rename> renames;
    renames.reserve(parsed.document.deprecated.size());
    for (const auto& entry : parsed.document.deprecated) {
        renames.push_back({entry.old_name, entry.new_name, entry.message});
    }
    if (cached) {
        for (const auto& report : zxconfig::rename_assignments(*cached, renames)) {
            zxconfig::log::warn("{}", report);
        }
    }

    // A cache that failed the digest gate is not in `cached`; a forced apply
    // re-reads it without that gate, migrating deprecated names on the way.
    // Dereferencing the empty `cached` here instead was undefined behavior.
    std::optional<zxconfig::CacheDocument> rescued;
    const auto rescue = [&]() -> zxconfig::CacheDocument* {
        if (rescued.has_value()) return &*rescued;
        if (auto raw = zxconfig::load_cache(cache_path); raw) {
            for (const auto& report : zxconfig::rename_assignments(*raw, renames)) {
                zxconfig::log::warn("{}", report);
            }
            rescued = std::move(*raw);
            return &*rescued;
        }
        return nullptr;
    };

    // The document reconciliation reasons about: a clean cache as loaded, or
    // the rescued one when the digest gate refused it.
    const zxconfig::CacheDocument* effective = cached ? &*cached : rescue();

    zxconfig::CacheState state = zxconfig::CacheState::Clean;
    if (!cached) {
        switch (cached.error().code) {
            case zxconfig::ErrorCode::CacheMissing:
                state = zxconfig::CacheState::Absent;
                zxconfig::log::debug("no cache at {}; defaults apply", cache_path.string());
                break;
            case zxconfig::ErrorCode::CacheStale:
                state = zxconfig::CacheState::Stale;
                break;
            default:
                state = zxconfig::CacheState::Broken;
                break;
        }
    }

    static const zxconfig::CacheDocument empty_document;
    const auto verdict = zxconfig::reconcile(
        state, effective != nullptr ? *effective : empty_document,
        declared, inv->cache_force);

    switch (verdict.disposition) {
        case zxconfig::CacheDisposition::Apply:
            zxconfig::log::info("cache clean: {} assignment(s) apply", verdict.kept.size());
            break;
        case zxconfig::CacheDisposition::ApplyPartial:
            zxconfig::log::warn("cache {}: keeping {} assignment(s), dropping {}, "
                                "{} unknown symbol(s)",
                                zxconfig::to_string(state),
                                verdict.kept.size(), verdict.dropped.size(),
                                verdict.unknown.size());
            break;
        case zxconfig::CacheDisposition::Reset:
            zxconfig::log::debug("starting from defaults");
            break;
        case zxconfig::CacheDisposition::Refuse:
            if (state == zxconfig::CacheState::Stale) {
                zxconfig::log::warn("cache was not applied ({}); press c to apply the "
                                    "assignments that still match, or rerun with "
                                    "--cache-force", cached.error().message);
            } else {
                zxconfig::log::warn("cache could not be read ({}); starting from defaults",
                                    cached.error().message);
            }
            break;
    }

    std::vector<zxconfig::Diagnostic> evaluation_notes;
    zxconfig::Runtime runtime{parsed.document, &evaluation_notes};

    if (verdict.disposition == zxconfig::CacheDisposition::Apply
        || verdict.disposition == zxconfig::CacheDisposition::ApplyPartial) {
        if (effective != nullptr) {
            std::size_t applied = 0;
            std::size_t refused = 0;
            for (const auto& assignment : effective->assignments) {
                const bool kept = std::find(verdict.kept.begin(), verdict.kept.end(),
                                            assignment.symbol) != verdict.kept.end();
                if (!kept) continue;
                if (auto result = runtime.apply_assignment(
                        assignment.symbol, assignment.value, assignment.type)) {
                    ++applied;
                } else {
                    ++refused;
                    zxconfig::Diagnostic warning = result.error();
                    warning.severity = zxconfig::Severity::Warning;
                    warning.message = "cache assignment refused for '"
                        + assignment.symbol + "': " + result.error().message;
                    zxconfig::log::emit(warning, &arena);
                }
            }
            if (refused > 0) {
                zxconfig::log::warn("cache: {} assignment(s) applied, {} refused",
                                    applied, refused);
            }
        }
    }

    if (auto evaluated = runtime.evaluate(); !evaluated) {
        zxconfig::log::emit(evaluated.error(), &arena);
        return 1;
    }
    // Presets and overrides layer on the reconciled cache in the order given
    // on the command line, exactly like editing steps: later inputs win.
    for (const auto& input : inv->inputs) {
        switch (input.kind) {
            case Invocation::Input::Kind::Preset: {
                const auto target = zxconfig::resolve_preset(input.name, manifest_name);
                auto preset = zxconfig::load_cache(target);
                if (!preset) {
                    zxconfig::log::warn("preset {}: {}", target.string(),
                                        preset.error().message);
                    continue;
                }
                for (const auto& report : zxconfig::rename_assignments(*preset, renames)) {
                    zxconfig::log::warn("{}", report);
                }
                for (const auto& assignment : preset->assignments) {
                    if (auto applied = runtime.apply_assignment(
                            assignment.symbol, assignment.value, assignment.type)) {
                        zxconfig::log::info("preset: {} = {}", assignment.symbol,
                                            assignment.value);
                    } else {
                        zxconfig::log::warn("preset {}: {}", input.name,
                                            applied.error().message);
                    }
                }
                break;
            }
            case Invocation::Input::Kind::Set:
                if (auto applied = runtime.set_user_value(input.name, input.value)) {
                    zxconfig::log::info("set {} = {}", input.name, input.value);
                } else {
                    zxconfig::log::error("cannot set {}: {}", input.name,
                                         applied.error().message);
                    return 1;
                }
                break;
            case Invocation::Input::Kind::Unset:
                if (auto done = runtime.reset_symbol(input.name)) {
                    zxconfig::log::info("reset {}", input.name);
                } else {
                    zxconfig::log::error("cannot reset {}: {}", input.name,
                                         done.error().message);
                    return 1;
                }
                break;
        }
    }

    for (const auto& note : evaluation_notes) zxconfig::log::emit(note, &arena);

    // Validation runs over the final values: presets and overrides may well
    // have cleared what defaults violated.
    for (const auto& finding : runtime.run_validation()) {
        zxconfig::log::record(level_for(finding.severity), "{}", finding.message);
    }
    for (const auto& implication : runtime.unsatisfied_implications()) {
        zxconfig::log::warn("unresolved: {}", implication.message);
    }

    // Validation errors block writing, as the spec requires; --force is the
    // deliberate override for regenerating outputs anyway.
    bool validation_errors = false;
    for (const auto& finding : runtime.run_validation()) {
        if (finding.severity != zxconfig::Severity::Warning) validation_errors = true;
    }
    if (validation_errors && (inv->generate || inv->save) && !inv->force) {
        zxconfig::log::error("validation errors block --generate and --save; "
                             "fix them or pass --force");
        return 1;
    }

    if (inv->save) {
        auto document = runtime.to_cache(manifest_name, digest);
        const auto target = inv->save_path.empty()
            ? cache_path : std::filesystem::path{inv->save_path};
        auto lock = zxconfig::CacheLock::acquire(target, 2);
        if (!lock) {
            zxconfig::log::error("{}", lock.error().message);
            return 1;
        }
        if (auto stored = zxconfig::store_cache(target, document)) {
            zxconfig::log::info("saved {} assignment(s) to {}",
                                document.assignments.size(), target.string());
        } else {
            zxconfig::log::error("{}", stored.error().message);
            return 1;
        }
    }

    if (inv->generate) {
        std::vector<std::string> failures;
        const auto written = zxconfig::run_generators(
            parsed.document, runtime, zxconfig::k_version,
            std::filesystem::path{manifest_name}.parent_path().string(), failures);
        for (const auto& failure : failures) {
            zxconfig::log::error("generate: {}", failure);
        }
        zxconfig::log::info("generated {} output(s), {} failed",
                            written, failures.size());
        return failures.empty() ? 0 : 1;
    }
    if (inv->parse_only) {
        std::println("ok: {} symbol(s), {} menu(s)", stats.symbols, stats.menus);
        return 0;
    }
    if (inv->headless) {
        zxconfig::log::info("headless: nothing further to do");
        return 0;
    }

    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        zxconfig::log::warn("stdin or stdout is not a terminal; the interface "
                            "is disabled (use --headless to silence this)");
        return 0;
    }

    // A refused cache is kept aside for the interface: the TUI's `c` action
    // applies it on request.  It is re-read without the digest gate, which is
    // exactly the gate that refused it.
    std::optional<zxconfig::CacheDocument> pending_cache;
    if (verdict.disposition == zxconfig::CacheDisposition::Refuse
        && rescued.has_value()) {
        pending_cache = std::move(*rescued);
    }

    Session session{parsed.document, runtime, arena, manifest_name, digest,
                    cache_path, std::move(pending_cache)};
    const auto code = zxconfig::tui::run(session);
    if (code == 4) {
        zxconfig::log::error("the interface failed internally; the terminal has been restored");
    }
    return code;
}
