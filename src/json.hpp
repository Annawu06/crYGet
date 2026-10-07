#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace cryget {

struct Json {
    enum class Kind { Null, Boolean, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json& get(const std::string& key) const {
        static const Json empty;
        const auto found = object.find(key);
        return found == object.end() ? empty : found->second;
    }
    const Json& at(size_t index) const {
        static const Json empty;
        return index < array.size() ? array[index] : empty;
    }
    bool is_string() const { return kind == Kind::String; }
};

class JsonParser {
    const std::string& input_;
    size_t position_ = 0;
    static void append_utf8(std::string& output, uint32_t point) {
        if (point <= 0x7f) output += static_cast<char>(point);
        else if (point <= 0x7ff) {
            output += static_cast<char>(0xc0 | (point >> 6));
            output += static_cast<char>(0x80 | (point & 0x3f));
        } else if (point <= 0xffff) {
            output += static_cast<char>(0xe0 | (point >> 12));
            output += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (point & 0x3f));
        } else {
            output += static_cast<char>(0xf0 | (point >> 18));
            output += static_cast<char>(0x80 | ((point >> 12) & 0x3f));
            output += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (point & 0x3f));
        }
    }
    void space() {
        while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\n' ||
               input_[position_] == '\r' || input_[position_] == '\t')) ++position_;
    }
    void expect(char character) {
        space();
        if (position_ >= input_.size() || input_[position_++] != character)
            throw std::runtime_error("Invalid JSON");
    }
    uint32_t hex4() {
        if (position_ + 4 > input_.size()) throw std::runtime_error("Invalid JSON Unicode escape");
        uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[position_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code += c - '0';
            else if (c >= 'a' && c <= 'f') code += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') code += c - 'A' + 10;
            else throw std::runtime_error("Invalid JSON Unicode escape");
        }
        return code;
    }
    std::string read_string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') return result;
            if (static_cast<unsigned char>(c) < 0x20) throw std::runtime_error("Invalid JSON string");
            if (c != '\\') { result += c; continue; }
            if (position_ >= input_.size()) break;
            switch (input_[position_++]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': {
                    uint32_t point = hex4();
                    if (point >= 0xd800 && point <= 0xdbff) {
                        if (position_ + 2 > input_.size() || input_[position_++] != '\\' || input_[position_++] != 'u')
                            throw std::runtime_error("Invalid JSON surrogate pair");
                        const uint32_t lower = hex4();
                        if (lower < 0xdc00 || lower > 0xdfff) throw std::runtime_error("Invalid JSON surrogate pair");
                        point = 0x10000 + ((point - 0xd800) << 10) + (lower - 0xdc00);
                    } else if (point >= 0xdc00 && point <= 0xdfff) {
                        throw std::runtime_error("Invalid JSON surrogate pair");
                    }
                    append_utf8(result, point);
                    break;
                }
                default: throw std::runtime_error("Invalid JSON escape");
            }
        }
        throw std::runtime_error("Unterminated JSON string");
    }
    Json value(int depth) {
        if (depth > 64) throw std::runtime_error("JSON nesting is too deep");
        space();
        if (position_ >= input_.size()) throw std::runtime_error("Unexpected end of JSON");
        Json result;
        const char first = input_[position_];
        if (first == '"') {
            result.kind = Json::Kind::String;
            result.string = read_string();
        } else if (first == '{') {
            result.kind = Json::Kind::Object;
            ++position_;
            space();
            if (position_ < input_.size() && input_[position_] == '}') { ++position_; return result; }
            do {
                space();
                auto key = read_string();
                expect(':');
                result.object.emplace(std::move(key), value(depth + 1));
                space();
                if (position_ < input_.size() && input_[position_] == '}') { ++position_; return result; }
                expect(',');
            } while (true);
        } else if (first == '[') {
            result.kind = Json::Kind::Array;
            ++position_;
            space();
            if (position_ < input_.size() && input_[position_] == ']') { ++position_; return result; }
            do {
                result.array.push_back(value(depth + 1));
                space();
                if (position_ < input_.size() && input_[position_] == ']') { ++position_; return result; }
                expect(',');
            } while (true);
        } else if (first == 't' && input_.compare(position_, 4, "true") == 0) {
            result.kind = Json::Kind::Boolean; result.boolean = true; position_ += 4;
        } else if (first == 'f' && input_.compare(position_, 5, "false") == 0) {
            result.kind = Json::Kind::Boolean; position_ += 5;
        } else if (first == 'n' && input_.compare(position_, 4, "null") == 0) {
            position_ += 4;
        } else {
            const size_t start = position_;
            if (input_[position_] == '-') ++position_;
            if (position_ >= input_.size()) throw std::runtime_error("Invalid JSON number");
            if (input_[position_] == '0') ++position_;
            else {
                if (input_[position_] < '1' || input_[position_] > '9') throw std::runtime_error("Invalid JSON number");
                while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            }
            if (position_ < input_.size() && input_[position_] == '.') {
                ++position_;
                if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9')
                    throw std::runtime_error("Invalid JSON number");
                while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            }
            if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
                ++position_;
                if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
                if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9')
                    throw std::runtime_error("Invalid JSON number");
                while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            }
            result.kind = Json::Kind::Number;
            result.number = std::strtod(input_.c_str() + start, nullptr);
        }
        return result;
    }
public:
    explicit JsonParser(const std::string& input) : input_(input) {}
    Json parse() {
        auto parsed = value(0);
        space();
        if (position_ != input_.size()) throw std::runtime_error("Trailing JSON data");
        return parsed;
    }
};

} // namespace cryget
