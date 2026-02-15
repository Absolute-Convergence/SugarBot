#pragma once

// -----------------------------------------------------------------------------
// json_min: tiny JSON value + parser
// -----------------------------------------------------------------------------
//
// This is a deliberately small JSON implementation intended for SugarBot's
// PlanSpec and related configuration files. It supports:
//   - null, bool, number, string
//   - arrays
//   - objects
//
// It is NOT a full-featured JSON library (no JSON pointer, no schema, etc.).
//
// Design goals:
//   - zero third-party dependencies
//   - clear error messages (line/col)
//   - stable behavior for AI-generated PlanSpecs
//
// Numbers are parsed as:
//   - int64 if they contain no '.', 'e', or 'E'
//   - double otherwise
//
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace sugar::json_min {

    struct ParseError {
        std::string message;
        std::size_t line{ 1 };
        std::size_t col{ 1 };
    };

    struct Value;
    using Object = std::unordered_map<std::string, Value>;
    using Array = std::vector<Value>;

    struct Value {
        using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;
        Storage v{};

        // Constructors
        Value() : v(nullptr) {}
        Value(std::nullptr_t) : v(nullptr) {}
        Value(bool b) : v(b) {}
        Value(std::int64_t i) : v(i) {}
        Value(double d) : v(d) {}
        Value(std::string s) : v(std::move(s)) {}
        Value(const char* s) : v(std::string(s)) {}
        Value(Array a) : v(std::move(a)) {}
        Value(Object o) : v(std::move(o)) {}

        // Type checks
        bool is_null()  const { return std::holds_alternative<std::nullptr_t>(v); }
        bool is_bool()  const { return std::holds_alternative<bool>(v); }
        bool is_i64()   const { return std::holds_alternative<std::int64_t>(v); }
        bool is_f64()   const { return std::holds_alternative<double>(v); }
        bool is_num()   const { return is_i64() || is_f64(); }
        bool is_str()   const { return std::holds_alternative<std::string>(v); }
        bool is_arr()   const { return std::holds_alternative<Array>(v); }
        bool is_obj()   const { return std::holds_alternative<Object>(v); }

        // Accessors (throws std::bad_variant_access if wrong)
        const bool& as_bool() const { return std::get<bool>(v); }
        const std::int64_t& as_i64() const { return std::get<std::int64_t>(v); }
        const double& as_f64() const { return std::get<double>(v); }
        const std::string& as_str() const { return std::get<std::string>(v); }
        const Array& as_arr() const { return std::get<Array>(v); }
        const Object& as_obj() const { return std::get<Object>(v); }

        // Helpers
        double number_as_f64() const {
            if (is_f64()) return std::get<double>(v);
            return static_cast<double>(std::get<std::int64_t>(v));
        }

        std::int64_t number_as_i64() const {
            if (is_i64()) return std::get<std::int64_t>(v);
            return static_cast<std::int64_t>(std::get<double>(v));
        }
    };

    // Parse a JSON document into a Value.
    // On failure, returns false and fills err.
    bool parse(std::string_view src, Value& out, ParseError& err);

    // Convenience helpers for object field access.
    const Value* obj_get(const Object& o, const char* key);
    const Value* obj_get(const Object& o, const std::string& key);

} // namespace sugar::json_min
