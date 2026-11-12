// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/fuzzy.hpp>
#include <zxconfig/tui.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace zxconfig::tui {
namespace {

using namespace ftxui;

constexpr int k_log_lines = 6;
constexpr int k_view_top = 3;
constexpr int k_chrome_rows = 1 + 3 + (k_log_lines + 2) + 1;

constexpr int k_name_width = 52;
constexpr int k_type_width = 11;
constexpr int k_value_width = 30;
constexpr int k_details_width = 46;

constexpr std::size_t k_log_history = 200;

/// @brief A counter that is dim when zero and a lit chip when not.
Element emphasize(std::size_t count) {
    if (count == 0) return text("0") | dim;
    return text(" " + std::to_string(count) + " ") | bold | inverted;
}

Element flag_markers(std::uint8_t flags) {
    std::string markers;
    if ((flags & FLAG_MODIFIED) != 0) markers += " [M]";
    if ((flags & FLAG_LOCKED) != 0) markers += " [L]";
    if ((flags & FLAG_WARNING) != 0) markers += " [W]";
    return markers.empty() ? text(std::string{}) : text(markers) | dim;
}

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::Menu:     return "menu";
        case Kind::Bool:     return "bool";
        case Kind::Tristate: return "tristate";
        case Kind::Int:      return "int";
        case Kind::Uint:     return "uint";
        case Kind::Hex:      return "hex";
        case Kind::String:   return "string";
        case Kind::Path:     return "path";
        case Kind::Enum:     return "enum";
        case Kind::Set:      return "set";
        case Kind::List:     return "list";
        case Kind::Computed: return "computed";
    }
    return "?";
}

std::string clip(std::string text, std::size_t width) {
    if (text.size() <= width) return text;
    return text.substr(0, width - 1) + "…";
}

/// @brief Wrap @p text to @p width columns, greedily by spaces.
std::vector<std::string> wrap_lines(std::string_view text, std::size_t width) {
    std::vector<std::string> lines;
    if (width == 0) return lines;
    std::size_t at = 0;
    while (at < text.size()) {
        auto end = std::min(text.size(), at + width);
        if (end < text.size()) {
            const auto space = text.rfind(' ', end);
            if (space != std::string_view::npos && space > at) end = space;
        }
        lines.emplace_back(text.substr(at, end - at));
        at = end;
        while (at < text.size() && text[at] == ' ') ++at;
    }
    return lines;
}

/// @brief Split a comma-separated list, trimming each part.
std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto comma = text.find(',', start);
        auto part = text.substr(start, comma == std::string_view::npos
                                          ? std::string_view::npos : comma - start);
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
            part.remove_prefix(1);
        }
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
            part.remove_suffix(1);
        }
        items.emplace_back(part);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return items;
}

class TuiApp {
public:
    explicit TuiApp(Controller& controller) : controller_{controller} {}

    int run() {
        if (::isatty(STDIN_FILENO) != 1 || ::isatty(STDOUT_FILENO) != 1) return 3;

        auto screen = ScreenInteractive::Fullscreen();
        screen_ = &screen;
        quit_ = screen.ExitLoopClosure();

        static const sigset_t k_watched = [] {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGTERM);
            return set;
        }();
        sigset_t previous_mask{};
        ::pthread_sigmask(SIG_BLOCK, &k_watched, &previous_mask);

        std::atomic_bool stopping{false};
        std::thread watcher([&screen, &stopping] {
            int delivered = 0;
            if (::sigwait(&k_watched, &delivered) == 0) {
                if (!stopping.load(std::memory_order_acquire)) {
                    ScreenInteractive::PostEventOrExecute([&screen] { screen.Exit(); });
                }
            }
        });

        auto view = Renderer([this] { return render_all(); });
        root_ = CatchEvent(view, [this](const Event& event) { return handle(event); });

        int code = 0;
        try {
            screen.Loop(root_);
        } catch (const std::exception&) {
            code = 4;
        } catch (...) {
        }

        // sigwait has no cancellation primitive.  Wake it with the same
        // blocked signal, then join while the stack screen is still alive.
        stopping.store(true, std::memory_order_release);
        ::pthread_kill(watcher.native_handle(), SIGTERM);
        watcher.join();
        ::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
        return code;
    }

