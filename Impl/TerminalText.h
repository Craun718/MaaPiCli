#pragma once

#include <string>
#include <string_view>

namespace MaaPiCliTerminal
{
// Remove terminal commands as complete sequences, rather than leaving their
// printable payload behind. Keep UTF-8 bytes intact. Single-line fields cannot
// inject extra lines; message bodies may retain newlines and tabs.
inline std::string sanitize(std::string_view text, bool multiline = false)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0x1b) {
            if (++i == text.size()) {
                break;
            }
            const char command = text[i];
            if (command == '[') { // CSI, terminated by a final byte.
                while (++i < text.size()) {
                    if (text[i] >= 0x40 && text[i] <= 0x7e) {
                        break;
                    }
                }
            }
            else if (command == ']' || command == 'P' || command == 'X' || command == '^' || command == '_') {
                // OSC/DCS/SOS/PM/APC, terminated by ST (or BEL for OSC).
                while (++i < text.size()) {
                    if ((command == ']' && text[i] == '\a') || (text[i] == 0x1b && i + 1 < text.size() && text[i + 1] == '\\')) {
                        if (text[i] == 0x1b) {
                            ++i;
                        }
                        break;
                    }
                }
            }
            else { // ESC sequences with optional intermediate bytes.
                while (i < text.size() && text[i] >= 0x20 && text[i] <= 0x2f) {
                    ++i;
                }
            }
            continue;
        }
        if (c == 0x7f || (c < 0x20 && !(multiline && (c == '\n' || c == '\t')))) {
            continue;
        }
        result.push_back(text[i]);
    }
    return result;
}
}
