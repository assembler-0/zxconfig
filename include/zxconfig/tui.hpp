// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zxconfig::tui {

/// @brief One renderable line of the tree panel.
enum class Kind : std::uint8_t {
    Menu,
    Bool,
    Tristate,
    Int,
    Uint,
    Hex,
    String,
    Path,
    Enum,
    Set,
    List,
    Computed,
};

/// @brief Row state bits.  Rendered as text markers, never icons.
enum Flag : std::uint8_t {
    FLAG_MODIFIED = 1 << 0,  ///< value differs from the declaration's default
    FLAG_LOCKED   = 1 << 1,  ///< not editable (disabled or computed)
    FLAG_INVALID  = 1 << 2,  ///< last edit was refused
    FLAG_WARNING  = 1 << 3,  ///< a warn clause fires for this symbol
};

struct Row {
    std::size_t id{0};          ///< stable identity for expansion and actions
    std::size_t depth{0};       ///< 0 at the root's children
    Kind kind{Kind::Menu};
    std::uint8_t flags{0};
    bool expandable{false};
    bool expanded{false};
    std::string name;           ///< symbol name, or the menu label
    std::string label;          ///< display label; falls back to name
    std::string value;          ///< rendered value; menus leave it empty
    std::string tag;            ///< state text, e.g. "y", "m", "<unset>"
};

/// @brief One entry of the search index.
struct SearchItem {
    std::size_t id{0};
    std::string name;
    std::string label;
};

/// @brief Everything the detail popup shows about one symbol.
struct Detail {
    std::string name;
    std::string label;
    std::string help;
    std::string type;
    std::string value;
    std::string default_text;
    std::string range;        ///< "1 .. 512" or empty
    std::string choices;      ///< comma-joined or empty
    std::string source;       ///< "file:line" or empty
    std::string exported;     ///< flat declaration spelling in generated outputs
    std::uint8_t flags{0};
};

/// @brief One line of the log panel.
struct LogLine {
    std::string level;   ///< "info", "warn", "error", "state"
    std::string text;
};

/// @brief A soft suggestion awaiting a decision (an unsatisfied `implies`).
struct Prompt {
    std::size_t id{0};                 ///< generation; guards stale replies
    std::string source;                ///< symbol whose implies is open
    std::string message;               ///< the question to show
    std::vector<std::string> options;  ///< fix descriptions; empty = acknowledge
};

/// @brief The seam between the engine and the interface.
class Controller {
public:
    virtual ~Controller() = default;

    /// @brief Header text: project or manifest name.
    [[nodiscard]] virtual std::string title() const = 0;
    /// @brief Current run state, e.g. READY, MODIFIED, SAVED.
    [[nodiscard]] virtual std::string state_name() const = 0;
    /// @brief Validation error count.
    [[nodiscard]] virtual std::size_t error_count() const = 0;
    /// @brief Validation warning count.
    [[nodiscard]] virtual std::size_t warning_count() const = 0;
    /// @brief How many symbols carry a user value.
    [[nodiscard]] virtual std::size_t modified_count() const = 0;

    /// @brief Visible rows in display order, honoring @p expanded menus.
    /// @param[in] expanded  Ids of menus currently expanded.
    [[nodiscard]] virtual std::vector<Row> rows(
        const std::vector<std::size_t>& expanded) const = 0;

    /// @brief Ancestor menu ids of @p id, root first.  Used to reveal a
    ///        search hit.
    [[nodiscard]] virtual std::vector<std::size_t> menu_chain(std::size_t id) const = 0;

    /// @brief The searchable index: every visible symbol.
    [[nodiscard]] virtual std::vector<SearchItem> search_items() const = 0;

    /// @brief Detail data for the F2 popup.
    [[nodiscard]] virtual Detail detail(std::size_t id) const = 0;

    /// @brief The last @p max_lines records, oldest first.
    [[nodiscard]] virtual std::vector<LogLine> log_tail(std::size_t max_lines) const = 0;

    /// @brief The validation report, one line per finding, empty when clean.
    ///        Lines are pre-rendered: severity, message, and location.
    [[nodiscard]] virtual std::vector<std::string> validation_report() const = 0;

    /// @brief All actions return an empty string on success, or the refusal
    ///        reason the interface shows in a message popup.

    /// @brief Flip a boolean or cycle a tristate.
    [[nodiscard]] virtual std::string toggle(std::size_t id) = 0;
    /// @brief Set a text-valued symbol.
    [[nodiscard]] virtual std::string set_text(std::size_t id, std::string text) = 0;
    /// @brief Choose one enum choice or replace a set.
    [[nodiscard]] virtual std::string set_choice(std::size_t id, std::string choice) = 0;
    /// @brief Return a symbol to its default.
    [[nodiscard]] virtual std::string reset(std::size_t id) = 0;
    /// @brief Write the cache.  The refusal text is the failure message.
    [[nodiscard]] virtual std::string save() = 0;
    /// @brief Apply a cache that was refused at startup (stale or broken),
    ///        keeping the assignments whose symbols still exist.  An empty
    ///        string is success; otherwise the reason.
    [[nodiscard]] virtual std::string force_cache() = 0;

    /// @brief The suggestion awaiting a decision, if any.
    [[nodiscard]] virtual std::optional<Prompt> pending_prompt() const = 0;

    /// @brief Answer the pending prompt.  @p option indexes the prompt's
    ///        options; pass the count to skip.  A stale @p id (the prompt was
    ///        replaced) is ignored.  An empty string is success.
    [[nodiscard]] virtual std::string respond_prompt(std::size_t id, std::size_t option) = 0;

    /// @brief Preset files found in the manifest's `presets/` directory,
    ///        sorted.  The suggestion list for the load-preset dialog.
    [[nodiscard]] virtual std::vector<std::string> preset_items() const = 0;

    /// @brief Apply the preset at @p path over the current values.  Preset
    ///        assignments win on overlap, as with the command line.  A
    ///        relative path resolves against the manifest's directory.  An
    ///        empty string is success; otherwise the reason.
    [[nodiscard]] virtual std::string load_preset(std::string path) = 0;
};

/// @brief Run the full-screen interface until the user quits.
/// @retval 0  normal quit
/// @retval 3  stdin or stdout is not a terminal
/// @retval 4  the interface failed internally; the terminal is restored
int run(Controller& controller);

}  // namespace zxconfig::tui