private:
    /// @brief Marquee for the selected entry: a billboard scroll, left to
    ///        right, when the text overflows its column.  Unselected entries
    ///        stay still and clip.  The pace is clock-driven — one cell per
    ///        120 ms with a pause at the far end — so it stays readable and
    ///        independent of how fast frames arrive.  Sets the repaint flag
    ///        so the loop keeps painting while anything moves.
    std::string marquee(const std::string& text, std::size_t width) {
        if (text.size() <= width) return text;
        needs_repaint_ = true;
        const auto span = text.size() - width;
        const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - marquee_clock_).count() / 120;
        const auto cycle = span + 10;  // ten cells of hold at the far end
        const auto offset = static_cast<std::size_t>(ticks) % cycle;
        if (offset > span) return text.substr(span);
        return text.substr(offset, width);
    }

    Element render_all() {
        needs_repaint_ = false;
        Element main = render_main();
        if (popup_ == nullptr) {
            if (needs_repaint_) screen_->RequestAnimationFrame();
            return main;
        }
        // The popup window paints its own footprint black (see popup_frame);
        // the tree stays visible around it.  No screen-wide wipe.
        Element frame = dbox({std::move(main), popup_->Render()});
        if (needs_repaint_) screen_->RequestAnimationFrame();
        return frame;
    }

    Element render_main() {
        rows_ = controller_.rows(expanded_);

        if (pending_jump_) {
            pending_jump_ = false;
            for (std::size_t i = 0; i < rows_.size(); ++i) {
                if (rows_[i].id == jump_row_id_) cursor_ = i;
            }
        }
        if (cursor_ >= rows_.size()) cursor_ = rows_.empty() ? 0 : rows_.size() - 1;

        const int screen_rows = Terminal::Size().dimy;
        const bool findings_line =
            controller_.error_count() > 0 || controller_.warning_count() > 0;
        body_h_ = std::max(3, screen_rows - k_chrome_rows - (findings_line ? 1 : 0));

        if (cursor_ < view_begin_) view_begin_ = cursor_;
        if (cursor_ >= view_begin_ + static_cast<std::size_t>(body_h_)) {
            view_begin_ = cursor_ - static_cast<std::size_t>(body_h_) + 1;
        }

        Elements body;
        for (int line = 0; line < body_h_; ++line) {
            const auto index = view_begin_ + static_cast<std::size_t>(line);
            if (index >= rows_.size()) {
                body.push_back(text(std::string{}));
                continue;
            }
            body.push_back(render_row(index, rows_[index]));
        }

        const int screen_cols = Terminal::Size().dimx;
        // Scrollback: a window over the tail of the history.  [ scrolls
        // back, ] scrolls forward; the title always states the position.
        const auto history = controller_.log_tail(k_log_history);
        const auto max_offset = history.size() > static_cast<std::size_t>(k_log_lines)
            ? history.size() - static_cast<std::size_t>(k_log_lines) : 0;
        log_offset_ = std::min(log_offset_, max_offset);
        const auto end = history.size() - log_offset_;
        const auto begin = end > static_cast<std::size_t>(k_log_lines)
            ? end - static_cast<std::size_t>(k_log_lines) : 0;

        Elements log_lines;
        for (std::size_t i = begin; i < end; ++i) {
            const auto& line = history[i];
            std::string level = line.level;
            level.resize(5, ' ');
            Element entry = hbox({
                text(level) | dim,
                text(clip(line.text, static_cast<std::size_t>(
                                        std::max(10, screen_cols - 12)))),
            });
            if (line.level == "error") entry |= bold;
            log_lines.push_back(std::move(entry));
        }
        while (log_lines.size() < static_cast<std::size_t>(k_log_lines)) {
            log_lines.push_back(text(std::string{}));
        }

        auto columns = hbox({
            text("  name") | size(WIDTH, EQUAL, k_name_width),
            text("type") | size(WIDTH, EQUAL, k_type_width),
            text("value"),
        }) | dim;

        auto list_window = window(
            text(" configuration "),
            vbox({columns, vbox(std::move(body))}));

        auto details_window = window(
            text(" details "), render_description());

        auto header = hbox({
            text(" " + controller_.title()) | bold,
            filler(),
            text("state " + controller_.state_name()) | bold,
            text("     err "),
            emphasize(controller_.error_count()),
            text("     warn "),
            emphasize(controller_.warning_count()),
            text("     set "),
            text(std::to_string(controller_.modified_count())) | dim,
            text("   "),
        });

        std::string log_title = " log ";
        if (log_offset_ > 0) {
            log_title += "+" + std::to_string(log_offset_) + " back of "
                       + std::to_string(history.size()) + " ";
        }

        Elements layout;
        layout.push_back(header);
        layout.push_back(hbox({
            list_window | flex,
            text(" "),
            details_window | size(WIDTH, EQUAL, k_details_width),
        }));
        // Only when there is a finding: an empty element still occupies a
        // row, which would push the hints line off the screen.
        if (findings_line) layout.push_back(render_finding());
        layout.push_back(window(text(log_title), vbox(std::move(log_lines)))
                         | size(HEIGHT, EQUAL, k_log_lines + 2));
        layout.push_back(render_hints());
        return vbox(std::move(layout));
    }

    /// @brief The bottom line always carries the keys; findings get their
    ///        own loud line above the log instead of replacing the hints.
    Element render_hints() {
        return text(
            " [space] toggle  [enter] edit  [r] reset  [s] save  [c] apply cache"
            "  [p] preset  [/] find  [v] check  [d] details  [?] help"
            "  [ and ] scroll log  [q] quit ") | dim;
    }

    Element render_finding() {
        const auto errors = controller_.error_count();
        const auto warnings = controller_.warning_count();
        if (errors == 0 && warnings == 0) return text(std::string{});
        const auto report = controller_.validation_report();
        std::string first = report.empty() ? "validation failed" : report.front();
        std::string prefix = errors > 0 ? " ERROR " : " WARN ";
        return hbox({
            text(prefix) | bold | inverted,
            text(" " + clip(first, 110)) | bold,
            filler(),
            text(" [v] report ") | dim,
        });
    }

    Element render_row(std::size_t index, const Row& row) {
        std::string name_cell = std::string(row.depth * 2, ' ');
        if (row.expandable) {
            name_cell += row.expanded ? "[-] " : "[+] ";
        } else if (row.kind == Kind::Bool) {
            name_cell += row.value == "true" ? "[*] " : "[ ] ";
        } else if (row.kind == Kind::Tristate) {
            name_cell += "[" + row.value + "] ";
        } else {
            name_cell += "    ";
        }
        name_cell += row.label.empty() ? row.name : row.label;
        name_cell = clip(std::move(name_cell),
                         static_cast<std::size_t>(k_name_width - 1));
        name_cell.resize(static_cast<std::size_t>(k_name_width - 1), ' ');

        std::string value = clip(row.value.empty() ? "" : row.value,
                                 static_cast<std::size_t>(k_value_width));

        Elements line;
        line.push_back(text(" " + name_cell));
        line.push_back(text(kind_name(row.kind)) | dim
                       | size(WIDTH, EQUAL, k_type_width));
        line.push_back(text(value.empty() ? "—" : value)
                       | (value.empty() ? dim : bold));
        line.push_back(flag_markers(row.flags));

        Element element = hbox(std::move(line));
        if ((row.flags & FLAG_LOCKED) != 0) element |= dim;
        if (index == cursor_) element |= inverted;
        return element;
    }

    Element render_description() {
        Elements lines;
        if (rows_.empty() || cursor_ >= rows_.size()) {
            lines.push_back(text(" nothing selected") | dim);
        } else {
            const auto& row = rows_[cursor_];
            lines.push_back(text(clip(row.name, 40)) | bold);
            if (!row.label.empty() && row.label != row.name) {
                lines.push_back(text(clip(row.label, 40)));
            }
            lines.push_back(separator());

            if (row.kind == Kind::Menu) {
                lines.push_back(text("menu") | dim);
            } else {
                const auto data = controller_.detail(row.id);
                std::string help = data.help.empty()
                    ? "no description in the manifest" : data.help;
                lines.push_back(text(clip(help, 42)) | dim);
                lines.push_back(text(std::string{}));
                lines.push_back(fact("type", data.type));
                lines.push_back(fact("value", data.value));
                lines.push_back(fact("default", data.default_text));
                if (!data.range.empty()) lines.push_back(fact("range", data.range));
                if (!data.choices.empty()) lines.push_back(fact("choices", data.choices));
                if (!data.exported.empty()) lines.push_back(fact("exports", data.exported));
                lines.push_back(text(std::string{}));
                lines.push_back(fact("from", data.source));
            }
        }
        return vbox(std::move(lines));
    }

    static Element fact(std::string key, std::string value) {
        key.resize(8, ' ');
        return hbox({
            text(key) | dim,
            text(clip(value, 34)),
        });
    }

    /// @brief Whether the open popup owns a text field, where every
    ///        character — q and j and k included — is input.
    [[nodiscard]] bool popup_takes_text() const {
        return popup_kind_ == Popup::Edit
            || popup_kind_ == Popup::Search
            || popup_kind_ == Popup::Preset;
    }

    /// @brief Visible slice of a hit list: @p rows rows around the selection,
    ///        scrolling when the selection passes the window edge.  A list
    ///        that renders only its head makes every keystroke past it look
    ///        broken.
    static std::pair<std::size_t, std::size_t> list_window(std::size_t total,
                                                           std::size_t selected,
                                                           std::size_t rows) {
        if (total <= rows) return {0, total};
        const auto begin = selected + 2 <= rows ? 0 : selected + 2 - rows;
        return {begin, std::min(total, begin + rows)};
    }

    bool handle(const Event& event) {
        if (popup_ != nullptr) {
            // The local reference is load-bearing: handlers such as apply_text
            // close the popup, and the sole owner resetting popup_ mid-dispatch
            // would destroy the very object whose OnEvent is still running.
            Component popup = popup_;
            if (event == Event::Escape) { close_popup(); return true; }
            // 'q' closes a dialog, but it must reach a text field as a
            // character: half of every query contains a q.
            if (event.input() == "q" && !popup_takes_text()) {
                if (popup_kind_ == Popup::Message && quit_confirmed_) {
                    quit_();
                    return true;
                }
                close_popup();
                return true;
            }
            // Dispatch at the popup's interactive component, not at the
            // Renderer wrapper: a plain Renderer is a view and drops events,
            // which silently killed every key and button in every popup.
            Component events = popup_events_;
            if (events != nullptr && events->OnEvent(event)) {
                // Radiobox consumes Enter to select its focused entry.  It
                // still needs to commit that value, unless a button callback
                // already closed or replaced the popup during dispatch.
                if (popup_ == popup && popup_kind_ == Popup::Choice
                    && event == Event::Return) {
                    commit_popup();
                }
                return true;
            }
            // Enter applies whatever the popup holds when the popup did not
            // consume it itself: choosers and checkboxes commit here.
            if (event == Event::Return) { commit_popup(); return true; }
            return false;
        }
        return handle_mouse(event) || handle_main_key(event) || handle_global(event);
    }

    /// @brief Apply and close, in that order — closing first keeps a refusal
    ///        visible instead of destroying the message popup that shows it.
    void apply_text() {
        const auto refusal = controller_.set_text(edit_id_, edit_content_);
        close_popup();
        refuse(refusal);
    }

    void apply_choice() {
        if (choice_is_prompt_) {
            // Option index beyond the list means "skip".
            const auto option = static_cast<std::size_t>(radiobox_selected_);
            const auto refusal = controller_.respond_prompt(prompt_id_, option);
            close_popup();
            refuse(refusal);
            return;
        }
        std::string picked;
        if (choice_multi_) {
            for (std::size_t i = 0; i < choice_items_.size(); ++i) {
                if (!choice_checked_[i]) continue;
                if (!picked.empty()) picked += ",";
                picked += choice_items_[i].first;
            }
        } else if (static_cast<std::size_t>(radiobox_selected_) < choice_items_.size()) {
            picked = choice_items_[radiobox_selected_].first;
        }
        close_popup();
        refuse(controller_.set_choice(choice_id_, picked));
    }

    void commit_popup() {
        switch (popup_kind_) {
            case Popup::Edit: apply_text(); break;
            case Popup::Choice: apply_choice(); break;
            case Popup::Search: jump_to_search_hit(); break;
            case Popup::Preset: apply_preset(); break;
            default: close_popup(); break;
        }
    }

    bool handle_mouse(Event event) {
        if (!event.is_mouse()) return false;
        const auto& mouse = event.mouse();
        const bool in_list = mouse.y >= k_view_top
            && mouse.y < k_view_top + body_h_;

        if (in_list && mouse.button == Mouse::WheelUp) {
            cursor_ = cursor_ >= 3 ? cursor_ - 3 : 0;
            return true;
        }
        if (in_list && mouse.button == Mouse::WheelDown) {
            cursor_ = std::min(rows_.empty() ? 0 : rows_.size() - 1, cursor_ + 3);
            return true;
        }
        if (in_list && mouse.button == Mouse::Left
            && mouse.motion == Mouse::Pressed) {
            const auto index = view_begin_ + static_cast<std::size_t>(mouse.y - k_view_top);
            if (index >= rows_.size()) return true;
            cursor_ = index;

            // The expansion marker is its own hit zone: clicking [+] or [-]
            // toggles the menu without editing anything.
            const auto& row = rows_[index];
            const int name_cell = 1 + 2 * static_cast<int>(row.depth);
            if (row.expandable && mouse.x >= name_cell && mouse.x < name_cell + 4) {
                row.expanded ? collapse(row.id) : expand(row.id);
                click_pending_index_ = k_no_row;
                return true;
            }
            if (index == click_pending_index_) {
                click_pending_index_ = k_no_row;
                return open_editor(row);
            }
            click_pending_index_ = index;
            return true;
        }
        return false;
    }

    bool handle_global(const Event& event) {
        // F1/F2 are also bound to ? and d: several terminals keep the
        // function keys for themselves and never deliver them.
        if (event == Event::F1 || event.input() == "?") {
            popup_kind_ = Popup::Help;
            open_popup();
            return true;
        }
        if (event.input() == "q") {
            // Unsaved means changed since the last save, not "has values":
            // a save followed by q must exit without a prompt.
            if (controller_.dirty() && !quit_confirmed_) {
                message_ = "there are unsaved changes. press q again to quit anyway.";
                popup_kind_ = Popup::Message;
                open_popup();
                quit_confirmed_ = true;
                return true;
            }
            quit_();
            return true;
        }
        return false;
    }

    bool handle_main_key(const Event& event) {
        const std::size_t count = rows_.size();
        if (count == 0) return false;
        const Row* row = cursor_ < count ? &rows_[cursor_] : nullptr;

        if (event == Event::ArrowUp || event.input() == "k") {
            if (cursor_ > 0) --cursor_;
            return true;
        }
        if (event == Event::ArrowDown || event.input() == "j") {
            if (cursor_ + 1 < count) ++cursor_;
            return true;
        }
        if (event == Event::Home) { cursor_ = 0; return true; }
        if (event == Event::End) { cursor_ = count - 1; return true; }
        if (event == Event::PageUp) { cursor_ = cursor_ >= 10 ? cursor_ - 10 : 0; return true; }
        if (event == Event::PageDown) { cursor_ = std::min(count - 1, cursor_ + 10); return true; }
        if (event == Event::ArrowLeft || event.input() == "h") {
            if (row == nullptr) return true;
            if (row->expandable && row->expanded) {
                collapse(row->id);
            } else if (row->depth > 0) {
                // Explorer behavior: left on a leaf walks up and collapses
                // its parent menu.
                const auto parent = parent_of(cursor_);
                if (parent != k_no_row) {
                    collapse(rows_[parent].id);
                    cursor_ = parent;
                }
            }
            return true;
        }
        if (event == Event::ArrowRight || event.input() == "l") {
            if (row != nullptr && row->expandable) expand(row->id);
            return true;
        }
        if (event.input() == " ") {
            if (row != nullptr) activate_toggle(*row);
            return true;
        }
        if (event == Event::Return) {
            if (row == nullptr) return true;
            if (row->expandable) {
                row->expanded ? collapse(row->id) : expand(row->id);
                return true;
            }
            return open_editor(*row);
        }
        if (event == Event::F2 || event.input() == "d") {
            if (row != nullptr && row->kind != Kind::Menu) {
                detail_id_ = row->id;
                popup_kind_ = Popup::Detail;
                open_popup();
            }
            return true;
        }
        if (event.input() == "r") {
            if (row != nullptr) refuse(controller_.reset(row->id));
            return true;
        }
        if (event.input() == "s") {
            auto refusal = controller_.save();
            if (refusal.empty()) {
                // Saving is otherwise silent; say it happened.
                message_ = "saved.";
                popup_kind_ = Popup::Message;
                open_popup();
            } else {
                refuse(refusal);
            }
            return true;
        }
        if (event.input() == "c") {
            auto refusal = controller_.force_cache();
            if (!refusal.empty()) {
                refuse(refusal);
            } else {
                message_ = "cache applied.";
                popup_kind_ = Popup::Message;
                open_popup();
            }
            return true;
        }
        if (event.input() == "p") {
            preset_items_ = controller_.preset_items();
            preset_query_.clear();
            preset_hits_ = preset_items_;
            preset_selected_ = 0;
            popup_kind_ = Popup::Preset;
            open_popup();
            return true;
        }
        if (event.input() == "/") {
            popup_kind_ = Popup::Search;
            search_query_.clear();
            run_search();  // browse mode: the whole list, keys live at once
            open_popup();
            return true;
        }
        if (event.input() == "v") {
            popup_kind_ = Popup::Validation;
            open_popup();
            return true;
        }
        if (event.input() == "[") {
            log_offset_ = std::min(log_offset_ + 3, k_log_history);
            return true;
        }
        if (event.input() == "]") {
            log_offset_ = log_offset_ >= 3 ? log_offset_ - 3 : 0;
            return true;
        }
        return false;
    }

    /// @brief Index of the menu row that owns the row at @p index, walking
    ///        up by depth.  Returns k_no_row at the root level.
    std::size_t parent_of(std::size_t index) const {
        if (index >= rows_.size() || rows_[index].depth == 0) return k_no_row;
        const auto depth = rows_[index].depth;
        for (std::size_t at = index; at-- > 0;) {
            if (rows_[at].depth < depth) return at;
        }
        return k_no_row;
    }

    bool open_editor(const Row& row) {
        if (row.kind == Kind::Menu) {
            row.expanded ? collapse(row.id) : expand(row.id);
            return true;
        }
        if (row.kind == Kind::Enum || row.kind == Kind::Set) {
            open_choice(row);
            return true;
        }
        if (row.kind == Kind::Bool || row.kind == Kind::Tristate) {
            refuse(controller_.toggle(row.id));
            return true;
        }
        open_edit(row);
        return true;
    }

    bool activate_toggle(const Row& row) {
        if (row.kind == Kind::Enum || row.kind == Kind::Set) { open_choice(row); return true; }
        if (row.kind != Kind::Bool && row.kind != Kind::Tristate) return true;
        refuse(controller_.toggle(row.id));
        return true;
    }

    /// @brief Show a refused action instead of ignoring it.  Silence on a
    ///        refusal reads as a dead key.  An empty result is a successful
    ///        mutation; the controller marks the state dirty.
    void refuse(std::string refusal) {
        if (refusal.empty()) {
            maybe_prompt();
            return;
        }
        message_ = std::move(refusal);
        popup_kind_ = Popup::Message;
        open_popup();
    }

    /// @brief An unsatisfied `implies` becomes a prompt, as the spec's soft
    ///        suggestion requires: the engine never forces the value itself.
    void maybe_prompt() {
        if (popup_ != nullptr) return;
        const auto prompt = controller_.pending_prompt();
        if (!prompt.has_value()) return;

        prompt_id_ = prompt->id;
        prompt_message_ = prompt->message;
        choice_items_.clear();
        for (const auto& option : prompt->options) {
            choice_items_.emplace_back(option, option);
        }
        radiobox_selected_ = 0;
        choice_multi_ = false;
        choice_is_prompt_ = true;
        popup_kind_ = Popup::Choice;
        open_popup();
    }

    void expand(std::size_t id) {
        if (std::find(expanded_.begin(), expanded_.end(), id) == expanded_.end()) {
            expanded_.push_back(id);
        }
    }

    void collapse(std::size_t id) {
        std::erase(expanded_, id);
    }

    // ── popups ──────────────────────────────────────────────────────────
    enum class Popup : std::uint8_t { None, Help, Message, Detail, Edit, Choice, Validation, Search, Preset };
    static constexpr std::size_t k_no_row = static_cast<std::size_t>(-1);

    void open_popup() {
        popup_ = build_popup();
        if (popup_ != nullptr) popup_->TakeFocus();
    }

    void close_popup() {
        popup_kind_ = Popup::None;
        popup_ = nullptr;
        popup_events_ = nullptr;
        message_.clear();
    }

    /// @brief Every popup is a padded, rounded, fixed-size frame.
    /// @param[in] inner   The content.  Must fit within (width - 2) columns
    ///                    and (height - 2) rows; it is clipped beyond that.
    /// @param[in] width   Outer frame width, borders included.
    /// @param[in] height  Outer frame height, borders included.
    static Element popup_frame(std::string title_text, Element inner,
                               int width, int height) {
        Elements underlay;
        for (int i = 0; i < height - 2; ++i) {
            underlay.push_back(text(std::string(static_cast<std::size_t>(width - 2), ' ')));
        }
        return window(text(" " + title_text + " "),
                      dbox({vbox(std::move(underlay)), vbox({std::move(inner)})}))
             | size(WIDTH, EQUAL, width)
             | size(HEIGHT, EQUAL, height)
             | center;
    }

    /// @brief One padded content row.  Paints its indent; the underlay
    ///        covers the rest of the width.
    static Element padded(Element content) {
        return hbox({text("  "), std::move(content)});
    }

    static Component buttons_row(std::vector<std::pair<std::string, Closure>> buttons) {
        Components components;
        for (auto& button : buttons) {
            components.push_back(Button(button.first, std::move(button.second),
                                        ButtonOption::Ascii()));
        }
        return Container::Horizontal(std::move(components));
    }

    Component build_popup() {
        switch (popup_kind_) {
            case Popup::None: return nullptr;
            case Popup::Help: return build_help();
            case Popup::Message: return build_message();
            case Popup::Detail: return build_detail();
            case Popup::Edit: return build_edit();
            case Popup::Choice: return build_choice();
            case Popup::Validation: return build_validation();
            case Popup::Search: return build_search();
            case Popup::Preset: return build_preset();
        }
        return nullptr;
    }

    Component build_help() {
        auto buttons = buttons_row({{"Close", [this] { close_popup(); }}});
        // The buttons are container children, never rendered-and-discarded:
        // a drawing cannot take clicks or focus.
        popup_events_ = buttons;
        return Renderer(buttons, [buttons] {
            Elements rows;
            rows.push_back(padded(text("keys") | bold));
            rows.push_back(padded(text("  up/down, k/j, wheel      move")));
            rows.push_back(padded(text("  left/right, h/l          collapse / expand")));
            rows.push_back(padded(text("  space                    toggle boolean or tristate")));
            rows.push_back(padded(text("  enter / double-click     edit; choose for enum and set")));
            rows.push_back(padded(text("  r                        reset symbol to its default")));
            rows.push_back(padded(text("  s                        save the configuration")));
            rows.push_back(padded(text("  c                        apply a refused cache")));
            rows.push_back(padded(text("  p                        load a preset")));
            rows.push_back(padded(text("  /                        search symbols")));
            rows.push_back(padded(text("  v                        validation report")));
            rows.push_back(padded(text("  d                        symbol details")));
            rows.push_back(padded(text("  ?                        this help")));
            rows.push_back(padded(text("  q                        quit (asks on unsaved changes)")));
            rows.push_back(padded(text(std::string{})));
            rows.push_back(padded(buttons->Render()));
            rows.push_back(text(std::string{}));
            return popup_frame("help", vbox(std::move(rows)) | size(HEIGHT, EQUAL, 18),
                               74, 20);
        });
    }

    Component build_message() {
        auto buttons = buttons_row({{"OK", [this] { close_popup(); }}});
        // The box grows with the text instead of clipping it: a long refusal
        // has to be readable in full.
        const auto wrapped = wrap_lines(message_, 60);
        const int rows = static_cast<int>(std::min<std::size_t>(wrapped.size(), 10)) + 6;
        popup_events_ = buttons;
        return Renderer(buttons, [wrapped, rows, buttons] {
            Elements lines;
            for (const auto& line : wrapped) {
                lines.push_back(padded(text(line)));
            }
            lines.push_back(padded(text(std::string{})));
            lines.push_back(padded(buttons->Render()));
            lines.push_back(text(std::string{}));
            Element content = vbox(std::move(lines)) | size(HEIGHT, EQUAL, rows);
            return popup_frame("message", content, 68, rows + 2);
        });
    }

    Component build_detail() {
        const auto data = controller_.detail(detail_id_);

        auto buttons = buttons_row({
            {"Reset", [this] {
                 const auto refusal = controller_.reset(detail_id_);
                 close_popup();
                 refuse(refusal);
             }},
            {"Close", [this] { close_popup(); }},
        });
        popup_events_ = buttons;
        return Renderer(buttons, [data, buttons] {
            Elements rows;
            rows.push_back(padded(fact("name", data.name)));
            if (!data.label.empty()) rows.push_back(padded(fact("label", data.label)));
            rows.push_back(padded(fact("type", data.type)));
            rows.push_back(padded(fact("value", data.value)));
            rows.push_back(padded(fact("default", data.default_text)));
            if (!data.range.empty()) rows.push_back(padded(fact("range", data.range)));
            if (!data.choices.empty()) rows.push_back(padded(fact("choices", data.choices)));
            if (!data.exported.empty()) rows.push_back(padded(fact("exports", data.exported)));
            rows.push_back(padded(fact("from", data.source)));
            rows.push_back(padded(text(std::string{})));
            if (!data.help.empty()) rows.push_back(padded(text(clip(data.help, 54))));
            rows.push_back(padded(buttons->Render()));
            rows.push_back(text(std::string{}));
            return popup_frame("details", vbox(std::move(rows)) | size(HEIGHT, EQUAL, 18),
                               60, 20);
        });
    }

    Component build_edit() {
        const auto data = controller_.detail(edit_id_);
        InputOption option;
        option.on_enter = [this] { apply_text(); };
        // Single-line: multiline makes Enter insert a newline into the value
        // instead of applying it, and a trailing newline fails validation.
        option.multiline = false;
        // A quiet field: underline instead of the bright inverted bar, so
        // the cursor block stays visible while typing.
        option.transform = [](InputState state) {
            Element element = std::move(state.element);
            if (state.focused) element |= underlined;
            return element;
        };
        input_ = Input(&edit_content_, "new value", option);

        auto buttons = buttons_row({
            {"OK", [this] { apply_text(); }},
            {"Cancel", [this] { close_popup(); }},
        });

        const auto name = data.name;
        const auto subtitle = data.type + ", default " + data.default_text;
        auto form = Container::Vertical({input_, buttons});
        popup_events_ = form;
        return Renderer(form,
                        [name, subtitle, input_ref = input_, buttons] {
                            // Built per frame: the input must repaint as the
                            // user types and the buttons keep their focus.
                            Element content = vbox({
                                padded(text(name) | bold),
                                padded(text(clip(subtitle, 50)) | dim),
                                padded(text(std::string{})),
                                padded(input_ref->Render()),
                                padded(text(std::string{})),
                                padded(buttons->Render()),
                                text(std::string{}),
                            }) | size(HEIGHT, EQUAL, 9);
                            return popup_frame("edit value", content, 56, 11);
                        });
    }

    Component build_choice() {
        const auto data = controller_.detail(choice_id_);

        // A set is multi-select; anything else is single-select.  Members of
        // the current value start checked so editing extends the set instead
        // of silently replacing it.
        const bool multi = data.type == "set";
        const auto current_members = split_list(data.value);

        choice_labels_.clear();
        choice_checked_.clear();
        for (const auto& item : choice_items_) {
            choice_labels_.push_back(item.second);
            choice_checked_.push_back(
                std::find(current_members.begin(), current_members.end(), item.first)
                    != current_members.end());
        }
        if (choice_labels_.empty()) choice_labels_.push_back("(no choices declared)");

        Component list;
        if (multi) {
            Components boxes;
            for (std::size_t i = 0; i < choice_items_.size(); ++i) {
                boxes.push_back(Checkbox(choice_labels_[i], &choice_checked_[i],
                                         CheckboxOption::Simple()));
            }
            list = Container::Vertical(std::move(boxes));
        } else {
            list = Radiobox(&choice_labels_, &radiobox_selected_,
                            RadioboxOption::Simple());
        }

        auto buttons = buttons_row({
            {"OK", [this] { apply_choice(); }},
            {"Cancel", [this] {
                 if (choice_is_prompt_) {
                     // Beyond the list means "skip".
                     const auto refusal = controller_.respond_prompt(
                         prompt_id_, choice_items_.size());
                     close_popup();
                     refuse(refusal);
                     return;
                 }
                 close_popup();
             }},
        });

        const auto name = choice_is_prompt_ ? prompt_message_ : data.name;
        const auto current = choice_is_prompt_
            ? std::string{"choose a fix, or skip"}
            : "current: " + data.value;
        auto form = Container::Vertical({list, buttons});
        // Renderer is a view and does not forward input.  Keep the actual
        // selector tree as the popup event target so arrows, space, and
        // button activation reach the live controls.
        popup_events_ = form;
        return Renderer(form,
                        [this, name, current, list, buttons, multi] {
                            // Built per frame: the selection must repaint.
                            Element content = vbox({
                                padded(text(name) | bold),
                                padded(text(clip(current, 50)) | dim),
                                padded(text(std::string{})),
                                padded(list->Render() | vscroll_indicator | yframe
                                           | size(HEIGHT, EQUAL, 10)),
                                padded(buttons->Render()),
                                text(std::string{}),
                            }) | size(HEIGHT, EQUAL, 17);
                            return popup_frame(
                                choice_is_prompt_ ? "suggestion"
                                  : (multi ? "choose (multi)" : "choose"),
                                content, 56, 19);
                        });
    }

    Component build_validation() {
        validation_text_ = controller_.validation_report();

        auto buttons = buttons_row({{"Close", [this] { close_popup(); }}});
        popup_events_ = buttons;
        return Renderer(buttons, [this, buttons] {
            Elements rows;
            if (validation_text_.empty()) {
                rows.push_back(padded(text(" configuration is valid")));
            } else {
                for (const auto& line : validation_text_) {
                    Element entry = text(clip(line, 64));
                    if (line.starts_with("error") || line.starts_with("fatal")) {
                        entry |= bold;
                    } else if (line.starts_with("warning")) {
                        entry |= inverted;
                    }
                    rows.push_back(padded(std::move(entry)));
                }
            }
            rows.push_back(padded(text(std::string{})));
            rows.push_back(padded(buttons->Render()));
            rows.push_back(text(std::string{}));
            Element content = vbox(std::move(rows)) | vscroll_indicator | yframe
                            | size(HEIGHT, EQUAL, 18);
            return popup_frame("validation", content, 72, 20);
        });
    }

    Component build_search() {
        InputOption option;
        option.on_change = [this] { run_search(); };
        option.transform = [](InputState state) {
            Element element = std::move(state.element);
            if (state.focused) element |= underlined;
            return element;
        };
        input_ = Input(&search_query_, "type to search", option);

        auto buttons = buttons_row({
            {"Go", [this] { jump_to_search_hit(); }},
            {"Close", [this] { close_popup(); }},
        });

        auto search_keys = CatchEvent(Container::Vertical({input_, buttons}),
                                      [this](const Event& event) {
            // Arrows move the selection; every printable character types.
            if (event == Event::ArrowDown) {
                if (search_selected_ + 1 < search_hits_.size()) ++search_selected_;
                return true;
            }
            if (event == Event::ArrowUp) {
                if (search_selected_ > 0) --search_selected_;
                return true;
            }
            if (event == Event::Return) { jump_to_search_hit(); return true; }
            // Characters fall through to the input; on_change reruns the
            // search after the content updates, so results never lag.
            return false;
        });

        popup_events_ = search_keys;
        return Renderer(search_keys,
                        [input_ref = input_, buttons_ref = buttons, result_view = Renderer([this] {
                            Elements lines;
                            const auto [begin, end] =
                                list_window(search_hits_.size(), search_selected_, 12);
                            for (std::size_t i = begin; i < end; ++i) {
                                const bool selected = i == search_selected_;
                                std::string marker = selected ? "> " : "  ";
                                std::string name = search_hits_[i].name + "  " + search_hits_[i].label;
                                // The selected hit scrolls when it overflows;
                                // the rest clip and stay put.
                                auto line = hbox({
                                    text(marker),
                                    text(selected ? marquee(name, 56) : clip(name, 56)) | bold,
                                });
                                lines.push_back(selected ? line | bold : line);
                            }
                            if (search_hits_.empty()) {
                                lines.push_back(text(search_query_.empty()
                                    ? "  no visible symbols" : "  no match") | dim);
                            }
                            // Fixed height: the popup must not resize as hits come and go.
                            while (lines.size() < 12) lines.push_back(text(std::string{}));
                            return vbox(std::move(lines));
                        })] {
                            // Built per frame: query and hits must repaint.
                            Element body = vbox({
                                padded(input_ref->Render()),
                                padded(result_view->Render()),
                                padded(text(std::string{})),
                                padded(buttons_ref->Render()),
                                text(std::string{}),
                            }) | size(HEIGHT, EQUAL, 19);
                            return popup_frame("find", body, 64, 21);
                        });
    }

    /// @brief Filter the discovered presets by path fragment; empty query
    ///        lists them all.
    void run_preset_search() {
        preset_hits_.clear();
        preset_selected_ = 0;
        for (const auto& item : preset_items_) {
            if (preset_query_.empty() || item.find(preset_query_) != std::string::npos) {
                preset_hits_.push_back(item);
            }
        }
    }

    /// @brief Load the selected preset, or the typed path when nothing is
    ///        selected.  Loading marks the configuration unsaved: the values
    ///        no longer match the cache on disk.
    void apply_preset() {
        const std::string path = preset_selected_ < preset_hits_.size()
            ? preset_hits_[preset_selected_] : preset_query_;
        if (path.empty()) {
            close_popup();
            return;
        }
        const auto refusal = controller_.load_preset(path);
        close_popup();
        if (!refusal.empty()) {
            refuse(refusal);
            return;
        }
        message_ = "preset loaded: " + clip(path, 44);
        popup_kind_ = Popup::Message;
        open_popup();
        maybe_prompt();
    }

    Component build_preset() {
        InputOption option;
        option.on_change = [this] { run_preset_search(); };
        option.multiline = false;
        option.transform = [](InputState state) {
            Element element = std::move(state.element);
            if (state.focused) element |= underlined;
            return element;
        };
        input_ = Input(&preset_query_, "preset name or path", option);

        auto buttons = buttons_row({
            {"Load", [this] { apply_preset(); }},
            {"Cancel", [this] { close_popup(); }},
        });

        auto preset_keys = CatchEvent(Container::Vertical({input_, buttons}),
                                      [this](const Event& event) {
            if (event == Event::ArrowDown) {
                if (preset_selected_ + 1 < preset_hits_.size()) ++preset_selected_;
                return true;
            }
            if (event == Event::ArrowUp) {
                if (preset_selected_ > 0) --preset_selected_;
                return true;
            }
            if (event == Event::Return) { apply_preset(); return true; }
            return false;
        });

        popup_events_ = preset_keys;
        return Renderer(preset_keys,
                        [input_ref = input_, buttons, list = Renderer([this] {
                            Elements lines;
                            const auto [begin, end] =
                                list_window(preset_hits_.size(), preset_selected_, 12);
                            for (std::size_t i = begin; i < end; ++i) {
                                const bool selected = i == preset_selected_;
                                std::string marker = selected ? "> " : "  ";
                                // The selected entry scrolls when it
                                // overflows; the rest stay put.
                                const auto name = selected
                                    ? marquee(preset_hits_[i], 54)
                                    : clip(preset_hits_[i], 54);
                                auto line = hbox({text(marker), text(name) | bold});
                                lines.push_back(selected ? line | bold : line);
                            }
                            if (preset_hits_.empty()) {
                                lines.push_back(
                                    text("  no presets found — type a name or path") | dim);
                            }
                            while (lines.size() < 12) lines.push_back(text(std::string{}));
                            return vbox(std::move(lines));
                        })] {
                            // Built per frame: the query and hits must repaint.
                            Element body = vbox({
                                padded(input_ref->Render()),
                                padded(list->Render()),
                                padded(text(std::string{})),
                                padded(buttons->Render()),
                                text(std::string{}),
                            }) | size(HEIGHT, EQUAL, 19);
                            return popup_frame("load preset", body, 64, 21);
                        });
    }

    void run_search() {
        search_hits_.clear();
        search_selected_ = 0;
        const auto items = controller_.search_items();
        if (search_query_.empty()) {
            // Browse mode: every visible symbol up front, so the selection
            // keys work before anything is typed.
            search_hits_ = items;
            return;
        }
        std::vector<std::pair<int, SearchItem>> ranked;
        for (const auto& item : items) {
            int name_score = 0;
            int label_score = 0;
            const bool by_name = fuzzy_match(item.name, search_query_, name_score);
            const bool by_label = fuzzy_match(item.label, search_query_, label_score);
            if (by_name || by_label) {
                ranked.emplace_back(std::max(name_score, label_score), item);
            }
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (auto& [score, item] : ranked) (void)score, search_hits_.push_back(std::move(item));
    }

    void jump_to_search_hit() {
        if (search_hits_.empty()) { close_popup(); return; }
        const auto& hit = search_hits_[search_selected_];
        for (const auto menu_id : controller_.menu_chain(hit.id)) expand(menu_id);
        jump_row_id_ = hit.id;
        pending_jump_ = true;
        close_popup();
    }

    void open_choice(const Row& row) {
        choice_id_ = row.id;
        choice_items_.clear();
        const auto data = controller_.detail(row.id);
        for (const auto& item : split_list(data.choices)) {
            choice_items_.emplace_back(item, item);
        }
        radiobox_selected_ = 0;
        for (std::size_t i = 0; i < choice_items_.size(); ++i) {
            if (choice_items_[i].first == data.value) {
                radiobox_selected_ = static_cast<int>(i);
                break;
            }
        }
        choice_multi_ = row.kind == Kind::Set;
        choice_is_prompt_ = false;
        popup_kind_ = Popup::Choice;
        open_popup();
    }

    void open_edit(const Row& row) {
        edit_id_ = row.id;
        edit_content_ = row.value == "<unset>" ? "" : row.value;
        popup_kind_ = Popup::Edit;
        open_popup();
    }

    Controller& controller_;
    Component root_;
    Component popup_;
    Component popup_events_;
    Component input_;
    Closure quit_;
    ScreenInteractive* screen_{nullptr};

    std::chrono::steady_clock::time_point marquee_clock_{std::chrono::steady_clock::now()};
    bool needs_repaint_{false};

    Popup popup_kind_{Popup::None};
    std::vector<Row> rows_;
    std::vector<std::size_t> expanded_;
    std::size_t cursor_{0};
    std::size_t view_begin_{0};
    int body_h_{3};
    std::size_t log_offset_{0};

    std::size_t click_pending_index_{k_no_row};
    bool pending_jump_{false};
    std::size_t jump_row_id_{k_no_row};
    std::size_t detail_id_{0};
    std::size_t edit_id_{0};
    std::string edit_content_;
    std::size_t choice_id_{0};
    std::vector<std::pair<std::string, std::string>> choice_items_;
    std::vector<std::string> choice_labels_;
    std::deque<bool> choice_checked_;
    int radiobox_selected_{0};
    bool choice_multi_{false};
    bool choice_is_prompt_{false};
    std::size_t prompt_id_{0};
    std::string prompt_message_;
    std::vector<std::string> validation_text_;

    bool quit_confirmed_{false};
    std::string message_;

    std::string search_query_;
    std::vector<SearchItem> search_hits_;
    std::size_t search_selected_{0};

    std::vector<std::string> preset_items_;
    std::vector<std::string> preset_hits_;
    std::string preset_query_;
    std::size_t preset_selected_{0};
};

}  // namespace

int run(Controller& controller) {
    TuiApp app{controller};
    return app.run();
}

}  // namespace zxconfig::tui
