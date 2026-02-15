#include "json_min.h"

#include <cctype>
#include <charconv>
#include <cstdlib>

namespace sugar::json_min {

    namespace {
        struct Cursor {
            std::string_view s;
            std::size_t i{ 0 };
            std::size_t line{ 1 };
            std::size_t col{ 1 };

            bool eof() const { return i >= s.size(); }
            char peek() const { return eof() ? '\0' : s[i]; }

            char get() {
                if (eof()) return '\0';
                char c = s[i++];
                if (c == '\n') { ++line; col = 1; }
                else { ++col; }
                return c;
            }

            void skip_ws() {
                while (!eof()) {
                    unsigned char c = static_cast<unsigned char>(peek());
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') get();
                    else break;
                }
            }
        };

        [[nodiscard]] bool fail(Cursor const& cur, ParseError& err, std::string msg) {
            err.message = std::move(msg);
            err.line = cur.line;
            err.col = cur.col;
            return false;
        }

        bool parse_value(Cursor& cur, Value& out, ParseError& err);

        bool parse_literal(Cursor& cur, const char* lit) {
            for (std::size_t k = 0; lit[k]; ++k) {
                if (cur.eof() || cur.peek() != lit[k]) return false;
                cur.get();
            }
            return true;
        }

        bool parse_string(Cursor& cur, std::string& out, ParseError& err) {
            if (cur.get() != '"') return fail(cur, err, "Expected '\"' to start string");
            std::string s;
            while (!cur.eof()) {
                char c = cur.get();
                if (c == '"') { out = std::move(s); return true; }
                if (c == '\\') {
                    if (cur.eof()) return fail(cur, err, "Unexpected EOF in string escape");
                    char e = cur.get();
                    switch (e) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'b': s.push_back('\b'); break;
                    case 'f': s.push_back('\f'); break;
                    case 'n': s.push_back('\n'); break;
                    case 'r': s.push_back('\r'); break;
                    case 't': s.push_back('\t'); break;
                    case 'u':
                    {
                        auto hex = [](char h) -> int {
                            if (h >= '0' && h <= '9') return h - '0';
                            if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
                            if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
                            return -1;
                            };
                        if (cur.i + 4 > cur.s.size()) return fail(cur, err, "Unexpected EOF in \\u escape");
                        int h0 = hex(cur.get());
                        int h1 = hex(cur.get());
                        int h2 = hex(cur.get());
                        int h3 = hex(cur.get());
                        if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return fail(cur, err, "Invalid hex in \\u escape");
                        unsigned code = static_cast<unsigned>((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                        if (code <= 0x7F) {
                            s.push_back(static_cast<char>(code));
                        }
                        else if (code <= 0x7FF) {
                            s.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
                            s.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        else {
                            s.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
                            s.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            s.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                    }
                    break;
                    default:
                        return fail(cur, err, "Invalid string escape");
                    }
                }
                else {
                    if (static_cast<unsigned char>(c) < 0x20) return fail(cur, err, "Control char in string");
                    s.push_back(c);
                }
            }
            return fail(cur, err, "Unexpected EOF in string");
        }

        bool parse_number(Cursor& cur, Value& out, ParseError& err) {
            std::size_t start = cur.i;
            bool has_dot = false;
            bool has_exp = false;
            if (cur.peek() == '-') cur.get();

            auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
            if (!is_digit(cur.peek())) return fail(cur, err, "Invalid number");

            if (cur.peek() == '0') {
                cur.get();
            }
            else {
                while (is_digit(cur.peek())) cur.get();
            }

            if (cur.peek() == '.') {
                has_dot = true;
                cur.get();
                if (!is_digit(cur.peek())) return fail(cur, err, "Invalid number fraction");
                while (is_digit(cur.peek())) cur.get();
            }

            if (cur.peek() == 'e' || cur.peek() == 'E') {
                has_exp = true;
                cur.get();
                if (cur.peek() == '+' || cur.peek() == '-') cur.get();
                if (!is_digit(cur.peek())) return fail(cur, err, "Invalid number exponent");
                while (is_digit(cur.peek())) cur.get();
            }

            std::string_view token = cur.s.substr(start, cur.i - start);

            if (!has_dot && !has_exp) {
                std::int64_t v = 0;
                auto res = std::from_chars(token.data(), token.data() + token.size(), v);
                if (res.ec == std::errc()) { out = Value(v); return true; }
            }

            char* endp = nullptr;
            std::string tmp(token);
            double dv = std::strtod(tmp.c_str(), &endp);
            if (endp != tmp.c_str() + tmp.size()) return fail(cur, err, "Invalid floating number");
            out = Value(dv);
            return true;
        }

        bool parse_array(Cursor& cur, Value& out, ParseError& err) {
            if (cur.get() != '[') return fail(cur, err, "Expected '['");
            Array a;
            cur.skip_ws();
            if (cur.peek() == ']') { cur.get(); out = Value(std::move(a)); return true; }
            while (true) {
                cur.skip_ws();
                Value v;
                if (!parse_value(cur, v, err)) return false;
                a.push_back(std::move(v));
                cur.skip_ws();
                if (cur.peek() == ',') { cur.get(); continue; }
                if (cur.peek() == ']') { cur.get(); out = Value(std::move(a)); return true; }
                return fail(cur, err, "Expected ',' or ']' in array");
            }
        }

        bool parse_object(Cursor& cur, Value& out, ParseError& err) {
            if (cur.get() != '{') return fail(cur, err, "Expected '{'");
            Object o;
            cur.skip_ws();
            if (cur.peek() == '}') { cur.get(); out = Value(std::move(o)); return true; }
            while (true) {
                cur.skip_ws();
                if (cur.peek() != '"') return fail(cur, err, "Expected string key in object");
                std::string key;
                if (!parse_string(cur, key, err)) return false;
                cur.skip_ws();
                if (cur.get() != ':') return fail(cur, err, "Expected ':' after object key");
                cur.skip_ws();
                Value val;
                if (!parse_value(cur, val, err)) return false;
                o.emplace(std::move(key), std::move(val));
                cur.skip_ws();
                if (cur.peek() == ',') { cur.get(); continue; }
                if (cur.peek() == '}') { cur.get(); out = Value(std::move(o)); return true; }
                return fail(cur, err, "Expected ',' or '}' in object");
            }
        }

        bool parse_value(Cursor& cur, Value& out, ParseError& err) {
            cur.skip_ws();
            if (cur.eof()) return fail(cur, err, "Unexpected EOF while parsing value");
            char c = cur.peek();
            if (c == 'n') {
                if (!parse_literal(cur, "null")) return fail(cur, err, "Invalid token");
                out = Value(nullptr);
                return true;
            }
            if (c == 't') {
                if (!parse_literal(cur, "true")) return fail(cur, err, "Invalid token");
                out = Value(true);
                return true;
            }
            if (c == 'f') {
                if (!parse_literal(cur, "false")) return fail(cur, err, "Invalid token");
                out = Value(false);
                return true;
            }
            if (c == '"') {
                std::string s;
                if (!parse_string(cur, s, err)) return false;
                out = Value(std::move(s));
                return true;
            }
            if (c == '[') return parse_array(cur, out, err);
            if (c == '{') return parse_object(cur, out, err);
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number(cur, out, err);
            return fail(cur, err, "Unexpected character while parsing value");
        }
    }

    bool parse(std::string_view src, Value& out, ParseError& err) {
        Cursor cur{ src };
        if (!parse_value(cur, out, err)) return false;
        cur.skip_ws();
        if (!cur.eof()) return fail(cur, err, "Trailing characters after JSON document");
        return true;
    }

    const Value* obj_get(const Object& o, const char* key) {
        auto it = o.find(key);
        if (it == o.end()) return nullptr;
        return &it->second;
    }

    const Value* obj_get(const Object& o, const std::string& key) {
        auto it = o.find(key);
        if (it == o.end()) return nullptr;
        return &it->second;
    }

} // namespace sugar::json_min
