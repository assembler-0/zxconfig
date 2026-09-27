// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/text.hpp>

namespace zxconfig {
namespace {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool hex_digit(char c, std::uint32_t& value) noexcept {
    if (c >= '0' && c <= '9') { value = static_cast<std::uint32_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { value = static_cast<std::uint32_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { value = static_cast<std::uint32_t>(c - 'A' + 10); return true; }
    return false;
}

/// @brief Decode \u{...}, with i on the backslash that starts it.
bool decode_unicode(std::string_view raw, std::size_t& i, std::string& out) {
    i += 2;
    if (i >= raw.size() || raw[i] != '{') return false;
    ++i;

    std::uint32_t cp = 0;
    bool any = false;
    while (i < raw.size() && raw[i] != '}') {
        std::uint32_t digit = 0;
        if (!hex_digit(raw[i], digit)) return false;
        cp = (cp << 4) | digit;
        if (cp > 0x10FFFF) return false;
        any = true;
        ++i;
    }
    if (!any || i >= raw.size() || cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    ++i;

    append_utf8(out, cp);
    return true;
}

}  // namespace

DecodedText decode_escapes(std::string_view raw) {
    DecodedText out;
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '\\') {
            out.value.push_back(raw[i]);
            ++i;
            continue;
        }

        const std::size_t escape = i;
        if (i + 1 >= raw.size()) {
            out.error = static_cast<std::uint32_t>(escape);
            return out;
        }
        switch (raw[i + 1]) {
            case '\\': out.value.push_back('\\'); i += 2; break;
            case '"':  out.value.push_back('"');  i += 2; break;
            case 'n':  out.value.push_back('\n'); i += 2; break;
            case 't':  out.value.push_back('\t'); i += 2; break;
            case 'u':
                if (!decode_unicode(raw, i, out.value)) {
                    out.error = static_cast<std::uint32_t>(escape);
                    return out;
                }
                break;
            default:
                out.value.push_back('\\');
                out.value.push_back(raw[i + 1]);
                i += 2;
                break;
        }
    }
    return out;
}

}  // namespace zxconfig
