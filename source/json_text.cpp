#include "json_text.h"

#include <cctype>

using std::size_t;
using std::string;

namespace {

void append_utf8(string &out, unsigned int cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The four hex digits at `at`, or -1 if there are not four of them. Does not
// move anything: every caller decides for itself what a malformed escape means.
long read_hex4(const string &json, size_t at) {
    if (at + 4 > json.size())
        return -1;

    long value = 0;
    for (size_t i = at; i < at + 4; ++i) {
        const int digit = hex_value(json[i]);
        if (digit < 0)
            return -1;
        value = value * 16 + digit;
    }
    return value;
}

}  // namespace

string json_read_string(const string &json, size_t &pos) {
    string out;
    if (pos >= json.size() || json[pos] != '"')
        return out;
    ++pos;

    while (pos < json.size()) {
        const char c = json[pos];
        if (c == '"')
            return out;
        if (c != '\\') {
            out += c;
            ++pos;
            continue;
        }
        if (pos + 1 >= json.size()) {
            pos = json.size();
            return out;
        }

        const char escape = json[pos + 1];
        pos += 2;
        switch (escape) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'u': {
            const long first = read_hex4(json, pos);
            if (first < 0) {
                // Not an escape at all, whatever it was meant to be. Keeping the
                // letter loses nothing and cannot swallow the closing quote.
                out += 'u';
                break;
            }
            pos += 4;

            unsigned int cp = static_cast<unsigned int>(first);
            // A code point above the BMP arrives as a surrogate pair. Decoding
            // the halves separately would emit two invalid characters.
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                pos + 1 < json.size() && json[pos] == '\\' && json[pos + 1] == 'u') {
                const long second = read_hex4(json, pos + 2);
                if (second >= 0xDC00 && second <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10)
                                 + (static_cast<unsigned int>(second) - 0xDC00);
                    pos += 6;
                }
            }
            append_utf8(out, cp);
            break;
        }
        default:
            // An escape JSON does not define. The character itself is the best
            // guess and matches what every scan here did before.
            out += escape;
            break;
        }
    }

    return out;
}

string json_repair_dropped_escapes(const string &value) {
    string out;
    out.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != 'u') {
            out += value[i];
            continue;
        }

        const long first = read_hex4(value, i + 1);
        if (first < 0) {
            out += value[i];
            continue;
        }

        unsigned int cp = static_cast<unsigned int>(first);
        size_t consumed = 5;   // the u and its four digits

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // A surrogate pair lost both of its backslashes, so the low half is
            // sitting right behind the high one as another bare escape.
            const long second = (i + 6 < value.size() && value[i + 5] == 'u')
                                ? read_hex4(value, i + 6) : -1;
            if (second < 0xDC00 || second > 0xDFFF) {
                out += value[i];   // half a character is not something to guess at
                continue;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10)
                         + (static_cast<unsigned int>(second) - 0xDC00);
            consumed = 10;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            out += value[i];
            continue;
        }

        // Any well-formed uXXXX is taken as a dropped escape: the old scan
        // mangled every one of them, whatever the code point. A cached name
        // that genuinely reads "u" followed by four hex digits would be
        // rewritten here, which no game, genre or company name does.
        append_utf8(out, cp);
        i += consumed - 1;
    }

    return out;
}
