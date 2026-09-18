#pragma once
#include "core.h"
#include "gen/config.h"
#include <string>
#include <vector>
#include <unordered_map>

#if MOD_LOADER

// Lightweight JSON value type for game data loading.
// Avoids pulling in a full JSON library for simple config/data files.
struct JSONValue {
    enum class Type { Null, Bool, Int, Float, String, Array, Object };

    Type type = Type::Null;
    bool boolVal = false;
    s32 intVal = 0;
    f32 floatVal = 0.0f;
    std::string stringVal;
    std::vector<JSONValue> arrayVal;
    std::unordered_map<std::string, JSONValue> objectVal;

    JSONValue() = default;

    bool IsNull() const { return type == Type::Null; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsInt() const { return type == Type::Int; }
    bool IsFloat() const { return type == Type::Float; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    const JSONValue* FindMember(const char* key) const {
        if (!IsObject()) return nullptr;
        auto it = objectVal.find(key);
        return it != objectVal.end() ? &it->second : nullptr;
    }

    s32 GetInt(const char* key, s32 defaultVal = 0) const {
        const JSONValue* v = FindMember(key);
        return (v && v->IsInt()) ? v->intVal : defaultVal;
    }

    f32 GetFloat(const char* key, f32 defaultVal = 0.0f) const {
        const JSONValue* v = FindMember(key);
        return (v && (v->IsFloat() || v->IsInt())) ? (v->IsFloat() ? v->floatVal : (f32)v->intVal) : defaultVal;
    }

    std::string GetString(const char* key, const std::string& defaultVal = {}) const {
        const JSONValue* v = FindMember(key);
        return (v && v->IsString()) ? v->stringVal : defaultVal;
    }

    bool GetBool(const char* key, bool defaultVal = false) const {
        const JSONValue* v = FindMember(key);
        return (v && v->IsBool()) ? v->boolVal : defaultVal;
    }

    const JSONValue* GetArray(const char* key) const {
        const JSONValue* v = FindMember(key);
        return (v && v->IsArray()) ? v : nullptr;
    }
};

// Loads JSON data files for mod data replacement.
// Uses a minimal recursive-descent parser — suitable for game config files.
// For complex JSON, replace with nlohmann/json.
class JSONDataLoader {
public:
    // Parse a JSON file and return the root value.
    // Returns a Null value on failure.
    static JSONValue LoadFromFile(const char* path);
};

#endif // MOD_LOADER
