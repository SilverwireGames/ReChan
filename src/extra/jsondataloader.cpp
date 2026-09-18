#include "extra/jsondataloader.h"

#if MOD_LOADER

#include "pc/log.h"
#include "p3d/fileio.h"
#include <cctype>
#include <cstdlib>
#include <sstream>

// Minimal recursive-descent JSON parser

class JSONParser {
public:
    explicit JSONParser(const std::string& src) : m_src(src), m_pos(0) {}

    JSONValue Parse() {
        SkipWhitespace();
        JSONValue val = ParseValue();
        SkipWhitespace();
        if (m_pos != m_src.size()) m_error = true;
        return val;
    }

    bool HasError() const { return m_error; }

private:
    const std::string& m_src;
    size_t m_pos;
    bool m_error = false;

    char Peek() const {
        return m_pos < m_src.size() ? m_src[m_pos] : '\0';
    }

    char Next() {
        return m_pos < m_src.size() ? m_src[m_pos++] : '\0';
    }

    void SkipWhitespace() {
        while (m_pos < m_src.size() && std::isspace(static_cast<unsigned char>(m_src[m_pos]))) {
            m_pos++;
        }
    }

    JSONValue ParseValue() {
        SkipWhitespace();
        char c = Peek();
        if (c == '"') return ParseString();
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == 't' || c == 'f') return ParseBool();
        if (c == 'n') return ParseNull();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber();
        m_error = true;
        return {};
    }

    JSONValue ParseNull() {
        if (m_pos + 4 <= m_src.size() && m_src.substr(m_pos, 4) == "null") {
            m_pos += 4;
            JSONValue val;
            val.type = JSONValue::Type::Null;
            return val;
        }
        m_error = true;
        return {};
    }

    JSONValue ParseBool() {
        if (m_pos + 4 <= m_src.size() && m_src.substr(m_pos, 4) == "true") {
            m_pos += 4;
            JSONValue val;
            val.type = JSONValue::Type::Bool;
            val.boolVal = true;
            return val;
        }
        if (m_pos + 5 <= m_src.size() && m_src.substr(m_pos, 5) == "false") {
            m_pos += 5;
            JSONValue val;
            val.type = JSONValue::Type::Bool;
            val.boolVal = false;
            return val;
        }
        m_error = true;
        return {};
    }

    JSONValue ParseString() {
        Next(); // skip opening '"'
        std::string result;
        while (m_pos < m_src.size()) {
            char c = Next();
            if (c == '"') {
                JSONValue val;
                val.type = JSONValue::Type::String;
                val.stringVal = std::move(result);
                return val;
            }
            if (c == '\\') {
                char esc = Next();
                switch (esc) {
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/';  break;
                    case 'n':  result += '\n'; break;
                    case 'r':  result += '\r'; break;
                    case 't':  result += '\t'; break;
                    default:   result += esc;  break;
                }
            }
            else {
                result += c;
            }
        }
        m_error = true;
        return {};
    }

    JSONValue ParseNumber() {
        size_t start = m_pos;
        if (Peek() == '-') m_pos++;
        const size_t digitsStart = m_pos;

        // Integer part
        if (Peek() == '0') {
            m_pos++;
        }
        else {
            while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                m_pos++;
            }
        }
        if (m_pos == digitsStart) {
            m_error = true;
            return {};
        }

        bool isFloat = false;

        // Fractional part
        if (Peek() == '.') {
            isFloat = true;
            m_pos++;
            while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                m_pos++;
            }
        }

        // Exponent part
        if (Peek() == 'e' || Peek() == 'E') {
            isFloat = true;
            m_pos++;
            if (Peek() == '+' || Peek() == '-') m_pos++;
            while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                m_pos++;
            }
        }

        std::string numStr = m_src.substr(start, m_pos - start);

        JSONValue val;
        if (isFloat) {
            val.type = JSONValue::Type::Float;
            val.floatVal = static_cast<f32>(std::atof(numStr.c_str()));
        }
        else {
            val.type = JSONValue::Type::Int;
            val.intVal = static_cast<s32>(std::atoi(numStr.c_str()));
        }
        return val;
    }

    JSONValue ParseArray() {
        Next(); // skip '['
        JSONValue val;
        val.type = JSONValue::Type::Array;

        SkipWhitespace();
        if (Peek() == ']') {
            Next();
            return val;
        }

        while (true) {
            val.arrayVal.push_back(ParseValue());
            SkipWhitespace();
            char c = Next();
            if (c == ']') break;
            if (c != ',') { m_error = true; return val; }
            SkipWhitespace();
        }
        return val;
    }

    JSONValue ParseObject() {
        Next(); // skip '{'
        JSONValue val;
        val.type = JSONValue::Type::Object;

        SkipWhitespace();
        if (Peek() == '}') {
            Next();
            return val;
        }

        while (true) {
            SkipWhitespace();
            if (Peek() != '"') { m_error = true; return val; }
            JSONValue keyVal = ParseString();
            SkipWhitespace();
            if (Next() != ':') { m_error = true; return val; }
            val.objectVal[keyVal.stringVal] = ParseValue();
            SkipWhitespace();
            char c = Next();
            if (c == '}') break;
            if (c != ',') { m_error = true; return val; }
        }
        return val;
    }
};

// Public API

JSONValue JSONDataLoader::LoadFromFile(const char* path) {
    if (!path || !path[0]) {
        LOG("[JSONDataLoader] Invalid path");
        return {};
    }

    auto text = p3d::io::ReadTextFile(p3d::io::ResolvePath(path));
    if (!text) {
        LOG("[JSONDataLoader] Cannot open: %s", path);
        return {};
    }

    std::string content = std::move(*text);

    JSONParser parser(content);
    JSONValue result = parser.Parse();

    if (parser.HasError()) {
        LOG("[JSONDataLoader] Parse error in: %s", path);
        return {};
    }

    LOG("[JSONDataLoader] Loaded: %s", path);
    return result;
}

#endif // MOD_LOADER
